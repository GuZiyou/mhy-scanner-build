#pragma once

#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/wechat_qrcode.hpp>

/*
 * 多引擎二维码扫描器 —— 把 2 号版本（KFC.v50）的扫码方案并入本工程
 *
 * 引擎优先级（命中即返回）：
 *   1) ZBar              —— 2 号版本实际使用的解码核心，运行时动态加载
 *                           libzbar64-0.dll / zbar-0.dll / zbar.dll
 *   2) OpenCV QRCodeDetector —— 第二兜底，无需任何模型文件
 *   3) WeChatQRCode      —— 原方案，需要 ./ScanModel/*.caffemodel，仅在模型存在时启用
 *
 * 每一帧在交给解码器之前会依次尝试多种预处理（灰度 → 放大 → Otsu → 自适应阈值
 * → CLAHE → 中心裁剪放大），命中即返回，因此单帧最坏耗时可控。
 *
 * 对外接口与原 QRScanner 完全一致，UI 侧（QRCodeForScreen / QRCodeForStream）
 * 无需任何改动；新增的 setMode() 是可选的速度开关。
 */
class QRScanner
{
public:
    /*
     * 扫描强度：越往后越"兜底"，单帧成本也越高。
     *
     *   Fast   —— 直播流/抢码专用。只跑 ZBar 的短阶梯（原图 / 反色 / Otsu /
     *             中心裁剪放大），不跑任何兜底引擎。单帧只有几毫秒，
     *             代价是放弃了"ZBar 全都认不出、但 DNN 能认出"的极小概率情形。
     *   Normal —— 默认。完整预处理阶梯 + OpenCV QRCodeDetector 兜底，
     *             适合屏幕扫描（图像干净、不需要抢时间）。
     *   Full   —— Normal 再挂上 WeChatQRCode（检测 CNN + 超分 CNN + zxing 复解），
     *             即上游原方案。**单帧可能上百毫秒，且只在"没扫到"时最贵**，
     *             除非确实需要，否则不要打开。
     */
    enum class ScanMode
    {
        Fast,
        Normal,
        Full
    };

    QRScanner();
    ~QRScanner();

    QRScanner(const QRScanner&) = delete;
    QRScanner& operator=(const QRScanner&) = delete;

    void decodeSingle(const cv::Mat& img, std::string& qrCode);
    void decodeMultiple(const cv::Mat& img, std::string& qrCode);

    /* 扫描强度（默认 Normal），可在每帧调用前随时切换 */
    void setMode(ScanMode mode) noexcept { m_mode = mode; }
    ScanMode mode() const noexcept { return m_mode; }

    /* 调试信息：本次命中使用的引擎名（zbar / opencv / wechat / none） */
    const std::string& lastEngine() const { return m_lastEngine; }

    /* ZBar 运行库是否加载成功 */
    bool zbarAvailable() const { return m_zbar.ready; }

private:
    /* ZBar 以动态加载方式使用，避免引入新的编译期依赖 */
    struct ZBarApi
    {
        bool ready{ false };
        std::string library;
        void* scanner{ nullptr };

        void* (*image_scanner_create)() = nullptr;
        void (*image_scanner_destroy)(void*) = nullptr;
        int (*image_scanner_set_config)(void*, int, int, int) = nullptr;
        int (*scan_image)(void*, void*) = nullptr;

        void* (*image_create)() = nullptr;
        void (*image_destroy)(void*) = nullptr;
        void (*image_ref)(void*, int) = nullptr;
        void (*image_set_format)(void*, unsigned long) = nullptr;
        void (*image_set_size)(void*, unsigned int, unsigned int) = nullptr;
        void (*image_set_data)(void*, const void*, unsigned long, void*) = nullptr;
        const void* (*image_get_symbols)(const void*) = nullptr;

        const void* (*symbol_set_first_symbol)(const void*) = nullptr;
        const void* (*symbol_next)(const void*) = nullptr;
        int (*symbol_get_type)(const void*) = nullptr;
        const char* (*symbol_get_data)(const void*) = nullptr;
        unsigned int (*symbol_get_data_length)(const void*) = nullptr;
    };

    bool initZBar();
    bool zbarDecodeGray(const cv::Mat& gray, std::string& out,
                        std::vector<std::string>* all) const;

    bool scanWithLadder(const cv::Mat& frame, std::string& out, std::vector<std::string>* all);
    /* 短阶梯（Fast）：直播流用 */
    bool ladderFast(const cv::Mat& gray, std::string& out, std::vector<std::string>* all) const;
    /* 完整阶梯（Normal / Full）：屏幕扫描用 */
    bool ladderFull(const cv::Mat& gray, std::string& out, std::vector<std::string>* all) const;

    bool scanWithOpenCV(const cv::Mat& frame, std::string& out);
    bool scanWithWeChat(const cv::Mat& frame, std::string& out);

    void ensureWeChat();

    ZBarApi m_zbar{};
    ScanMode m_mode{ ScanMode::Normal };
    std::string m_lastEngine{ "none" };
    std::vector<std::string> m_results;

    cv::QRCodeDetector m_cvDetector;

    bool m_wechatTried{ false };
    bool m_wechatReady{ false };
    cv::Ptr<cv::wechat_qrcode::WeChatQRCode> m_wechat;
};
