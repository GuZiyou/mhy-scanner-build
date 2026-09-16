/*
 * scan_test —— 独立扫码自测/测速工具（不依赖 Qt 界面）
 *
 * 用途：在同一套 QRScanner 逻辑上验证"2 号扫码方案"的效果与单帧耗时，
 *       不需要启动整个程序、也不需要抓直播流。
 *
 * 构建（在源码根目录，建议保持默认的 DEV=OFF，这样 decodeSingle 内部的
 * 日志会被 CMakeLists 定义的 TESTSPEED 关掉，不会污染计时）：
 *     cmake -B build -DSCAN_TEST=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
 *     cmake --build build --config Release --target scan_test
 *
 * 运行（注意把 exe 和 libzbar64-0.dll 放在同一目录）：
 *     scan_test ..\..\doc\image\hk4e_qrcode_1080.png ..\..\doc\image\zzz_qrcode_1440.png
 *
 * 常用参数：
 *     --mode fast|normal|full   扫描强度（默认 fast，与程序一致；
 *                               ZBar 认不出的图再用 normal / full 提高召回）
 *     --gray                    按灰度读图（模拟直播流 Y 平面直通的输入）
 *     --repeat N                每张图重复 N 次，输出单帧最小/平均耗时
 *
 * 提示：想量"扫不到"时的开销，就拿一张**没有二维码**的图来跑——
 *       直播/屏幕监看时这才是绝大多数帧的真实情况。
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "QRScanner.h"

namespace
{
const char* modeName(QRScanner::ScanMode mode)
{
    switch (mode)
    {
    case QRScanner::ScanMode::Fast:
        return "fast";
    case QRScanner::ScanMode::Normal:
        return "normal";
    default:
        return "full";
    }
}

bool parseMode(const std::string& text, QRScanner::ScanMode& mode)
{
    if (text == "fast")
    {
        mode = QRScanner::ScanMode::Fast;
        return true;
    }
    if (text == "normal")
    {
        mode = QRScanner::ScanMode::Normal;
        return true;
    }
    if (text == "full")
    {
        mode = QRScanner::ScanMode::Full;
        return true;
    }
    return false;
}

void usage()
{
    std::cout << "用法: scan_test [--mode fast|normal|full] [--gray] [--repeat N] <图片路径> [更多图片...]\n"
                 "示例: scan_test --mode fast --gray ..\\..\\doc\\image\\hk4e_qrcode_1080.png\n"
                 "      scan_test --repeat 20 ..\\..\\doc\\image\\*.png\n"
              << std::endl;
}

double elapsedMs(const std::chrono::high_resolution_clock::time_point& start,
                 const std::chrono::high_resolution_clock::time_point& end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}
} // namespace

int main(int argc, char** argv)
{
    QRScanner::ScanMode mode = QRScanner::ScanMode::Fast;   // 与程序默认一致
    int repeat = 1;
    int readFlags = cv::IMREAD_COLOR;
    std::vector<std::string> paths;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--mode")
        {
            if (i + 1 >= argc || !parseMode(argv[i + 1], mode))
            {
                std::cerr << "--mode 只支持 fast / normal / full" << std::endl;
                return 1;
            }
            ++i;
        }
        else if (arg == "--repeat")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--repeat 需要跟一个次数" << std::endl;
                return 1;
            }
            repeat = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--gray")
        {
            readFlags = cv::IMREAD_GRAYSCALE;
        }
        else if (arg == "--help" || arg == "-h")
        {
            usage();
            return 0;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "未知参数: " << arg << std::endl;
            usage();
            return 1;
        }
        else
        {
            paths.push_back(arg);
        }
    }

    if (paths.empty())
    {
        usage();
        return 1;
    }

    QRScanner scanner;
    scanner.setMode(mode);

    std::cout << "ZBar 运行库: "
              << (scanner.zbarAvailable() ? "已加载" : "未加载（仅使用 OpenCV / WeChat 引擎）")
              << "  模式=" << modeName(mode)
              << "  重复=" << repeat
              << "  读图=" << ((readFlags == cv::IMREAD_GRAYSCALE) ? "灰度" : "彩色")
              << std::endl;

    int failed = 0;
    for (const std::string& path : paths)
    {
        const cv::Mat image = cv::imread(path, readFlags);
        if (image.empty())
        {
            std::cout << "[跳过] 读取失败: " << path << std::endl;
            ++failed;
            continue;
        }

        std::string result;
        double best = 0;
        double total = 0;
        for (int r = 0; r < repeat; ++r)
        {
            std::string decoded;
            const auto start = std::chrono::high_resolution_clock::now();
            scanner.decodeSingle(image, decoded);
            const auto end = std::chrono::high_resolution_clock::now();

            const double milliseconds = elapsedMs(start, end);
            best = (r == 0) ? milliseconds : std::min(best, milliseconds);
            total += milliseconds;
            if (!decoded.empty())
            {
                result = decoded;
            }
        }

        const double average = total / repeat;
        if (result.empty())
        {
            std::cout << "[失败] " << path << "  单帧最小 " << best << " ms  平均 " << average << " ms"
                      << std::endl;
            ++failed;
        }
        else
        {
            std::cout << "[成功] " << path << "  引擎=" << scanner.lastEngine()
                      << "  单帧最小 " << best << " ms  平均 " << average << " ms"
                      << "  长度=" << result.size() << "\n        " << result << std::endl;
        }
    }

    std::cout << "完成：成功 " << (static_cast<int>(paths.size()) - failed) << " / 共 "
              << paths.size() << std::endl;
    return failed == 0 ? 0 : 2;
}
