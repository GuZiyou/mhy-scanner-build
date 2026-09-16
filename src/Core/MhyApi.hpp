#pragma once

#include <string>
#include <string_view>
#include <format>
#include <random>
#include <sstream>
#include <optional>
#include <iostream>
#include <fstream>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include "ApiDefs.hpp"
#include "CreateUUID.hpp"
#include "CryptoKit.h"
#include "UtilString.hpp"
#include "TimeStamp.hpp"

static const std::string device_id{ CreateUUID::CreateUUID4() };
static GameType loginType{ GameType::TearsOfThemis };

[[nodiscard]] inline std::string DataSignAlgorithmVersionGen1()
{
    return "";
}

[[nodiscard]] inline std::string DataSignAlgorithmVersionGen2(const std::string_view body, const std::string_view query)
{
    const std::string time_now{ std::to_string(GetUnixTimeStampSeconds()) };
    std::random_device rd{};
    std::mt19937 gen{ rd() };
    int lower_bound{ 100001 };
    int upper_bound{ 200000 };
    std::uniform_int_distribution<int> dist(lower_bound, upper_bound);
    const std::string rand{ std::to_string(dist(gen)) };
    std::string m{ "salt=" + std::string(mihoyobbs_salt_x6) + "&t=" + time_now + "&r=" + rand + "&b=" + std::string(body) + "&q=" + std::string(query) };
    return time_now + "," + rand + "," + Md5(m);
}

inline std::string Encrypt(const std::string_view source)
{
    static constinit const char* PublicKey{
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDDvekdPMHN3AYhm/vktJT+YJr7"
        "cI5DcsNKqdsx5DZX0gDuWFuIjzdwButrIYPNmRJ1G8ybDIF7oDW2eEpm5sMbL9zs"
        "9ExXCdvqrn51qELbqj0XxtMTIpaCHFSI50PfPpTFV9Xt/hmyVwokoOXFlAEgCn+Q"
        "CgGs52bFoYMtyi+xEQIDAQAB\n"
        "-----END PUBLIC KEY-----"
    };
    return rsaEncrypt(source.data(), PublicKey);
}

inline cpr::Header GetRequestHeader()
{
    static cpr::Header headers{
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) miHoYoBBS/2.76.1" },
        { "Accept", "application/json" },
        { "x-rpc-app_id", "bll8iq97cem8" },
        { "x-rpc-app_version", "2.76.1" },
        { "x-rpc-client_type", "2" },
        { "x-rpc-device_id", device_id },
        { "x-rpc-device_name", "" },
        { "x-rpc-game_biz", "bbs_cn" },
        { "x-rpc-sdk_version", "2.16.0" }
    };
    return headers;
}

/* 诊断用：把扫码链路上每个请求的 URL / 请求体 / 原始响应追加写到
   ./Config/api_debug.log，便于在真机上定位问题（不影响正常流程）。
   必须放在文件前部：后面多处（含 QueryQRLoginStatus）都会用到它。 */
inline void LogScanDebug(const std::string_view tag, const std::string_view url,
                         const std::string_view body, const std::string_view resp)
{
    std::ofstream file{ "./Config/api_debug.log", std::ios::app };
    if (!file)
    {
        return;
    }
    file << "----- " << tag << " -----\n"
         << "url : " << url << "\n"
         << "body: " << body << "\n"
         << "resp: " << resp << "\n";
}

/* ---------------------------------------------------------------------- *
 * 新的扫码登录（passport-api …/ma-cn-passport/app/ 系列）
 *
 * 旧的 hk4e-sdk / api-sdk combo-panda 那套已经失效（见 ApiDefs.hpp 里的说明）。
 * 这一套只需要三个请求头，不需要 DS 签名、不需要 Cookie。
 * ---------------------------------------------------------------------- */

inline cpr::Header GetPassportAppHeader()
{
    return cpr::Header{
        { "Content-Type", "application/json" },
        { "x-rpc-app_id", std::string(passport_app_id) },
        { "x-rpc-device_id", device_id }
    };
}

/* 创建登录二维码，返回 { retcode, url, ticket }。
   url 是二维码内容（user.mihoyo.com 登录页），ticket 用来轮询状态。 */
