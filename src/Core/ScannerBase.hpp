#pragma once

#include <string>
#include <string_view>

#include "ApiDefs.hpp"

class ScannerBase
{
public:
    GameType gameType;
    std::string_view scanUrl{};
    std::string_view confirmUrl{};
    std::string lastTicket;
    std::string uid;
    std::string gameToken{};
    /* 新版扫码流程需要账号的米游社 mid（与 stoken 一起作为 Cookie），
       以及二维码 URL 里的 token_types（确认时要用同一个值） */
    std::string mid{};
    std::string lastPassportQrUrl{};   /* 游戏侧 scan 返回的 passport_qr_url，确认时要用它 */
    std::map<std::string_view, std::function<void()>> setGameType{
        { "8F3", [this]() {
             gameType = GameType::Honkai3;
             scanUrl = api::mhy::bh3::qrcode_scan;
             confirmUrl = api::mhy::bh3::qrcode_confirm;
         } },
        { "9E&", [this]() {
             gameType = GameType::Genshin;
             scanUrl = api::mhy::hk4e::qrcode_scan;
             confirmUrl = api::mhy::hk4e::qrcode_confirm;
         } },
        { "8F%", [this]() {
             gameType = GameType::HonkaiStarRail;
             scanUrl = api::mhy::hkrpg::qrcode_scan;
             confirmUrl = api::mhy::hkrpg::qrcode_confirm;
         } },
        { "%BA", [this]() {
             gameType = GameType::ZenlessZoneZero;
             scanUrl = api::mhy::nap::qrcode_scan;
             confirmUrl = api::mhy::nap::qrcode_confirm;
         } },
    };
};