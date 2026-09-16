#include "QRCodeForStream.h"

#include <iostream>
#include <string>
#include <string_view>

#include <opencv2/opencv.hpp>

#include "QRScanner.h"
#include "MhyApi.hpp"

/* ========================================================================= *
 * 直播流取帧改造（对齐 2 号版本 KFC.v50 的"快"）
 *
 * 原实现每一帧的固定开销：
 *     avcodec_receive_frame（软解）
 *       → sws_scale（全画幅 → BGR24）
 *       → cv::Mat 分配 + 写入（720p 约 2.7MB/帧）
 *       → 交给 QRScanner（内部再转一次灰度）
 *
 * 但扫码真正需要的只有亮度信息。改造后：
 *     D3D11VA 硬解 → av_hwframe_transfer_data 回拷 → 直接取 Y 平面当灰度图
 * 灰度直通（1 字节/像素）取代了 BGR 转换（3 字节/像素 + 色彩变换），
 * 同时把 H.264/H.265 解码交给 GPU。
 *
 * 任何一步失败（显卡/驱动不支持、非平面 YUV、10bit 等）都会自动回落到
 * 原来的 sws_scale 路径，行为与上游一致，不会因为硬解不可用而起不来。
 * ========================================================================= */
namespace
{
/* 是否为硬件解码帧（数据在 GPU 上，必须先回拷到 CPU 才能读像素） */
bool isHardwareFrame(const AVFrame* frame)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    return desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

/*
 * 把帧的亮度（Y）平面直接包成灰度 cv::Mat。
 *
 * YUV420P / NV12 / NV21 等 8bit 平面或半平面格式，data[0] + linesize[0] 就是 Y 平面，
 * 正好是 ZBar 需要的灰度数据（ZBar 用的 Y800 就是这个布局）。
 * 返回 false 表示该格式不能按平面取（packed RGB/YUYV、10bit P010 等），交调用方兜底。
 */
bool lumaToGray(const AVFrame* frame, cv::Mat& out)
{
    if (frame == nullptr || frame->data[0] == nullptr ||
        frame->width <= 0 || frame->height <= 0 || frame->linesize[0] < frame->width)
    {
        return false;
    }

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (desc == nullptr || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB)) != 0)
    {
        return false;
    }
    if (desc->comp[0].depth != 8 || desc->nb_components < 1)
    {
        return false;   /* 10bit/12bit（P010 等）亮度不是 1 字节 */
    }
    if (desc->nb_components > 1 && desc->comp[0].plane == desc->comp[1].plane)
    {
        return false;   /* packed YUV（YUYV422 等）行内交织，不能按平面取 */
    }

    /* linesize 一般大于 width（行尾对齐），clone() 逐行拷贝：
       既去掉填充、又保证连续（ZBar 要求连续缓冲），代价只有 1 字节/像素，而且是
       异步解码任务自己的内存，不受 FFmpeg 复用帧缓冲的影响 */
    const cv::Mat luma(frame->height, frame->width, CV_8UC1,
                       const_cast<uint8_t*>(frame->data[0]),
                       static_cast<size_t>(frame->linesize[0]));
    out = luma.clone();
    return !out.empty();
}

/*
 * 生成一帧送去 QRScanner 的图像：
 *   1) 硬解帧 → 回拷到 CPU 得到 CPU 帧
 *   2) 8bit 平面/半平面 YUV → 取 Y 平面（灰度直通，1 字节/像素）
 *   3) 其它格式（packed RGB、10bit P010 等）→ 保持上游的 sws_scale → BGR24 行为
 *
 * 注意第 3 步的转换上下文按"实际帧"的格式与尺寸重建（sws_getCachedContext）：
 * 硬解回拷后的 sw_format、以及个别流的解码输出，都可能与 codecpar 里的 pix_fmt 不同，
 * 用错格式的 SwsContext 会得到花屏而不是报错。
 */