inline std::tuple<int, std::string, std::string> CreateQRLogin()
{
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::app_create_qr_login },
        cpr::Body{ "{}" },
        GetPassportAppHeader());

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0)
    {
        return { j.is_discarded() ? -1 : j.value("retcode", -1), {}, {} };
    }

    return { 0,
             j["data"]["url"].get<std::string>(),
             j["data"]["ticket"].get<std::string>() };
}

/* 查询二维码状态，返回 { state, token, aid, mid, name }。
   Confirmed 时 tokens[0].token 就是登录凭证（token_type=1，stoken v2），
   user_info 里给出 mid / aid / account_name，正好是账号记录需要的字段。 */
inline std::tuple<LoginQRCodeState, std::string, std::string, std::string, std::string>
QueryQRLoginStatus(const std::string_view ticket)
{
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::app_query_qr_login_status },
        cpr::Body{ nlohmann::json{ { "ticket", ticket } }.dump() },
        GetPassportAppHeader());

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0)
    {
        return { LoginQRCodeState::Expired, {}, {}, {}, {} };
    }

    const auto& data = j["data"];
    const std::string status{ data.value("status", "") };
    if (status == "Created")
    {
        return { LoginQRCodeState::Init, {}, {}, {}, {} };
    }
    if (status == "Scanned")
    {
        return { LoginQRCodeState::Scanned, {}, {}, {}, {} };
    }
    if (status != "Confirmed")
    {
        return { LoginQRCodeState::Expired, {}, {}, {}, {} };
    }

    std::string token{};
    std::string types{};
    if (data.contains("tokens") && data["tokens"].is_array() && !data["tokens"].empty())
    {
        token = data["tokens"][0].value("token", "");
        for (const auto& t : data["tokens"])
        {
            types += std::format("{}:len{} ", t.value("token_type", -1), t.value("token", std::string{}).size());
        }
    }

    std::string mid{}, aid{}, name{};
    if (data.contains("user_info") && data["user_info"].is_object())
    {
        const auto& info = data["user_info"];
        mid = info.value("mid", "");
        aid = info.value("aid", "");
        name = info.value("account_name", "");
    }

    /* 诊断：只记录 token 类型与长度、以及 aid/mid 长度，不记录任何凭据值 */
    LogScanDebug("qrConfirmed", "tokens/user_info",
                 std::format("tokens=[{}] aid_len={} mid_len={} name_len={}", types, aid.size(), mid.size(), name.size()),
                 "n/a");

    return { LoginQRCodeState::Confirmed, token, aid, mid, name };
}

inline std::string GetLoginQrcodeUrl(const GameType type = loginType)
{
    auto res = cpr::Post(
        cpr::Url{ api::mhy::hk4e::qrcode_fetch },
        cpr::Body{ nlohmann::json{
            { "app_id", static_cast<int>(type) },
            { "device", device_id } }
                       .dump() },
        cpr::Header{ { "Content-Type", "application/json" } });

    auto data = nlohmann::json::parse(res.text);
    std::string qrcodeUrl = data["data"]["url"].get<std::string>();
    return qrcodeUrl;
}

inline std::tuple<LoginQRCodeState, std::string, std::string> GetQRCodeState(
    const std::string_view ticket,
    const GameType type = loginType)
{
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::hk4e::qrcode_query },
        cpr::Body{ nlohmann::json{
            { "app_id", static_cast<int>(type) },
            { "device", device_id },
            { "ticket", ticket } }
                       .dump() },
        cpr::Header{ { "Content-Type", "application/json" } });

    const auto data = nlohmann::json::parse(response.text);

    if (data.value("retcode", -1) != 0)
        return { LoginQRCodeState::Expired, {}, {} };

    static const std::unordered_map<std::string, LoginQRCodeState> stateMap{
        { "Init", LoginQRCodeState::Init },
        { "Scanned", LoginQRCodeState::Scanned },
        { "Confirmed", LoginQRCodeState::Confirmed },
    };

    const auto stat = data["data"]["stat"].get<std::string>();
    const auto it = stateMap.find(stat);

    if (it == stateMap.end())
        return { LoginQRCodeState::Expired, {}, {} };

    if (it->second == LoginQRCodeState::Confirmed)
    {
        const auto payload = nlohmann::json::parse(
            data["data"]["payload"]["raw"].get<std::string>());
        return { LoginQRCodeState::Confirmed,
                 payload["uid"].get<std::string>(),
                 payload["token"].get<std::string>() };
    }

    return { it->second, {}, {} };
}

