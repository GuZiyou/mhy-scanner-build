#include "QRScanner.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
/* ------------------------------------------------------------------ *
 * ZBar 常量
 *
 * 这里不包含 zbar.h：ZBar 走运行时动态加载，因此只需要这些跨版本
 * ABI 稳定的枚举值（与 zbar.h 中的定义一致）。
 * ------------------------------------------------------------------ */
constexpr int ZBAR_NONE = 0;
constexpr int ZBAR_QRCODE = 64;

constexpr int ZBAR_CFG_ENABLE = 0;
constexpr int ZBAR_CFG_X_DENSITY = 11;
constexpr int ZBAR_CFG_Y_DENSITY = 12;

constexpr unsigned long fourcc(char a, char b, char c, char d)
{
    return static_cast<unsigned long>(static_cast<unsigned char>(a)) |
           (static_cast<unsigned long>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<unsigned long>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<unsigned long>(static_cast<unsigned char>(d)) << 24);
}

/* zbar_fourcc('Y','8','0','0') —— 灰度图格式 */
constexpr unsigned long ZBAR_FOURCC_Y800 = fourcc('Y', '8', '0', '0');

/* ------------------------------------------------------------------ *
 * 扫描分辨率上限
 *
 * ZBar 的耗时与像素数基本成正比（Y/X_DENSITY 都取 1 时，它会沿行、列
 * 各正反两个方向扫完整幅图，等于 4 遍全图）。所以"整帧多大"直接决定单帧成本。
 * ------------------------------------------------------------------ */

/* 完整阶梯（Normal / Full）用的上限：屏幕截图、图片自测 */
constexpr int MAX_SCAN_WIDTH = 1920;

/* Fast 阶梯（直播流）用的上限：直播间的登录二维码在这个分辨率下足够清晰，
   2 号版本本身也要求 ≥720p 才能扫描 */
constexpr int FAST_MAX_SCAN_WIDTH = 1280;

/* Fast 阶梯里，只有画面本来就很小（低码率/小窗口抓流）才值得全图放大：
   放大不会增加信息，只对"码在画面里占的像素太少"有意义 */
constexpr int FAST_UPSCALE_MAX_WIDTH = 800;

bool fileExists(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

/*
 * 取中心 num/den 区域后按 scale 放大。
 * 抢码场景二维码基本居中，这条路等价于提高局部分辨率，而开销只有全图放大的
 * 百分之几（放大不会增加信息，把整帧 1080p 放到 2880p 纯属浪费）。
 */
bool centerZoom(const cv::Mat& src, cv::Mat& dst, int num, int den, double scale)
{
    const int cropWidth = src.cols * num / den;
    const int cropHeight = src.rows * num / den;
    if (cropWidth < 100 || cropHeight < 100)
    {
        return false;
    }

    const cv::Rect roi((src.cols - cropWidth) / 2, (src.rows - cropHeight) / 2,
                       cropWidth, cropHeight);
    /* 送给 ZBar 的都是要被二值化的图，INTER_LINEAR 足够且比 INTER_CUBIC 快得多 */
    cv::resize(src(roi), dst, cv::Size(), scale, scale, cv::INTER_LINEAR);
    return !dst.empty();
}

#ifdef _WIN32
template <typename Fn>
bool loadSymbol(HMODULE module, const char* name, Fn& target)
{
    target = reinterpret_cast<Fn>(::GetProcAddress(module, name));
    return target != nullptr;
}
#endif
} // namespace

QRScanner::QRScanner()
{
    initZBar();

#ifndef TESTSPEED
    if (!m_zbar.ready)
    {
        std::cerr << "[QRScanner] 未加载到 ZBar 运行库（libzbar64-0.dll / zbar-0.dll），"
                     "将只使用 OpenCV / WeChat 引擎"
                  << std::endl;
    }
#endif
}

QRScanner::~QRScanner()
{
    if (m_zbar.ready && m_zbar.scanner != nullptr && m_zbar.image_scanner_destroy != nullptr)
    {
        m_zbar.image_scanner_destroy(m_zbar.scanner);
    }
    m_zbar.scanner = nullptr;
    m_zbar.ready = false;
}

/* ---------------------------------------------------------------------- *
 * ZBar
 * ---------------------------------------------------------------------- */