bool buildScanImage(AVFrame* frame, AVFrame*& swFrame, SwsContext*& sws,
                    int scaledWidth, int scaledHeight, cv::Mat& out)
{
    AVFrame* cpuFrame = frame;

    if (isHardwareFrame(frame))
    {
        if (swFrame == nullptr)
        {
            swFrame = av_frame_alloc();
        }
        if (swFrame == nullptr)
        {
            return false;
        }
        if (av_hwframe_transfer_data(swFrame, frame, 0) < 0)
        {
            av_frame_unref(swFrame);

            /* 回拷失败只能丢这一帧：绝不能把 D3D11 纹理指针当像素数据用。
               持续失败说明硬解解码器与设备不匹配，日志提示一次便于定位 */
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                std::cerr << "[QRCodeForStream] 硬解帧回拷失败，已跳过该帧；"
                             "若持续出现请反馈（可临时关掉硬解）" << std::endl;
            }
            return false;
        }
        cpuFrame = swFrame;
    }

    if (lumaToGray(cpuFrame, out))
    {
        if (cpuFrame == swFrame)
        {
            av_frame_unref(swFrame);
        }
        return true;
    }

    sws = sws_getCachedContext(sws, cpuFrame->width, cpuFrame->height,
                               static_cast<AVPixelFormat>(cpuFrame->format),
                               scaledWidth, scaledHeight, AV_PIX_FMT_BGR24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws == nullptr)
    {
        if (cpuFrame == swFrame)
        {
            av_frame_unref(swFrame);
        }
        return false;
    }

    out.create(scaledHeight, scaledWidth, CV_8UC3);
    uint8_t* dstData[1] = { out.data };
    const int dstLinesize[1] = { static_cast<int>(out.step) };
    sws_scale(sws, cpuFrame->data, cpuFrame->linesize, 0, cpuFrame->height, dstData, dstLinesize);

    if (cpuFrame == swFrame)
    {
        av_frame_unref(swFrame);
    }
    return true;
}
} // namespace

QRCodeForStream::QRCodeForStream(QObject* parent) :
    QThread(parent),
    pAvdictionary(nullptr),
    pAVFormatContext(nullptr),
    pSwsContext(nullptr),
    pAVFrame(nullptr),
    pAVPacket(nullptr),
    pAVCodecContext(nullptr),
    m_stop(false),
    servertype(ServerType::Official)

{
    av_log_set_level(AV_LOG_FATAL);
    m_config = &(ConfigDate::getInstance());
}

QRCodeForStream::~QRCodeForStream()
{
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken)
{
    this->uid = uid;
    this->gameToken = gameToken;
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name)
{
    this->uid = uid;
    this->gameToken = gameToken;
    this->m_name = name;
}

void QRCodeForStream::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}

/* 硬解协商：优先 D3D11VA，解码器不支持时交回 FFmpeg 默认逻辑（软解） */
enum AVPixelFormat QRCodeForStream::getHwPixelFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts)
{
    for (const enum AVPixelFormat* p = fmts; p != nullptr && *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_D3D11)
        {
            return *p;
        }
    }
    return avcodec_default_get_format(ctx, fmts);
}

void QRCodeForStream::LoginOfficial()
{
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }
        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img;
            if (!buildScanImage(pAVFrame, pAVFrameSW, pSwsContext,
                                videoStreamWidth, videoStreamHeight, img))
            {
                av_frame_unref(pAVFrame);
                continue;
            }
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                /* 直播流走 Fast：只跑 ZBar 的短阶梯，不跑 OpenCV / WeChat 兜底。
                   抢码场景"这一帧没中就等下一帧"比"再多花几十毫秒找"更划算 */
                qrScanners.setMode(QRScanner::ScanMode::Fast);
                std::string str;
                qrScanners.decodeSingle(img, str);
                if (str.size() < 85)
                {
                    return;
                }
                std::string_view view(str.c_str() + 79, 3);
                if (!setGameType.contains(view))
                {
                    return;
                }
                const std::string_view ticket(str.data() + str.size() - 24, 24);
                setGameType[view]();
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ScanQRLogin(scanUrl.data(), ticket, gameType))
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(gameType, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ScanRet::FAILURE_1);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::LoginBH3BiliBili()
{
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }

        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img;
            if (!buildScanImage(pAVFrame, pAVFrameSW, pSwsContext,
                                videoStreamWidth, videoStreamHeight, img))
            {
                av_frame_unref(pAVFrame);
                continue;
            }
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                /* 直播流走 Fast：只跑 ZBar 的短阶梯，不跑 OpenCV / WeChat 兜底。
                   抢码场景"这一帧没中就等下一帧"比"再多花几十毫秒找"更划算 */
                qrScanners.setMode(QRScanner::ScanMode::Fast);
                std::string str;
                qrScanners.decodeSingle(img, str);
                if (str.size() < 85)
                {
                    return;
                }
                if (std::string_view view(str.c_str() + 79, 3); view != "8F3")
                {
                    return;
                }
                const std::string& ticket = str.substr(str.length() - 24);
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(GameType::Honkai3_BiliBili, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ret);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::setStreamHW()
{
    if (pAVCodecContext->width < pAVCodecContext->height ||
        pAVCodecContext->height == 480 ||
        pAVCodecContext->height == 720)
    {
        videoStreamWidth = pAVCodecContext->width;
        videoStreamHeight = pAVCodecContext->height;
    }
    else
    {
        videoStreamWidth = pAVCodecContext->width / 1.5;
        videoStreamHeight = pAVCodecContext->height / 1.5;
    }
}