inline std::string getMysUserName(const std::string_view uid)
{
    static constexpr std::string_view url = api::mhy::mys::userinfo;
    const auto response = cpr::Get(
        cpr::Url{ std::format("{}?uid={}", url, uid) });

    const auto data = nlohmann::json::parse(response.text);
    return data["data"]["user_info"]["nickname"].get<std::string>();
}

inline std::tuple<int, std::string, std::string> GetStokenByGameToken(
    const std::string_view uid,
    const std::string_view game_token)
{
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::takumi::game_token_stoken },
        cpr::Body{ nlohmann::json{ { "account_id", std::stoi(uid.data()) }, { "game_token", game_token } }.dump() },
        GetRequestHeader());

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);

    if (retcode != 0)
        return { retcode, {}, {} };

    return { 0,
             j["data"]["user_info"]["mid"].get<std::string>(),
             j["data"]["token"]["token"].get<std::string>() };
}

inline std::tuple<int, std::string> GetGameTokenByStoken(
    const std::string_view stoken,
    const std::string_view mid)
{
    const auto response = cpr::Get(
        cpr::Url{ api::mhy::takumi::game_token },
        cpr::Parameters{
            { "stoken", stoken.data() },
            { "mid", mid.data() } });

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);

    if (retcode != 0)
        return { retcode, {} };

    return { 0, j["data"]["game_token"].get<std::string>() };
}

inline std::tuple<int, GeetestData> CreateLoginCaptcha(
    const std::string_view mobile,
    const std::string_view aigis = "")
{
    const std::string body{ nlohmann::json{
        { "area_code", Encrypt("+86") },
        { "mobile", Encrypt(mobile) } }
                                .dump() };
    cpr::Header reqHeaders{ GetRequestHeader() };
    reqHeaders["DS"] = DataSignAlgorithmVersionGen2(body, "");
    if (!aigis.empty())
        reqHeaders["X-Rpc-Aigis"] = aigis;
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::login_by_mobile_captcha },
        cpr::Body{ body },
        cpr::Header{ reqHeaders });

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);
    GeetestData result{};
    if (retcode == 0)
    {
        result.action_type = j["data"]["action_type"].get<std::string>();
        return { retcode, result };
    }
    if (retcode == -3101)
    {
        const auto it = response.header.find("X-Rpc-Aigis");
        if (it != response.header.end())
        {
            const auto aigisJson = nlohmann::json::parse(it->second);
            const auto captchaJson = nlohmann::json::parse(aigisJson["data"].get<std::string>());

            result.session_id = aigisJson["session_id"].get<std::string>();
            result.mmt_type = aigisJson["mmt_type"].get<int>();
            result.gt = captchaJson["gt"].get<std::string>();
            result.challenge = captchaJson["challenge"].get<std::string>();
            result.GeeTestType = ServerType::Official;
        }
    }
    return { retcode, result };
}

inline auto LoginByMobileCaptcha(const std::string_view actionType, const std::string_view mobile, const std::string_view captcha, const std::string_view aigis = "")
{
    struct
    {
        int retcode{};
        struct
        {
            std::string V2Token{};
            std::string aid{};
            std::string mid{};
        } data;
    } result;
#if 0
	const std::string RequestBody{ std::format(R"({{"area_code":"{}","action_type":"{}","captcha":"{}","mobile":"{}"}})", Encrypt("+86"), actionType, captcha, Encrypt(mobile)) };
    std::map<std::string, std::string> headers{ GetRequestHeader() };
    headers["DS"] = DataSignAlgorithmVersionGen2(RequestBody, "");
    if (!aigis.empty())
    {
        headers["X-Rpc-Aigis"] = aigis;
    }
    HttpClient h;
    std::string s;
    h.PostRequest(s, URL_LoginByMobileCaptcha, RequestBody, headers);
    //std::cout << s << std::endl;
    json::Json j{};
    j.parse(s);
    result.retcode = j["retcode"];
    if (result.retcode == -3205)
    {
        return result;
    }
    else if (result.retcode == 0)
    {
        result.data.V2Token = j["data"]["token"]["token"];
        result.data.aid = j["data"]["user_info"]["aid"];
        result.data.mid = j["data"]["user_info"]["mid"];
    }
#endif
    return result;
}

