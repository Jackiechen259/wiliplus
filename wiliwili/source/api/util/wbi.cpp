//
// Created for wiliwili based on bilibili-API-collect documentation
//

#include "bilibili/util/wbi.hpp"

#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <cpr/cpr.h>
#include <pystring.h>

#include "bilibili/util/md5.hpp"
#include "bilibili/util/http.hpp"
#include "bilibili/util/json.hpp"
#include "bilibili/api.h"

namespace bilibili::wbi {
// 重排映射表，用于计算 mixin_key
constexpr std::array<int, 64> MIXIN_KEY_ENC_TAB = {46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
                                                   27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                                                   37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
                                                   22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52};

// 存储的 WBI 实时口令
static std::time_t g_last_update_time = 0;

// 缓存的 mixin_key
static std::string g_mixin_key;
static std::mutex g_mixin_key_mutex;
using WbiCallbackPair = std::pair<std::function<void()>, ErrorCallback>;
static bool g_update_in_flight = false;
static std::vector<WbiCallbackPair> g_update_waiters;

/**
 * 从URL中提取key
 * 例如：从 https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png
 * 提取出 7cd084941338484aae1ad9425b84077c
 */
std::string extractKeyFromUrl(const std::string& url) {
    const size_t lastSlash = url.find_last_of('/');
    const size_t lastDot   = url.find_last_of('.');

    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastSlash < lastDot) {
        return url.substr(lastSlash + 1, lastDot - lastSlash - 1);
    }

    return "";
}

/**
 * 通过混淆算法计算 mixin_key
 * 将 img_key 和 sub_key 拼接后，根据映射表重新排列得到新的字符串
 * 并截取前32位作为 mixin_key
 */
std::string getMixinKey(const std::string& img_key, const std::string& sub_key) {
    const std::string raw_key = img_key + sub_key;
    std::string key;
    key.reserve(32);

    for (size_t i = 0; i < 32; ++i) {
        key.push_back(raw_key[MIXIN_KEY_ENC_TAB[i]]);
    }

    return key;
}

void updateWbiKeys(const std::function<void()>& success, const ErrorCallback& error, bool force) {
    const std::time_t now = std::time(nullptr);
    bool startRequest     = false;

    {
        std::lock_guard lock(g_mixin_key_mutex);
        if (!force && now - g_last_update_time < 3600 && !g_mixin_key.empty()) {
            startRequest = false;
        } else {
            g_update_waiters.emplace_back(success, error);
            if (!g_update_in_flight) {
                g_update_in_flight = true;
                startRequest       = true;
            } else {
                return;
            }
        }
    }

    if (!startRequest) {
        if (success) success();
        return;
    }

    auto finish = [](const std::string& mixinKey, std::time_t updatedAt, const std::string& message, int code) {
        std::vector<WbiCallbackPair> waiters;
        {
            std::lock_guard lock(g_mixin_key_mutex);
            if (!mixinKey.empty()) {
                g_mixin_key        = mixinKey;
                g_last_update_time = updatedAt;
            }
            g_update_in_flight = false;
            waiters.swap(g_update_waiters);
        }

        for (const auto& waiter : waiters) {
            if (!mixinKey.empty()) {
                if (waiter.first) waiter.first();
            } else if (waiter.second) {
                waiter.second(message, code);
            }
        }
    };

    auto session = HTTP::createSession();
    session->SetUrl(cpr::Url{parseLink(Api::Nav)});
    session->GetCallback([finish, now](const cpr::Response& r) {
        if (r.error) {
            finish("", 0, r.error.message, -1);
            return;
        }
        if (r.status_code != 200) {
            finish("", 0, HTTP::getStatusErrorMessage(Api::Nav, r.status_code), r.status_code);
            return;
        }
        try {
            if (nlohmann::json res = nlohmann::json::parse(r.text);
                res.contains("data") && res["data"].contains("wbi_img")) {
                const std::string imgKey = extractKeyFromUrl(res["data"]["wbi_img"]["img_url"]);
                const std::string subKey = extractKeyFromUrl(res["data"]["wbi_img"]["sub_url"]);
                finish(getMixinKey(imgKey, subKey), now, "", 0);
                return;
            }
        } catch (...) {
        }
        finish("", 0, "WBI签名失败", -412);
    });
}

void encWbi(cpr::Parameters& params) {
    // 添加 wts 时间戳
    std::time_t wts = std::time(nullptr);
    params.Add({"wts", std::to_string(wts)});

    // 获取所有参数并解析
    std::string paramContent = params.GetContent(cpr::CurlHolder());
    std::vector<std::string> paramPairs;
    pystring::split(paramContent, paramPairs, "&");

    // 解析参数并过滤特殊字符
    std::map<std::string, std::string> sorted_params;
    for (const auto& pair : paramPairs) {
        std::vector<std::string> kv;
        pystring::split(pair, kv, "=");
        if (kv.size() == 2) {
            const std::string& key = kv[0];
            std::string value      = kv[1];

            // 过滤 value 中的 "!'()*" 字符
            value.erase(
                std::remove_if(value.begin(), value.end(),
                               [](char c) { return c == '!' || c == '\'' || c == '(' || c == ')' || c == '*'; }),
                value.end());

            sorted_params[key] = value;
        }
    }

    // 构建待签名字符串
    std::string query;
    for (const auto& pair : sorted_params) {
        if (!query.empty()) {
            query += "&";
        }
        query += pair.first + "=" + pair.second;
    }

    // 计算 w_rid
    std::string w_rid = websocketpp::md5::md5_hash_hex(query + g_mixin_key);

    // 添加 w_rid 参数
    params.Add({"w_rid", w_rid});
}
}