void QRCodeForStream::stop()
{
    m_stop.store(false);
}

void QRCodeForStream::setUrl(const std::string& url, const std::map<std::string, std::string> heard)
{
    streamUrl = url;
    for (const auto& it : heard)
    {
        av_dict_set(&pAvdictionary, it.first.c_str(), it.second.c_str(), 0);
    }
    av_dict_set(&pAvdictionary, "max_delay", "0", 0);
    av_dict_set(&pAvdictionary, "probesize", "1024", 0);
    av_dict_set(&pAvdictionary, "packetsize", "128", 0);
    av_dict_set(&pAvdictionary, "rtbufsize", "0", 0);
    av_dict_set(&pAvdictionary, "delay", "0", 0);
    av_dict_set(&pAvdictionary, "buffer_size", "1000", 0);
}

auto QRCodeForStream::init() -> bool
{
    pAVFormatContext = avformat_alloc_context();
    if (avformat_open_input(&pAVFormatContext, streamUrl.c_str(), NULL, &pAvdictionary) != 0)
    {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(pAVFormatContext, NULL) < 0)
    {
        std::cerr << "Error finding stream information" << std::endl;
        return false;
    }
    AVStream* videoStream = nullptr;
    for (int i = 0; i < pAVFormatContext->nb_streams; i++)
    {
        if (pAVFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = pAVFormatContext->streams[i];
            break;
        }
    }
    if (videoStream == nullptr)
    {
        std::cerr << "No video stream found" << std::endl;
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        std::cerr << "Codec not found" << std::endl;
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);

    /* ---- 尝试 D3D11VA 硬解；任何一步失败都保持原来的软解路径 ---- */
    if (av_hwdevice_ctx_create(&pHwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0)
    {
        pAVCodecContext->hw_device_ctx = av_buffer_ref(pHwDeviceCtx);
        if (pAVCodecContext->hw_device_ctx != nullptr)
        {
            pAVCodecContext->get_format = getHwPixelFormat;
            hwDecodeRequested = true;
        }
    }
    if (!hwDecodeRequested && pHwDeviceCtx != nullptr)
    {
        av_buffer_unref(&pHwDeviceCtx);
    }

    if (avcodec_open2(pAVCodecContext, decoder, NULL) < 0)
    {
        std::cerr << "Error opening codec" << std::endl;
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    return true;
}

void QRCodeForStream::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = ConfirmQRLogin(confirmUrl, uid, gameToken, lastTicket, gameType);
        if (b)
        {
            Q_EMIT loginResults(ScanRet::SUCCESS);
        }
        else
        {
            Q_EMIT loginResults(ScanRet::FAILURE_2);
        }
    }
    break;
    case BH3_BiliBili:
    {
        ret = scanConfirm(lastTicket, uid, gameToken, m_name);
        Q_EMIT loginResults(ret);
    }
    break;
    default:
        break;
    }
}

void QRCodeForStream::run()
{
    threadPool.setMaxThreadCount(threadNumber);
    m_stop.store(true);
    ret = ScanRet::UNKNOW;
    //TODO 获取直播流地址放在这里
    if (init())
    {
        std::cout << "[QRCodeForStream] 取帧方式: "
                  << (hwDecodeRequested ? "D3D11VA 硬解 + Y 平面直通（灰度）"
                                        : "软件解码 + Y 平面直通（灰度）")
                  << std::endl;
#ifndef SHOW
        cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
        cv::resizeWindow("Video_Stream", videoStreamWidth / 2, videoStreamHeight / 2);
#endif
        switch (servertype)
        {
            using enum ServerType;
        case Official:
            LoginOfficial();
            break;
        case BH3_BiliBili:
            LoginBH3BiliBili();
            break;
        default:
            break;
        }
    }
    if (ret == ScanRet::LIVESTOP)
    {
        emit loginResults(ret);
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
    avformat_close_input(&pAVFormatContext);
    avcodec_free_context(&pAVCodecContext);
    av_buffer_unref(&pHwDeviceCtx);
    av_frame_free(&pAVFrameSW);
    sws_freeContext(pSwsContext);
    av_dict_free(&pAvdictionary);
    av_frame_free(&pAVFrame);
    av_packet_free(&pAVPacket);
    pAVFormatContext = nullptr;
    pAVCodecContext = nullptr;
    pSwsContext = nullptr;
    pAvdictionary = nullptr;
    pAVFrame = nullptr;
    pAVPacket = nullptr;
}