/* ---------------------------------------------------------------------- *
 * 扫码确认登录（监视屏幕 / 监视直播间）
 *
 * 新版流程（1.16.0 起）：
 *   1. 从扫到的二维码 URL 里解析出 tk 与 token_types；
 *   2. 带上账号 Cookie（stoken=...; mid=...）调 passport 的
 *      /account/ma-cn-passport/app/scanQRLogin 标记已扫码；
 *   3. 再调 /app/confirmQRLogin 确认登录。
 *
 * 旧的一套（各游戏 api-sdk 的 combo/panda/qrcode/scan + confirm，以及
 * api-takumi 的 getGameToken 换 token）已经失效 —— 这正是"一点击监视屏幕/
 * 直播间就弹登录状态失效"的原因。新版不再需要 game_token 中转，直接用
 * 账号自己的 stoken/mid 作为 Cookie。
 * ---------------------------------------------------------------------- */

/* 从扫码得到的 URL 里取出 tk 与 token_types（旧格式回退到 ticket=） */
inline std::pair<std::string, std::string> ParseQrTicket(const std::string_view url)
{
    std::string ticket{};
    std::string tokenTypes{};

    auto takeParam = [&url](const std::string_view name) -> std::string {
        const std::string pattern{ std::string(name) + "=" };
        const size_t pos = url.find(pattern);
        if (pos == std::string_view::npos)
        {
            return {};
        }
        const size_t begin = pos + pattern.size();
        size_t end = url.find_first_of("&#", begin);
        if (end == std::string_view::npos)
        {
            end = url.size();
        }
        return std::string{ url.substr(begin, end - begin) };
    };

    ticket = takeParam("tk");
    if (ticket.empty())
    {
        ticket = takeParam("ticket");
    }
    tokenTypes = takeParam("token_types");

    return { ticket, tokenTypes };
}