bool QRScanner::initZBar()
{
#ifdef _WIN32
    /* 与 2 号版本的部署方式一致：把 zbar 运行库放在 exe 同目录即可 */
    const char* candidates[] = {
        "libzbar64-0.dll",
        "zbar-0.dll",
        "libzbar.dll",
        "zbar.dll",
    };

    HMODULE module = nullptr;
    for (const char* name : candidates)
    {
        module = ::LoadLibraryA(name);
        if (module != nullptr)
        {
            m_zbar.library = name;
            break;
        }
    }
    if (module == nullptr)
    {
        return false;
    }

    const bool symbolsOk =
        loadSymbol(module, "zbar_image_scanner_create", m_zbar.image_scanner_create) &&
        loadSymbol(module, "zbar_image_scanner_destroy", m_zbar.image_scanner_destroy) &&
        loadSymbol(module, "zbar_image_scanner_set_config", m_zbar.image_scanner_set_config) &&
        loadSymbol(module, "zbar_scan_image", m_zbar.scan_image) &&
        loadSymbol(module, "zbar_image_create", m_zbar.image_create) &&
        loadSymbol(module, "zbar_image_set_format", m_zbar.image_set_format) &&
        loadSymbol(module, "zbar_image_set_size", m_zbar.image_set_size) &&
        loadSymbol(module, "zbar_image_set_data", m_zbar.image_set_data) &&
        loadSymbol(module, "zbar_image_get_symbols", m_zbar.image_get_symbols) &&
        loadSymbol(module, "zbar_symbol_set_first_symbol", m_zbar.symbol_set_first_symbol) &&
        loadSymbol(module, "zbar_symbol_next", m_zbar.symbol_next) &&
        loadSymbol(module, "zbar_symbol_get_type", m_zbar.symbol_get_type) &&
        loadSymbol(module, "zbar_symbol_get_data", m_zbar.symbol_get_data) &&
        loadSymbol(module, "zbar_symbol_get_data_length", m_zbar.symbol_get_data_length);

    /* 这两个是可选的，缺失时退化为不释放/不引用计数 */
    loadSymbol(module, "zbar_image_destroy", m_zbar.image_destroy);
    loadSymbol(module, "zbar_image_ref", m_zbar.image_ref);

    if (!symbolsOk)
    {
        return false;
    }

    m_zbar.scanner = m_zbar.image_scanner_create();
    if (m_zbar.scanner == nullptr)
    {
        return false;
    }

    /* 只保留二维码识别，并启用全分辨率扫描（小二维码必需） */
    m_zbar.image_scanner_set_config(m_zbar.scanner, ZBAR_NONE, ZBAR_CFG_ENABLE, 0);
    m_zbar.image_scanner_set_config(m_zbar.scanner, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
    /* 1 = 每一行/每一列都扫（ZBar 默认值）。调大可以省时间，代价是小码召回率，
       见 直播流直通改造.md 的调参一节 */
    m_zbar.image_scanner_set_config(m_zbar.scanner, ZBAR_NONE, ZBAR_CFG_X_DENSITY, 1);
    m_zbar.image_scanner_set_config(m_zbar.scanner, ZBAR_NONE, ZBAR_CFG_Y_DENSITY, 1);

    /*
     * 刻意【不】打开 ZBAR_CFG_TEST_INVERTED。
     *
     * ZBar 的实现是：每次 zbar_scan_image() 没找到符号时，就 _zbar_image_copy()
     * 复制一整幅图（并逐字节取反）再全图重扫一遍（见 zbar/img_scanner.c 的
     * zbar_scan_image）。它是"每次调用"级别的开关，而我们的阶梯一帧要调用它好几次，
     * 于是"没扫到"的帧要付出好几倍的复制 + 扫描开销。
     *
     * 反色二维码由阶梯自己在合适的位置用 cv::bitwise_not() 处理：整帧只反色一次，
     * 而且只付一次扫描的代价。
     */
    m_zbar.ready = true;
    return true;
#else
    return false;
#endif
}

bool QRScanner::zbarDecodeGray(const cv::Mat& gray, std::string& out,
                               std::vector<std::string>* all) const
{
    if (!m_zbar.ready || m_zbar.scanner == nullptr || gray.empty())
    {
        return false;
    }
    if (gray.type() != CV_8UC1)
    {
        return false;
    }

    /* zbar 不拷贝图像数据，缓冲区必须在扫描期间保持有效且连续 */
    const cv::Mat contiguous = gray.isContinuous() ? gray : gray.clone();

    void* image = m_zbar.image_create();
    if (image == nullptr)
    {
        return false;
    }

    m_zbar.image_set_format(image, ZBAR_FOURCC_Y800);
    m_zbar.image_set_size(image, static_cast<unsigned int>(contiguous.cols),
                          static_cast<unsigned int>(contiguous.rows));
    m_zbar.image_set_data(image, contiguous.data,
                          static_cast<unsigned long>(contiguous.cols) *
                              static_cast<unsigned long>(contiguous.rows),
                          nullptr);

    bool found = false;
    if (m_zbar.scan_image(m_zbar.scanner, image) > 0)
    {
        const void* symbols = m_zbar.image_get_symbols(image);
        for (const void* symbol = (symbols != nullptr) ? m_zbar.symbol_set_first_symbol(symbols) : nullptr;
             symbol != nullptr; symbol = m_zbar.symbol_next(symbol))
        {
            if (m_zbar.symbol_get_type(symbol) != ZBAR_QRCODE)
            {
                continue;
            }

            const char* data = m_zbar.symbol_get_data(symbol);
            const unsigned int length = m_zbar.symbol_get_data_length(symbol);
            if (data == nullptr || length == 0)
            {
                continue;
            }

            if (!found)
            {
                out.assign(data, length);
                found = true;
            }
            if (all != nullptr)
            {
                all->emplace_back(data, length);
            }
            if (all == nullptr)
            {
                break;
            }
        }
    }

    if (m_zbar.image_destroy != nullptr)
    {
        m_zbar.image_destroy(image);
    }
    else if (m_zbar.image_ref != nullptr)
    {
        m_zbar.image_ref(image, -1);
    }

    return found;
}

/* ---------------------------------------------------------------------- *
 * 对外接口
 * ---------------------------------------------------------------------- */
bool QRScanner::scanWithLadder(const cv::Mat& frame, std::string& out, std::vector<std::string>* all)
{
    if (frame.empty() || !m_zbar.ready)
    {
        return false;
    }

    cv::Mat gray;
    switch (frame.channels())
    {
    case 1:
        gray = frame;
        break;
    case 3:
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        break;
    case 4:
        cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
        break;
    default:
        return false;
    }
    if (gray.depth() != CV_8U)
    {
        gray.convertTo(gray, CV_8U);
    }

    return (m_mode == ScanMode::Fast) ? ladderFast(gray, out, all)
                                      : ladderFull(gray, out, all);
}

/*
 * 短阶梯：直播流/抢码专用。
 *
 * 设计原则：
 *   - 只保留"性价比最高"的几级，单帧总共 4~6 次 ZBar 调用，且都在 ≤1.3MP 的图上；
 *   - 不做 CLAHE、不在放大图上做自适应阈值（那两级在 1080p 上动辄几十毫秒）；
 *   - 反色由 bitwise_not 自己做，而不是让 ZBar 每调用一次就复制整图重扫。
 *
 * 实测（原实现同一帧"没码"时的开销）参见 直播流直通改造.md。
 */
bool QRScanner::ladderFast(const cv::Mat& gray, std::string& out, std::vector<std::string>* all) const
{
    cv::Mat base = gray;
    if (gray.cols > FAST_MAX_SCAN_WIDTH)
    {
        const double scale = static_cast<double>(FAST_MAX_SCAN_WIDTH) / gray.cols;
        cv::resize(gray, base, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    /* 1) 原图直解：命中率最高、最便宜 */
    if (zbarDecodeGray(base, out, all))
    {
        return true;
    }

    /* 2) 反色原图：深底浅码的反色二维码（整帧反色一次，比 TEST_INVERTED 便宜一个数量级） */
    cv::Mat inverted;
    cv::bitwise_not(base, inverted);
    if (zbarDecodeGray(inverted, out, all))
    {
        return true;
    }

    /* 3) Otsu 全局二值化：低对比度、浅色水印 */
    cv::Mat otsu;
    cv::threshold(base, otsu, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    if (zbarDecodeGray(otsu, out, all))
    {
        return true;
    }

    /* 4) 中心 3/5 裁剪 ×2：抢码场景二维码居中，等价于局部提高分辨率（比全图放大便宜得多） */
    cv::Mat zoomed;
    if (centerZoom(base, zoomed, 3, 5, 2.0))
    {
        if (zbarDecodeGray(zoomed, out, all))
        {
            return true;
        }
        cv::bitwise_not(zoomed, inverted);
        if (zbarDecodeGray(inverted, out, all))
        {
            return true;
        }
    }

    /* 5) 画面本来很小（小窗口/低码率抓流）才值得全图放大 */
    if (base.cols <= FAST_UPSCALE_MAX_WIDTH)
    {
        cv::Mat up;
        cv::resize(base, up, cv::Size(), 1.5, 1.5, cv::INTER_LINEAR);
        if (zbarDecodeGray(up, out, all))
        {
            return true;
        }
        cv::threshold(up, otsu, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        if (zbarDecodeGray(otsu, out, all))
        {
            return true;
        }
    }

    return false;
}

/*
 * 完整阶梯：屏幕扫描 / 图片自测用（图像干净，可以多花点时间换召回率）。
 *
 * 与短阶梯相比多了：全图放大、放大图上的自适应阈值、CLAHE。
 * 自适应阈值从 GAUSSIAN 改成 MEAN：块大小 41 的高斯版本在百万像素图上很贵，
 * 均值版本走盒滤波（积分图），快一个量级，而后面还有 ZBar 自己的二值化兜着。
 */
bool QRScanner::ladderFull(const cv::Mat& gray, std::string& out, std::vector<std::string>* all) const
{
    cv::Mat base = gray;
    if (gray.cols > MAX_SCAN_WIDTH)
    {
        const double scale = static_cast<double>(MAX_SCAN_WIDTH) / gray.cols;
        cv::resize(gray, base, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    /* 1) 原图直接解码：屏幕上/直播流里的二维码大多在这一步就命中 */
    if (zbarDecodeGray(base, out, all))
    {
        return true;
    }

    /* 2) Otsu 全局二值化：低对比度 */
    cv::Mat otsu;
    cv::threshold(base, otsu, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    if (zbarDecodeGray(otsu, out, all))
    {
        return true;
    }

    /* 3) 反色原图：反色二维码 */
    cv::Mat inverted;
    cv::bitwise_not(base, inverted);
    if (zbarDecodeGray(inverted, out, all))
    {
        return true;
    }

    /* 4) 放大后解码：应对画面里偏小的二维码 */
    cv::Mat up;
    {
        const double scale = (base.cols < 900) ? 2.0 : 1.5;
        cv::resize(base, up, cv::Size(), scale, scale, cv::INTER_LINEAR);
        if (zbarDecodeGray(up, out, all))
        {
            return true;
        }
    }

    /* 5) 放大 + 自适应阈值：抗背景渐变、水印、阴影 */
    {
        cv::Mat adaptive;
        cv::adaptiveThreshold(up, adaptive, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                              cv::THRESH_BINARY, 41, 7);
        if (zbarDecodeGray(adaptive, out, all))
        {
            return true;
        }
    }

    /* 6) CLAHE 增强对比度后再 Otsu：局部过暗/过亮 */
    {
        cv::Mat enhanced;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
        clahe->apply(base, enhanced);

        cv::threshold(enhanced, otsu, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        if (zbarDecodeGray(otsu, out, all))
        {
            return true;
        }
    }

    /* 7) 中心区域裁剪后放大：抢码场景二维码基本居中，等价于提高局部分辨率 */
    cv::Mat zoomed;
    if (centerZoom(base, zoomed, 3, 5, 2.0))
    {
        if (zbarDecodeGray(zoomed, out, all))
        {
            return true;
        }
        cv::bitwise_not(zoomed, inverted);
        if (zbarDecodeGray(inverted, out, all))
        {
            return true;
        }
    }

    return false;
}

/* ---------------------------------------------------------------------- *
 * OpenCV QRCodeDetector（第二兜底，无需模型文件）
 * ---------------------------------------------------------------------- */
bool QRScanner::scanWithOpenCV(const cv::Mat& frame, std::string& out)
{
    try
    {
        cv::Mat bgr;
        if (frame.channels() == 1)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        }
        else if (frame.channels() == 4)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        }
        else
        {
            bgr = frame;
        }

        std::string text = m_cvDetector.detectAndDecode(bgr);
        if (!text.empty())
        {
            out = std::move(text);
            return true;
        }

        cv::Mat zoomed;
        cv::resize(bgr, zoomed, cv::Size(), 2.0, 2.0, cv::INTER_LINEAR);
        text = m_cvDetector.detectAndDecode(zoomed);
        if (!text.empty())
        {
            out = std::move(text);
            return true;
        }
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[QRScanner] OpenCV 解码异常: " << e.what() << std::endl;
    }
    return false;
}

/* ---------------------------------------------------------------------- *
 * WeChatQRCode（原方案，需要 ./ScanModel；只在 ScanMode::Full 下使用）
 * ---------------------------------------------------------------------- */
void QRScanner::ensureWeChat()
{
    if (m_wechatTried)
    {
        return;
    }
    m_wechatTried = true;

    constexpr const char* DETECT_PROTOTXT_PATH = "./ScanModel/detect.prototxt";
    constexpr const char* DETECT_CAFFE_MODEL_PATH = "./ScanModel/detect.caffemodel";
    constexpr const char* SR_PROTOTXT_PATH = "./ScanModel/sr.prototxt";
    constexpr const char* SR_CAFFE_MODEL_PATH = "./ScanModel/sr.caffemodel";

    /* 缺少模型时直接跳过：原实现会在这里抛异常导致程序无法启动 */
    if (!fileExists(DETECT_PROTOTXT_PATH) || !fileExists(DETECT_CAFFE_MODEL_PATH))
    {
        std::cerr << "[QRScanner] 未找到 ScanModel，已跳过 WeChatQRCode 引擎" << std::endl;
        return;
    }

    try
    {
        m_wechat = cv::makePtr<cv::wechat_qrcode::WeChatQRCode>(
            DETECT_PROTOTXT_PATH, DETECT_CAFFE_MODEL_PATH, SR_PROTOTXT_PATH, SR_CAFFE_MODEL_PATH);
        m_wechat->setScaleFactor(0.4);
        m_wechatReady = true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[QRScanner] WeChatQRCode 初始化失败: " << e.what() << std::endl;
        m_wechatReady = false;
    }
}

bool QRScanner::scanWithWeChat(const cv::Mat& frame, std::string& out)
{
    ensureWeChat();
    if (!m_wechatReady || !m_wechat)
    {
        return false;
    }

    try
    {
        cv::Mat bgr;
        if (frame.channels() == 1)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        }
        else if (frame.channels() == 4)
        {
            cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        }
        else
        {
            bgr = frame;
        }

        const std::vector<std::string> decoded = m_wechat->detectAndDecode(bgr);
        if (!decoded.empty() && !decoded[0].empty())
        {
            out = decoded[0];
            return true;
        }
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[QRScanner] WeChatQRCode 解码异常: " << e.what() << std::endl;
    }
    return false;
}

/* ---------------------------------------------------------------------- *
 * 对外接口
 * ---------------------------------------------------------------------- */
void QRScanner::decodeSingle(const cv::Mat& img, std::string& qrCode)
{
#ifndef TESTSPEED
    const auto startTime = std::chrono::high_resolution_clock::now();
#endif

    std::string result;
    m_lastEngine = "none";

    if (scanWithLadder(img, result, nullptr))
    {
        m_lastEngine = "zbar";
    }
    else if (m_mode != ScanMode::Fast && scanWithOpenCV(img, result))
    {
        /* Fast 模式（直播流）刻意不做这一步：QRCodeDetector 在 1080p 上要十几毫秒，
           而抢码时"这一帧没中就等下一帧"比"多花十几毫秒去找"更划算 */
        m_lastEngine = "opencv";
    }
    else if (m_mode == ScanMode::Full && scanWithWeChat(img, result))
    {
        /* 上游的原方案：检测 CNN + 超分 CNN + zxing 复解。它在"没扫到"的帧上
           也要跑完整套推理，是本工程里最贵的一步，所以默认不启用 */
        m_lastEngine = "wechat";
    }

    if (!result.empty())
    {
        qrCode = result;
    }

#ifndef TESTSPEED
    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    std::cout << static_cast<float>(duration) / 1000000 << " decode(" << m_lastEngine
              << "): " << qrCode << std::endl;
#endif
}

void QRScanner::decodeMultiple(const cv::Mat& img, std::string& qrCode)
{
    m_results.clear();
    m_lastEngine = "none";

    std::string result;
    if (scanWithLadder(img, result, &m_results))
    {
        m_lastEngine = "zbar";
    }
    else if (m_mode != ScanMode::Fast && scanWithOpenCV(img, result))
    {
        m_lastEngine = "opencv";
        m_results.push_back(result);
    }
    else if (m_mode == ScanMode::Full && scanWithWeChat(img, result))
    {
        m_lastEngine = "wechat";
        m_results.push_back(result);
    }

    /* 与原实现保持一致：多结果时取最后一个 */
    for (const std::string& item : m_results)
    {
        qrCode = item;
    }
}