/* 用账号的 stoken 校验登录状态是否仍然有效（取代已失效的 getGameToken 中转） */
inline bool CheckStokenValid(const std::string_view stoken, const std::string_view mid)
{
    const auto response = cpr::Get(
        cpr::Url{ api::mhy::takumi::cookie_account_info_by_stoken },
        cpr::Parameters{ { "stoken", stoken.data() }, { "mid", mid.data() } },
        GetRequestHeader());

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

/* 诊断用：把扫码链路上每个请求的 URL / 请求体 / 原始响应追加写到
   ./Config/api_debug.log，便于在真机上定位问题（不影响正常流程）。 */
inline void LogScanDebugOldRemoved()
{
}

/* 诊断：确认失败时，把常见的凭证/请求头组合各试一遍并写日志。
   仅用于定位（-502 究竟是 Cookie 字段、拼法还是 app_id/client_type 的问题）。 */
inline void DiagnoseConfirmQRLogin(const std::string_view passportQrUrl,
                                   const std::string_view stoken, const std::string_view mid)
{
    const auto [ticket, tokenTypes] = ParseQrTicket(passportQrUrl);
    if (ticket.empty())
    {
        return;
    }
    const std::string tk{ ticket };
    const std::string tt{ tokenTypes.empty() ? std::string{ "1" } : tokenTypes };

    struct Variant
    {
        const char* tag;
        const char* endpoint;
        const char* appId;
        std::string cookie;
        std::string body;
        const char* clientType;
    };

    const std::string bodyStr{ std::format(R"({{"ticket":"{}","token_types":"{}"}})", tk, tt) };
    const std::string bodyNum{ std::format(R"({{"ticket":"{}","token_types":{}}})", tk, tt) };
    const std::string bodyOnly{ std::format(R"({{"ticket":"{}"}})", tk) };
    const std::string st{ stoken };
    const std::string md{ mid };

    /* 直接用字面量，避免 compile_string 常量到 const char* 的转换问题 */
    constexpr const char* scanEp = "https://passport-api.mihoyo.com/account/ma-cn-passport/app/scanQRLogin";
    constexpr const char* confirmEp = "https://passport-api.mihoyo.com/account/ma-cn-passport/app/confirmQRLogin";
    constexpr const char* appWeb = "bll8iq97cem8";
    constexpr const char* appDw = "dw9y09jqjpxc";

    const std::vector<Variant> variants{
        { "d1 scan   bll8 cookie(;mid)      ", scanEp, appWeb,
          std::format("stoken={};mid={}", st, md), bodyStr, nullptr },
        { "d2 confirm bll8 cookie(;mid)      ", confirmEp, appWeb,
          std::format("stoken={};mid={}", st, md), bodyStr, nullptr },
        { "d3 confirm bll8 cookie(; mid 空格)", confirmEp, appWeb,
          std::format("stoken={}; mid={}", st, md), bodyStr, nullptr },
        { "d4 confirm bll8 stoken_v2 字段     ", confirmEp, appWeb,
          std::format("stoken_v2={};mid={}", st, md), bodyStr, nullptr },
        { "d5 confirm bll8 account_id+stoken  ", confirmEp, appWeb,
          std::format("account_id={};stoken={};mid={}", md, st, md), bodyStr, nullptr },
        { "d6 confirm bll8 ltoken/ltuid        ", confirmEp, appWeb,
          std::format("ltoken={};ltuid={}", st, md), bodyStr, nullptr },
        { "d7 confirm dw9y cookie(;mid)        ", confirmEp, appDw,
          std::format("stoken={};mid={}", st, md), bodyStr, nullptr },
        { "d8 confirm bll8 client_type=3       ", confirmEp, appWeb,
          std::format("stoken={};mid={}", st, md), bodyStr, "3" },
        { "d9 confirm bll8 token_types 数字    ", confirmEp, appWeb,
          std::format("stoken={};mid={}", st, md), bodyNum, nullptr },
        { "d10 confirm bll8 只发 ticket        ", confirmEp, appWeb,
          std::format("stoken={};mid={}", st, md), bodyOnly, nullptr },
        { "d11 confirm bll8 无 Cookie          ", confirmEp, appWeb,
          std::string{}, bodyStr, nullptr },
    };

    for (const auto& v : variants)
    {
        cpr::Header header{
            { "Content-Type", "application/json" },
            { "x-rpc-app_id", v.appId },
            { "x-rpc-device_id", device_id }
        };
        if (!v.cookie.empty())
        {
            header["Cookie"] = v.cookie;
        }
        if (v.clientType != nullptr)
        {
            header["x-rpc-client_type"] = v.clientType;
        }
        const auto response = cpr::Post(cpr::Url{ v.endpoint }, cpr::Body{ v.body }, header);
        LogScanDebug(v.tag, v.endpoint, v.body, response.text);
    }
}

/* 诊断：用账号的真实 stoken/mid 探测两条换取 game token 的路径，
   只记录 retcode/message 与长度，不记录凭据值。 */
inline void DiagnoseStoken(const std::string_view stoken, const std::string_view mid,
                           const std::string_view uid)
{
    const std::string lenInfo{ std::format("stoken_len={} mid_len={} uid_len={}", stoken.size(), mid.size(), uid.size()) };

    const auto r1 = cpr::Get(cpr::Url{ api::mhy::takumi::cookie_account_info_by_stoken },
                             cpr::Parameters{ { "stoken", stoken.data() }, { "mid", mid.data() } },
                             GetRequestHeader());
    LogScanDebug("CheckStoken(getCookieAccountInfoBySToken)", "takumi", lenInfo, r1.text);

    const auto r2 = cpr::Get(cpr::Url{ api::mhy::takumi::game_token },
                             cpr::Parameters{ { "stoken", stoken.data() }, { "mid", mid.data() } },
                             GetRequestHeader());
    LogScanDebug("GetGameToken(旧中转)", "takumi", lenInfo, r2.text);

    for (const char* biz : { "hk4e_cn", "hkrpg_cn", "nap_cn", "bh3_cn" })
    {
        const std::string ticketUrl{ std::format(
            "https://passport-api.mihoyo.com/account/ma-cn-verifier/app/createAuthTicketByGameBiz"
            "?game_biz={}&stoken={}&uid={}&mid={}", biz, stoken, uid, mid) };
        const auto r3 = cpr::Post(
            cpr::Url{ ticketUrl },
            cpr::Header{ { "x-rpc-client_type", "3" }, { "x-rpc-app_id", "ddxf5dufpuyo" }, { "x-rpc-device_id", device_id } });
        LogScanDebug(biz, "passport/createAuthTicketByGameBiz", lenInfo, r3.text);
    }
}

/* 新接口共用：Cookie 里带账号的 stoken/mid，头里带 web 的 app id */
inline cpr::Header GetScanConfirmHeader(const std::string_view stoken, const std::string_view mid)
{
    return cpr::Header{
        { "Content-Type", "application/json" },
        { "x-rpc-app_id", "bll8iq97cem8" },
        { "x-rpc-device_id", device_id },
        { "Cookie", std::format("stoken={};mid={}", stoken, mid) }
    };
}

inline bool ScanQRLogin(const std::string_view passportQrUrl, const std::string_view stoken,
                        const std::string_view mid)
{
    const auto [ticket, tokenTypes] = ParseQrTicket(passportQrUrl);
    if (ticket.empty())
    {
        return false;
    }

    const std::string body{ nlohmann::json{
        { "ticket", ticket },
        { "token_types", tokenTypes } }
                                .dump() };
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::app_scan_qr_login },
        cpr::Body{ body },
        GetScanConfirmHeader(stoken, mid));

    LogScanDebug("scanQRLogin", "passport/app/scanQRLogin", body, response.text);
    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

inline bool ConfirmQRLogin(const std::string_view passportQrUrl, const std::string_view stoken,
                           const std::string_view mid)
{
    const auto [ticket, tokenTypes] = ParseQrTicket(passportQrUrl);
    if (ticket.empty())
    {
        return false;
    }

    const std::string body{ nlohmann::json{
        { "ticket", ticket },
        { "token_types", tokenTypes } }
                                .dump() };
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::app_confirm_qr_login },
        cpr::Body{ body },
        GetScanConfirmHeader(stoken, mid));

    LogScanDebug("confirmQRLogin", "passport/app/confirmQRLogin", body, response.text);
    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

/* ★ 真正的最后一步：各游戏 combo/panda/qrcode/confirm（实测可用）。
   请求体保留旧格式 payload{proto:"Account", raw:"{uid, token}"}，
   其中 token 现在直接放账号的 stoken（access_key）—— 因为
   api-takumi 的 getGameToken 中转已失效，1 号也已删除该步骤。
   （实测：只发 ticket 或带 passport_app_id/ts/passport_qr_url 都返回
     -102 系统错误；只有带 payload 才返回 retcode 0。） */
inline bool PandaConfirmQRLogin(const std::string_view url, const std::string_view uid,
                                const std::string_view token, const std::string_view ticket,
                                GameType gameType)
{
    const std::string body{ nlohmann::json{
        { "app_id", static_cast<int>(gameType) },
        { "device", device_id },
        { "ticket", ticket },
        { "payload", { { "proto", "Account" }, { "raw", nlohmann::json{ { "uid", uid }, { "token", token } }.dump() } } } }
                            .dump() };

    const auto response = cpr::Post(
        cpr::Url{ url },
        cpr::Body{ body },
        cpr::Header{ { "Content-Type", "application/json" } });

    LogScanDebug("pandaConfirm", std::string(url), body, response.text);
    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

/* 扫码流程的第一步：仍然要调游戏侧的 combo/panda scan，但必须带上
   passport_app_id 与 ts —— 响应里的 data.passport_qr_url 才是后续
   passport scanQRLogin / confirmQRLogin 要用的二维码地址。
   （1 号的 PandaScanQRCode 就是这么做的；漏掉这一步会导致扫码失败、
     游戏端也毫无反应。） */
inline std::string PandaScanQRCode(const std::string_view url, const std::string_view ticket,
                                   GameType gameType)
{
    const std::string body{ nlohmann::json{
        { "app_id", static_cast<int>(gameType) },
        { "device", device_id },
        { "ticket", ticket },
        { "passport_app_id", "bll8iq97cem8" },
        { "ts", GetUnixTimeStampSeconds() } }
                                .dump() };
    const auto response = cpr::Post(
        cpr::Url{ url },
        cpr::Body{ body },
        cpr::Header{
            { "Content-Type", "application/json" },
            { "x-rpc-app_id", "bll8iq97cem8" },
            { "x-rpc-device_id", device_id } });

    LogScanDebug("pandaScan", std::string(url), body, response.text);
    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0)
    {
        return {};
    }
    return j["data"].value("passport_qr_url", std::string{});
}

inline std::string makeSign(const nlohmann::json& data)
{
    std::string param;
    for (auto& [key, value] : data.items())
    {
        if (key == "sign")
            continue;
        const std::string strVal = value.is_string() ? value.get<std::string>() : value.dump();

        param += key + "=" + strVal + "&";
    }
    if (!param.empty())
        param.pop_back();
#ifdef _DEBUG
    std::cout << "make_param = " << param << std::endl;
#endif
    constexpr std::string_view key = "0ebc517adb1b62c6b408df153331f9aa";
    return HmacSha256(param, std::string(key));
}

inline std::string& getOAString()
{
    static std::string value = []() {
        const auto response = cpr::Get(cpr::Url{ "https://api.v6qbb.cloud/get_bh3_bilibili_oa" });
        if (response.text.empty())
            throw std::runtime_error("");
        return response.text;
    }();
    return value;
}

inline std::tuple<int, std::string, std::string, std::string> GetBH3ExternalLoginInfo(const std::string& uid, const std::string& access_key)
{
    const std::string bodyData = std::format(R"({{"access_key":"{}","uid":{}}})", access_key, uid);

    nlohmann::json body{
        { "device", "0000000000000000" },
        { "app_id", 1 },
        { "channel_id", 14 },
        { "data", bodyData }
    };
    body["sign"] = makeSign(body);
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::v2_login },
        cpr::Header{ { "Content-Type", "application/json" } },
        cpr::Body{ body.dump() });

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);

#ifdef _DEBUG
    std::cout << "崩坏3验证完成 : " << response.text << std::endl;
#endif

    if (retcode != 0)
    {
        return { retcode, {}, {}, {} };
    }

    return { 0,
             j["data"]["open_id"].get<std::string>(),
             j["data"]["combo_token"].get<std::string>(),
             j["data"]["combo_id"].get<std::string>() };
}

inline ScanRet scanCheck(const std::string& ticket)
{
    const std::string body = nlohmann::json{
        { "app_id", "1" },
        { "device", "0000000000000000" },
        { "ticket", ticket },
        { "ts", GetUnixTimeStampSeconds() }
    }.dump();

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::qrcode_scan },
        cpr::Body{ body },
        cpr::Header{ { "Content-Type", "application/json" } });

    const auto j = nlohmann::json::parse(response.text);
    return j.value("retcode", -1) == 0 ? ScanRet::SUCCESS : ScanRet::FAILURE_1;
}

inline ScanRet scanConfirm(const std::string& ticket, const std::string& uid, const std::string& access_key, const std::string& name)
{
    auto [code, open_id, combo_token, combo_id] = GetBH3ExternalLoginInfo(uid, access_key);
    if (code != 0)
        return ScanRet::FAILURE_2;

    const auto raw =
        nlohmann::json{
            { "heartbeat", false },
            { "open_id", open_id },
            { "device_id", "0000000000000000" },
            { "app_id", "1" },
            { "channel_id", "14" },
            { "combo_token", combo_token },
            { "asterisk_name", name },
            { "combo_id", combo_id },
            { "account_type", "2" }
        };

    const auto ext =
        nlohmann::json{
            { "data", nlohmann::json{
                          { "accountType", "2" },
                          { "accountID", "" },
                          { "c", open_id },
                          { "accountToken", combo_token },
                          { "dispatch", getOAString() } } }
        };

    const nlohmann::json postBody{
        { "device", "0000000000000000" },
        { "app_id", 1 },
        { "ts", GetUnixTimeStampSeconds() },
        { "ticket", ticket },
        { "payload", nlohmann::json{
                         { "proto", "Combo" },
                         { "raw", raw.dump() },
                         { "ext", ext.dump() } } }
    };

#ifdef _DEBUG
    std::cout << postBody.dump() << std::endl;
#endif

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::qrcode_confirm },
        cpr::Header{ { "Content-Type", "application/json" } },
        cpr::Body{ postBody.dump() });

    const auto j = nlohmann::json::parse(response.text);
    return j.value("retcode", -1) == 0 ? ScanRet::SUCCESS : ScanRet::FAILURE_2;
}