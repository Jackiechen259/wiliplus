//
// Created by fang on 2023/1/11.
//

#include <borealis/core/logger.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/thread.hpp>

#include <pystring.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fmt/core.h>
#include <unordered_map>
#include <utility>
#include <lunasvg.h>

#include "view/danmaku_core.hpp"
#include "utils/config_helper.hpp"
#include "utils/string_helper.hpp"
#include "utils/number_helper.hpp"
#include "bilibili.h"

// include ntohl / ntohll
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#if defined(__linux__)
#include <endian.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/endian.h>
#elif defined(__OpenBSD__)
#include <sys/types.h>
#endif
#endif
#ifdef __SWITCH__
static inline uint64_t ntohll(uint64_t netlonglong) { return __builtin_bswap64(netlonglong); }
#elif defined(__WINRT__)
#elif defined(_WIN32_WINNT) && _WIN32_WINNT >= _WIN32_WINNT_WIN8
#elif defined(betoh64)
#define ntohll betoh64
#elif defined(be64toh)
#define ntohll be64toh
#elif !defined(ntohll)
static inline uint64_t ntohll(uint64_t value) {
    if (ntohl(1) == 1) {
        // The system is big endian, no conversion is needed
        return value;
    } else {
        // The system is little endian, convert from network byte order (big endian) to host byte order
        const uint32_t high_part = ntohl(static_cast<uint32_t>(value >> 32));
        const uint32_t low_part  = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFFLL));
        return (static_cast<uint64_t>(low_part) << 32) | high_part;
    }
}
#endif

// include mpv after winsock2.h
#include "view/mpv_core.hpp"

// Uncomment this line to show mask in a more obvious way.
//#define DEBUG_MASK

// Set flag to 0 to get a more soft mask
#ifndef MASK_IMG_FLAG
#define MASK_IMG_FLAG NVG_IMAGE_NEAREST
#endif

#ifndef MAX_PREFETCH_MASK
#define MAX_PREFETCH_MASK 10
#endif

#ifndef MAX_DANMAKU_LENGTH
#define MAX_DANMAKU_LENGTH 4096
#endif

namespace {

constexpr float DANMAKU_MERGE_THRESHOLD = 30.0f;
constexpr int DANMAKU_MERGE_MAX_DIST     = 5;
constexpr float DANMAKU_MERGE_MIN_COSINE = 0.45f;

bool decodeUtf8(const std::string &text, std::vector<uint32_t> &out) {
    out.clear();
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp     = 0;
        size_t extra    = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp    = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp    = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp    = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) return false;
        for (size_t j = 1; j <= extra; j++) {
            unsigned char cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.emplace_back(cp);
        i += extra + 1;
    }
    return true;
}

void appendUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000; }

bool isCjk(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

bool isEndingChar(uint32_t cp) {
    switch (cp) {
        case '.':
        case 0x3002:
        case ',':
        case 0xFF0C:
        case '/':
        case '?':
        case 0xFF1F:
        case '!':
        case 0xFF01:
        case 0x2026:
        case '~':
        case 0xFF5E:
        case '@':
        case '^':
        case 0x3001:
        case '+':
        case '=':
        case '-':
        case '_':
        case 0x2642:
        case 0x2640:
            return true;
        default:
            return isSpace(cp);
    }
}

uint32_t normalizeWidth(uint32_t cp) {
    if (cp == 0x3000) return ' ';
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
}

std::string encodeUtf8(const std::vector<uint32_t> &chars) {
    std::string out;
    for (auto cp : chars) appendUtf8(cp, out);
    return out;
}

std::string normalizeDanmakuText(const std::string &text) {
    std::vector<uint32_t> chars;
    if (!decodeUtf8(text, chars)) return pystring::strip(text);

    size_t begin = 0;
    size_t end   = chars.size();
    while (begin < end && isSpace(chars[begin])) begin++;
    while (end > begin && isSpace(chars[end - 1])) end--;

    while (end > begin && isEndingChar(chars[end - 1])) end--;
    if (begin == end) {
        begin = 0;
        end   = chars.size();
    }

    std::vector<uint32_t> normalized;
    normalized.reserve(end - begin);
    for (size_t i = begin; i < end; i++) {
        uint32_t cp = normalizeWidth(chars[i]);
        if (isSpace(cp)) {
            if (normalized.empty()) continue;
            size_t next = i + 1;
            while (next < end && isSpace(chars[next])) next++;
            if (next >= end) continue;
            if (isCjk(normalized.back()) && isCjk(normalizeWidth(chars[next]))) continue;
            if (normalized.back() != ' ') normalized.emplace_back(' ');
            continue;
        }
        normalized.emplace_back(cp);
    }

    std::string out = encodeUtf8(normalized);
    if (out.size() >= 3 && out.front() == '2' &&
        std::all_of(out.begin() + 1, out.end(), [](char c) { return c == '3'; })) {
        return "23333";
    }
    if (out.size() >= 3 && std::all_of(out.begin(), out.end(), [](char c) { return c == '6'; })) {
        return "66666";
    }
    return out;
}

std::vector<uint32_t> normalizedCodepoints(const std::string &text) {
    std::vector<uint32_t> chars;
    decodeUtf8(text, chars);
    return chars;
}

uint64_t bigramKey(uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; }

struct DanmakuTextFeatures {
    std::unordered_map<uint32_t, int> chars;
    std::unordered_map<uint64_t, int> bigrams;
    int64_t bigramNorm = 0;
};

DanmakuTextFeatures buildTextFeatures(const std::vector<uint32_t> &text) {
    DanmakuTextFeatures features;
    features.chars.reserve(text.size());
    features.bigrams.reserve(text.size());

    for (auto cp : text) features.chars[cp]++;
    for (size_t i = 0; i < text.size(); i++) {
        features.bigrams[bigramKey(text[i], text[(i + 1) % text.size()])]++;
    }
    for (const auto &item : features.bigrams) {
        features.bigramNorm += static_cast<int64_t>(item.second) * item.second;
    }
    return features;
}

int multisetDistance(const DanmakuTextFeatures &a, const DanmakuTextFeatures &b) {
    int dist = 0;
    for (const auto &item : a.chars) {
        auto it = b.chars.find(item.first);
        dist += std::abs(item.second - (it == b.chars.end() ? 0 : it->second));
    }
    for (const auto &item : b.chars) {
        if (a.chars.find(item.first) == a.chars.end()) dist += item.second;
    }
    return dist;
}

float cosineSimilarity(const DanmakuTextFeatures &a, const DanmakuTextFeatures &b) {
    if (a.bigramNorm == 0 || b.bigramNorm == 0) return 0;

    int64_t dot = 0;
    for (const auto &item : a.bigrams) {
        auto it = b.bigrams.find(item.first);
        if (it != b.bigrams.end()) dot += static_cast<int64_t>(item.second) * it->second;
    }
    return static_cast<float>(dot) * dot / a.bigramNorm / b.bigramNorm;
}

bool isMergeableDanmaku(const DanmakuItem &item) {
    if (item.type < 1 || item.type > 5) return false;
    if (item.image != DanmakuImageType::DANMAKU_IMAGE_NONE) return false;
    return !item.msg.empty();
}

bool isSimilarDanmaku(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b,
                      const DanmakuTextFeatures &featuresA, const DanmakuTextFeatures &featuresB) {
    if (a == b) return true;
    int lenSum = static_cast<int>(a.size() + b.size());
    int lenDiff = std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size()));
    if (lenDiff <= DANMAKU_MERGE_MAX_DIST) {
        int dist = multisetDistance(featuresA, featuresB);
        int minSize = std::max(1, DANMAKU_MERGE_MAX_DIST * 2);
        int maxDist = lenSum < minSize ? (DANMAKU_MERGE_MAX_DIST * lenSum / minSize) : DANMAKU_MERGE_MAX_DIST;
        if (dist <= maxDist) return true;
    }
    return cosineSimilarity(featuresA, featuresB) >= DANMAKU_MERGE_MIN_COSINE;
}

struct DanmakuMergeItem {
    DanmakuItem item;
    std::string normalized;
    std::vector<uint32_t> codepoints;
    DanmakuTextFeatures features;
};

struct DanmakuCluster {
    std::vector<size_t> indexes;
    float firstTime = 0;
};

std::string chooseClusterText(const std::vector<DanmakuMergeItem> &items, const DanmakuCluster &cluster) {
    std::unordered_map<std::string, size_t> count;
    size_t bestCount = 0;
    std::string bestText;
    for (auto index : cluster.indexes) {
        const auto &text = items[index].normalized;
        size_t current = ++count[text];
        if (current > bestCount || (current == bestCount && text.size() < bestText.size())) {
            bestCount = current;
            bestText  = text;
        }
    }
    return bestText.empty() ? items[cluster.indexes.front()].item.msg : bestText;
}

DanmakuItem buildClusterRepresentative(const std::vector<DanmakuMergeItem> &items, const DanmakuCluster &cluster) {
    size_t repOffset = std::min(cluster.indexes.size() / 5, cluster.indexes.size() - 1);
    DanmakuItem representative = items[cluster.indexes[repOffset]].item;
    representative.msg         = chooseClusterText(items, cluster);

    if (cluster.indexes.size() > 1) {
        representative.msg += fmt::format("[x{}]", cluster.indexes.size());

        for (auto index : cluster.indexes) {
            const auto &item = items[index].item;
            if (item.type == 4) {
                representative.type = 4;
            } else if (item.type == 5 && representative.type != 4) {
                representative.type = 5;
            }
            if (item.fontSize > representative.fontSize && item.fontSize < 1.2f) representative.fontSize = item.fontSize;
            if (item.level > representative.level) representative.level = item.level;
        }
    }

    representative.isShown  = false;
    representative.showing  = false;
    representative.canShow  = true;
    representative.length   = 0;
    representative.line     = 0;
    representative.speed    = 0;
    representative.startTime = 0;
    return representative;
}

std::vector<DanmakuItem> mergeDanmakuData(std::vector<DanmakuItem> data) {
    if (data.size() < 2) return data;
    std::sort(data.begin(), data.end());

    std::vector<DanmakuMergeItem> items;
    items.reserve(data.size());
    for (auto &item : data) {
        std::string normalized = isMergeableDanmaku(item) ? normalizeDanmakuText(item.msg) : "";
        auto codepoints        = normalizedCodepoints(normalized);
        auto features          = buildTextFeatures(codepoints);
        items.push_back(
            {std::move(item), std::move(normalized), std::move(codepoints), std::move(features)});
    }

    std::vector<DanmakuItem> output;
    output.reserve(data.size());
    std::deque<DanmakuCluster> clusters;

    auto flushFront = [&]() {
        output.emplace_back(buildClusterRepresentative(items, clusters.front()));
        clusters.pop_front();
    };

    for (size_t i = 0; i < items.size(); i++) {
        auto &item = items[i];
        while (!clusters.empty() && item.item.time - clusters.front().firstTime > DANMAKU_MERGE_THRESHOLD) {
            flushFront();
        }

        if (!isMergeableDanmaku(item.item) || item.normalized.empty()) {
            output.emplace_back(item.item);
            continue;
        }

        bool merged = false;
        for (auto &cluster : clusters) {
            const auto &first = items[cluster.indexes.front()];
            if (isSimilarDanmaku(item.codepoints, first.codepoints, item.features, first.features)) {
                cluster.indexes.emplace_back(i);
                merged = true;
                break;
            }
        }
        if (!merged) {
            clusters.push_back({{i}, item.item.time});
        }
    }

    while (!clusters.empty()) flushFront();
    std::sort(output.begin(), output.end());
    return output;
}

}  // namespace

DanmakuItem::DanmakuItem(std::string content, const char *attributes) : msg(std::move(content)) {
    std::vector<std::string> attrs;
    pystring::split(attributes, attrs, ",");
    if (attrs.size() < 9) {
        brls::Logger::error("error decode danmaku: {} {}", msg, attributes);
        type = -1;
        return;
    }
    time      = atof(attrs[0].c_str());
    type      = atoi(attrs[1].c_str());
    fontSize  = atoi(attrs[2].c_str()) / 25.0f;
    fontColor = atoi(attrs[3].c_str());
    level     = atoi(attrs[8].c_str());

    int r          = (fontColor >> 16) & 0xff;
    int g          = (fontColor >> 8) & 0xff;
    int b          = fontColor & 0xff;
    isDefaultColor = (r & g & b) == 0xff;
    color          = nvgRGB(r, g, b);
    color.a        = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.01;
    borderColor.a  = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.005;

    // 判断是否添加浅色边框
    if ((r * 299 + g * 587 + b * 114) < 60000) {
        borderColor   = nvgRGB(255, 255, 255);
        borderColor.a = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.005;
    }

    // 解析高级弹幕动画
    if (type == 7) {
        // 高级弹幕示例:
        // [1045,275,"1-0",4,"内容",0,90,109,274,3000,500,1,"SimHei",1]
        // [0.9,0.01,"0.1-0.9",3,"百分比坐标",1,2,0.1,0.02,400,100,1,"\"Microsoft YaHei\"",1]
        // [0.9,0.01,"0.1-0.9",3,"路径跟随",1,2,0.1,0.02,400,100,1,"\"Microsoft YaHei\"",1,"M1043,57L1043,57L1043,57L1041,57L1041,58L1040,59L1038,61L1035,64L1033,67L1031,68L1028,71L1025,71L1019,71L1017,71L1010,70L994,64L982,58L969,50L959,41L949,33L944,29L942,27L941,26L941,25L941,23L941,23L941,23"]
        // 起始x,起始y, "透明度起始-透明度结束", 生存时间, "正文"，z翻转，y翻转，结束x，结束y，运行耗时(ms)，延迟时间(ms), 文字描边 , "字体", 线性加速, ["路径跟随"]
        // 如果 起始x,起始y 结束x，结束y 都小于 1 则使用百分比坐标。
        if (msg.size() < 2) return;
        std::vector<std::string> extraData;
        pystring::split(msg.substr(1, msg.size() - 2), extraData, ",", 14);
        if (extraData.size() >= 11) {
            std::vector<std::string> alphaData;
            pystring::split(pystring::strip(extraData[2], "\""), alphaData, "-");
            if (alphaData.size() != 2) return;
            msg = pystring::strip(extraData[4], "\"");
            msg = pystring::replace(msg, "\\n", "\n");
            AdvancedAnimation ani{};
            ani.alpha1    = atof(pystring::strip(alphaData[0], "\"").c_str());
            ani.alpha2    = atof(pystring::strip(alphaData[1], "\"").c_str());
            ani.startX    = atof(pystring::strip(extraData[0], "\"").c_str());
            ani.startY    = atof(pystring::strip(extraData[1], "\"").c_str());
            ani.endX      = atof(pystring::strip(extraData[7], "\"").c_str());
            ani.endY      = atof(pystring::strip(extraData[8], "\"").c_str());
            ani.time1     = atof(pystring::strip(extraData[10], "\"").c_str());
            ani.time2     = atof(pystring::strip(extraData[9], "\"").c_str());
            ani.rotateZ   = nvgDegToRad(atof(pystring::strip(extraData[5], "\"").c_str()));
            ani.rotateY   = nvgDegToRad(atof(pystring::strip(extraData[6], "\"").c_str()));
            ani.linear    = pystring::strip(extraData[13], "\"") == "1";
            ani.wholeTime = atof(pystring::strip(extraData[3], "\"").c_str()) * 1000;
            ani.time3     = ani.wholeTime - ani.time1 - ani.time2;
            if (ani.time3 < 0) ani.time3 = 0;

            // 如果起点和终点坐标都小于等于1，则说明其为百分比坐标
            ani.relativeLayout = ani.startX <= 1.0f && ani.startY <= 1.0f && ani.endX <= 1.0f && ani.endY <= 1.0f;

            // 路径跟随
            if (extraData.size() >= 15) {
                std::vector<brls::Point> p;
                auto str = pystring::strip(extraData[14], "\"M");
                std::vector<std::string> pointData;
                pystring::split(str, pointData, "L");
                for (auto &pointStr : pointData) {
                    std::vector<std::string> xy;
                    pystring::split(pointStr, xy, ",");
                    if (xy.size() != 2) continue;
                    ani.path.emplace_back(atof(xy[0].c_str()), atof(xy[1].c_str()));
                }
                // 强制关闭相对坐标
                ani.relativeLayout = false;
            } else {
                ani.path.emplace_back(ani.startX, ani.startY);
                ani.path.emplace_back(ani.endX, ani.endY);
            }
            advancedAnimation.emplace(ani);
        }
    }

    // 调整弹幕内容
    if (msg.size() > 1 && msg[0] == '[' && msg[msg.size() - 1] == ']') {
        // 检查是否是彩蛋弹幕: [ohh] [前方高能]
        if (pystring::startswith(msg, "[前方高能")) {
            image = DanmakuImageType::DANMAKU_IMAGE_HIGHLIGHT;
        } else if (pystring::startswith(wiliwili::toUpper(msg, 4), "[OHH")) {
            image = DanmakuImageType::DANMAKU_IMAGE_OHH;
        }
    } else {
        // 根据需要做简繁转换
#ifdef OPENCC
        static bool ZH_T = brls::Application::getLocale() == brls::LOCALE_ZH_HANT ||
                           brls::Application::getLocale() == brls::LOCALE_ZH_TW;
        if (ZH_T && brls::Label::OPENCC_ON) msg = brls::Label::STConverter(msg);
#endif
    }
}

void DanmakuItem::draw(NVGcontext *vg, float x, float y, float alpha, bool multiLine) const {
    if (image != DanmakuImageType::DANMAKU_IMAGE_NONE) {
        // 绘制图片弹幕
        float width    = DanmakuCore::DANMAKU_STYLE_FONTSIZE * 3;
        float height   = DanmakuCore::DANMAKU_STYLE_FONTSIZE;
        float newAlpha = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.01f * alpha;
        nvgBeginPath(vg);
        auto paint =
            nvgImagePattern(vg, x, y, width, height, 0,
                            image == DanmakuImageType::DANMAKU_IMAGE_OHH ? DanmakuCore::DANMAKU_IMAGE_OHH
                                                                         : DanmakuCore::DANMAKU_IMAGE_HIGHLIGHT,
                            newAlpha);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
        return;
    }
    float blur   = DanmakuCore::DANMAKU_STYLE_FONT == DanmakuFontStyle::DANMAKU_FONT_SHADOW;
    float dilate = DanmakuCore::DANMAKU_STYLE_FONT == DanmakuFontStyle::DANMAKU_FONT_STROKE;
    float dx, dy;
    dx = dy = DanmakuCore::DANMAKU_STYLE_FONT == DanmakuFontStyle::DANMAKU_FONT_INCLINE;

    // background
    if (DanmakuCore::DANMAKU_STYLE_FONT != DanmakuFontStyle::DANMAKU_FONT_PURE) {
        nvgFontDilate(vg, dilate);
        nvgFontBlur(vg, blur);
        nvgFillColor(vg, a(borderColor, alpha));
        if (multiLine)
            nvgTextBox(vg, x + dx, y + dy, MAX_DANMAKU_LENGTH, msg.c_str(), nullptr);
        else
            nvgText(vg, x + dx, y + dy, msg.c_str(), nullptr);
    }

    // content
    nvgFontDilate(vg, 0.0f);
    nvgFontBlur(vg, 0.0f);
    nvgFillColor(vg, a(color, alpha));
    if (multiLine)
        nvgTextBox(vg, x + dx, y + dy, MAX_DANMAKU_LENGTH, msg.c_str(), nullptr);
    else
        nvgText(vg, x, y, msg.c_str(), nullptr);
}

NVGcolor DanmakuItem::a(NVGcolor color, float alpha) {
    color.a *= alpha;
    return color;
}

DanmakuCore::DanmakuCore() {
    event_id = MPV_E->subscribe([this](MpvEventEnum e) {
        if (e == MpvEventEnum::LOADING_END) {
            this->refresh();
        } else if (e == MpvEventEnum::RESET) {
            this->reset();
        } else if (e == MpvEventEnum::VIDEO_SPEED_CHANGE) {
            this->setSpeed(MPVCore::instance().getSpeed());
        }
    });

    NVGcontext *vg = brls::Application::getNVGContext();
#ifdef USE_LIBROMFS
    auto image1             = romfs::get("pictures/danmaku_ohh.png");
    DANMAKU_IMAGE_OHH       = nvgCreateImageMem(vg, 0, (unsigned char *)image1.data(), image1.size());
    auto image2             = romfs::get("pictures/danmaku_highlight.png");
    DANMAKU_IMAGE_HIGHLIGHT = nvgCreateImageMem(vg, 0, (unsigned char *)image2.data(), image2.size());
#else
    DANMAKU_IMAGE_OHH       = nvgCreateImage(vg, BRLS_ASSET("pictures/danmaku_ohh.png"), 0);
    DANMAKU_IMAGE_HIGHLIGHT = nvgCreateImage(vg, BRLS_ASSET("pictures/danmaku_highlight.png"), 0);
#endif

    // 退出前清空遮罩纹理
    brls::Application::getExitDoneEvent()->subscribe([this]() {
        NVGcontext *vg = brls::Application::getNVGContext();
        if (maskTex != 0) {
            nvgDeleteImage(vg, maskTex);
            maskTex = 0;
        }
        if (DANMAKU_IMAGE_OHH != 0) {
            nvgDeleteImage(vg, DANMAKU_IMAGE_OHH);
            DANMAKU_IMAGE_OHH = 0;
        }
        if (DANMAKU_IMAGE_HIGHLIGHT != 0) {
            nvgDeleteImage(vg, DANMAKU_IMAGE_HIGHLIGHT);
            DANMAKU_IMAGE_HIGHLIGHT = 0;
        }
    });
}

DanmakuCore::~DanmakuCore() { MPV_E->unsubscribe(event_id); }

void DanmakuCore::reset() {
    danmakuDataGeneration.fetch_add(1, std::memory_order_relaxed);
    danmakuMutex.lock();
    lineNum     = 20;
    scrollLines = std::vector<std::pair<float, float>>(20, {0, 0});
    centerLines = std::vector<float>(20, {0});
    this->danmakuData.clear();
    this->danmakuLoaded = false;
    danmakuIndex        = 0;
    maskIndex           = 0;
    maskLastIndex       = SIZE_T_MAX;
    maskWidth        = 0;
    maskHeight       = 0;
    maskSliceIndex      = 0;
    videoSpeed          = MPVCore::instance().getSpeed();
    lineHeight          = DANMAKU_STYLE_FONTSIZE * DANMAKU_STYLE_LINE_HEIGHT * 0.01f;
    maskData.clear();
    if (maskTex != 0) {
        nvgDeleteImage(brls::Application::getNVGContext(), maskTex);
        maskTex = 0;
    }
    danmakuMutex.unlock();
}

uint64_t DanmakuCore::beginDanmakuRequest() {
    return danmakuDataGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::vector<DanmakuItem> DanmakuCore::prepareDanmakuData(std::vector<DanmakuItem> data, bool merge) {
    if (merge) return mergeDanmakuData(std::move(data));
    std::sort(data.begin(), data.end());
    return data;
}

void DanmakuCore::loadDanmakuData(std::vector<DanmakuItem> data, uint64_t generation) {
    if (generation != danmakuDataGeneration.load(std::memory_order_relaxed)) return;

    {
        std::lock_guard<std::mutex> lock(danmakuMutex);
        this->danmakuData = std::move(data);
        danmakuLoaded     = !danmakuData.empty();
    }

    this->refresh();
    APP_E->fire("DANMAKU_LOADED", nullptr);
}

void DanmakuCore::addSingleDanmaku(const DanmakuItem &item) {
    danmakuMutex.lock();
    this->danmakuData.emplace_back(item);
    this->danmakuLoaded = true;
    danmakuMutex.unlock();

    // 通过mpv来通知弹幕加载完成
    APP_E->fire("DANMAKU_LOADED", nullptr);
}

void DanmakuCore::loadMaskData(const std::string &url) {
    maskData.clear();
    BILI::get_webmask(
        url, 0, 15,
        [this, url](const std::string &text) {
            maskData.url = url;
            if (text.size() != 16) {
                brls::Logger::error("解析数据头失败: {}", text.size());
                return;
            }
            brls::Logger::debug("解析防遮挡数据头: {}", url);
            maskData.parseHeader1(text);
            brls::Logger::debug("解析数据头结束，数据段数量：{}", maskData.length);
            BILI::get_webmask(
                url, 16, 16 * maskData.length + 15,
                [this](const std::string &text) {
                    if (text.size() != (size_t)(16 * maskData.length)) {
                        brls::Logger::error("解析数据头2失败: {} != {}", text.size(), 16 * maskData.length);
                        return;
                    }
                    brls::Logger::debug("解析防遮挡数据头2: {}", text.size());
                    maskData.parseHeader2(text);
                    brls::Logger::debug("解析数据头2结束");
                },
                [](BILI_ERR) { brls::Logger::error("get web mask 2: {}", error); });
        },
        [](BILI_ERR) { brls::Logger::error("get web mask 1: {}", error); });
}

void DanmakuCore::refresh() {
    danmakuMutex.lock();

    // 获取视频播放速度
    videoSpeed = MPVCore::instance().getSpeed();

    // 将当前屏幕第一条弹幕序号设为0
    danmakuIndex = 0;

    // 将遮罩序号设为0
    maskSliceIndex = 0;
    maskIndex      = 0;
    maskLastIndex  = SIZE_T_MAX;
    maskWidth   = 0;
    maskHeight  = 0;

    // 重置弹幕控制显示的信息
    for (auto &i : danmakuData) {
        i.showing = false;
        i.canShow = true;
    }

    // 重新设置最大显示的行数
    lineNum = brls::Application::windowHeight / DANMAKU_STYLE_FONTSIZE;
    while (scrollLines.size() < lineNum) {
        scrollLines.emplace_back(0, 0);
        centerLines.emplace_back(0);
    }

    // 重新设置行高
    lineHeight = DANMAKU_STYLE_FONTSIZE * DANMAKU_STYLE_LINE_HEIGHT * 0.01f;

    // 更新弹幕透明度
    for (auto &d : danmakuData) {
        d.color.a       = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.01;
        d.borderColor.a = DanmakuCore::DANMAKU_STYLE_ALPHA * 0.005;
    }

    // 重置弹幕每行的时间信息
    for (size_t k = 0; k < lineNum; k++) {
        scrollLines[k].first  = 0;
        scrollLines[k].second = 0;
        centerLines[k]        = 0;
    }
    danmakuMutex.unlock();
}

void DanmakuCore::setSpeed(double speed) {
    double oldSpeed     = videoSpeed;
    videoSpeed          = speed;
    int64_t currentTime = brls::getCPUTimeUsec();
    double factor       = oldSpeed / speed;
    // 修改滚动弹幕的起始播放时间，满足修改后的时间在新速度下生成的位置不变。
    for (size_t j = this->danmakuIndex; j < this->danmakuData.size(); j++) {
        auto &i = this->danmakuData[j];
        if (i.type == 4 || i.type == 5) continue;
        if (!i.canShow) continue;
        if (i.time > MPVCore::instance().playback_time) return;
        i.startTime = currentTime - (currentTime - i.startTime) * factor;
    }
}

void DanmakuCore::save() {
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_ON, DANMAKU_ON, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_SMART_MASK, DANMAKU_SMART_MASK, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_TOP, DANMAKU_FILTER_SHOW_TOP, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_BOTTOM, DANMAKU_FILTER_SHOW_BOTTOM, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_SCROLL, DANMAKU_FILTER_SHOW_SCROLL, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_COLOR, DANMAKU_FILTER_SHOW_COLOR, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_ADVANCED, DANMAKU_FILTER_SHOW_ADVANCED, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_MERGE, DANMAKU_MERGE, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_FILTER_LEVEL, DANMAKU_FILTER_LEVEL, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_STYLE_AREA, DANMAKU_STYLE_AREA, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_STYLE_FONTSIZE, DANMAKU_STYLE_FONTSIZE, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_STYLE_LINE_HEIGHT, DANMAKU_STYLE_LINE_HEIGHT, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_STYLE_SPEED, DANMAKU_STYLE_SPEED, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_STYLE_ALPHA, DANMAKU_STYLE_ALPHA, false);
    ProgramConfig::instance().setSettingItem(SettingItem::DANMAKU_RENDER_QUALITY, DANMAKU_RENDER_QUALITY, false);
    ProgramConfig::instance().save();
}

std::vector<DanmakuItem> DanmakuCore::getDanmakuData() {
    danmakuMutex.lock();
    std::vector<DanmakuItem> data = danmakuData;
    danmakuMutex.unlock();
    return data;
}

void DanmakuCore::drawMask(NVGcontext *vg, float x, float y, float width, float height) {
#ifdef DRAW_DANMAKU_MASK
    if (!DANMAKU_SMART_MASK || !maskData.isLoaded()) return;
    double playbackTime = MPVCore::instance().playback_time;
    /// 1. 先根据时间选择分片
    while (maskSliceIndex < maskData.sliceData.size() - 1) {
        auto &slice = maskData.sliceData[maskSliceIndex + 1];
        if (slice.time > playbackTime * 1000) break;
        maskSliceIndex++;
        maskIndex     = 0;
        maskLastIndex = SIZE_T_MAX;
        maskWidth  = 0;
        maskHeight = 0;
    }

    /// 2. 在分片内选择对应时间的svg
    if (maskSliceIndex >= maskData.sliceData.size()) return;
    auto &slice = maskData.getSlice(maskSliceIndex);
    if (!slice.isLoaded()) return;
    while (maskIndex < slice.svgData.size() - 1) {
        auto &svg = slice.svgData[maskIndex + 1];
        if (svg.showTime > playbackTime * 1000) break;
        maskIndex++;
    }

    /// 3. 从 svg 生成遮罩纹理
    if (maskIndex >= slice.svgData.size()) return;
    if (maskLastIndex != maskIndex) {
        maskLastIndex = maskIndex;
        // 生成新的纹理
        auto maskDocument = lunasvg::Document::loadFromData(slice.svgData[maskIndex].svg);
        if (maskDocument == nullptr) return;
        auto bitmap = maskDocument->renderToBitmap(maskDocument->width(), maskDocument->height());
        maskWidth   = bitmap.width();
        maskHeight  = bitmap.height();
#ifdef BOREALIS_USE_D3D11
        // 使用 dx11 的拷贝交换
        const static int imageFlags = NVG_IMAGE_STREAMING | NVG_IMAGE_COPY_SWAP | MASK_IMG_FLAG;
#else
        const static int imageFlags = MASK_IMG_FLAG;
#endif
        if (maskTex != 0) {
            nvgUpdateImage(vg, maskTex, bitmap.data());
        } else {
            maskTex = nvgCreateImageRGBA(vg, (int)maskWidth, (int)maskHeight, imageFlags, bitmap.data());
        }
    }
    if (maskTex == 0) return;

    /// 根据视频设置调整遮罩尺寸
    // 手动设置了视频比例
    if (MPVCore::instance().video_aspect > 0) {
        maskWidth = maskHeight * MPVCore::instance().video_aspect;
    }
    // 镜像视频
    if (MPVCore::VIDEO_MIRROR) {
        nvgSave(vg);
        nvgTranslate(vg, width + x + x, 0);
        nvgScale(vg, -1, 1);
    }
    nvgBeginPath(vg);
    float drawHeight = height, drawWidth = width;
    float drawX = x, drawY = y;
    if (MPVCore::instance().video_aspect == -2) {
        // 拉伸全屏
    } else if (MPVCore::instance().video_aspect == -3) {
        // 裁剪填充
        if (maskWidth * height > maskHeight * width) {
            drawWidth = maskWidth * height / maskHeight;
            drawX     = x + (width - drawWidth) / 2;
        } else {
            drawHeight = maskHeight * width / maskWidth;
            drawY      = y + (height - drawHeight) / 2;
        }
    } else {
        // 自由比例
        if (maskWidth * height > maskHeight * width) {
            drawHeight = maskHeight * width / maskWidth;
            drawY      = y + (height - drawHeight) / 2;
        } else {
            drawWidth = maskWidth * height / maskHeight;
            drawX     = x + (width - drawWidth) / 2;
        }
    }

    // 返回的 svg 底部固定留有1像素高度透明 (不是很清楚具体作用)
    // 拉伸图片纹理1像素高度，绘制时舍弃掉这部分
    float offsetY = drawHeight / maskHeight;

    /// 遮罩绘制
    auto paint = nvgImagePattern(vg, drawX, drawY, drawWidth, drawHeight + offsetY, 0, maskTex, 1.0f);
    nvgRect(vg, drawX, drawY, drawWidth, drawHeight);
    nvgFillPaint(vg, paint);
#if defined(DEBUG_MASK)
    nvgFill(vg);
#else
    nvgStencil(vg);
#endif

    // 镜像视频恢复
    if (MPVCore::VIDEO_MIRROR) nvgRestore(vg);
#endif
}

void DanmakuCore::clearMask(NVGcontext *vg, float x, float y, float width, float height) {
#if !defined(DEBUG_MASK) && defined(DRAW_DANMAKU_MASK)
    if (!DANMAKU_SMART_MASK || !maskData.isLoaded()) return;
    if (maskTex > 0) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgStencilClear(vg);
    }
#endif
}

void DanmakuCore::draw(NVGcontext *vg, float x, float y, float width, float height, float alpha) {
    if (!DanmakuCore::DANMAKU_ON) return;
    if (!this->danmakuLoaded) return;
    if (danmakuData.empty()) return;

    int64_t currentTime = brls::getCPUTimeUsec();
    double playbackTime = MPVCore::instance().playback_time;
    float SECOND        = 0.12f * DANMAKU_STYLE_SPEED;
    float CENTER_SECOND = 0.04f * DANMAKU_STYLE_SPEED;

    // Enable scissoring
    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, width, height);

    // 设置遮罩
    drawMask(vg, x, y, width, height);

    // 设置基础字体
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFaceId(vg, DanmakuCore::DANMAKU_FONT);
    nvgTextLineHeight(vg, 1);

    // 弹幕渲染质量
    if (DANMAKU_RENDER_QUALITY < 100) {
        nvgFontQuality(vg, 0.01f * DANMAKU_RENDER_QUALITY);
    }

    // 实际弹幕显示行数 （小于等于总行数且受弹幕显示区域限制）
    size_t LINES = height / lineHeight * DANMAKU_STYLE_AREA * 0.01f;
    if (LINES > lineNum) LINES = lineNum;

    //取出需要的弹幕
    float bounds[4];
    for (size_t j = this->danmakuIndex; j < this->danmakuData.size(); j++) {
        auto &i = this->danmakuData[j];
        // 溢出屏幕外或被过滤调不展示的弹幕
        if (!i.canShow) continue;

        // 正在展示中的弹幕
        if (i.showing) {
            if (i.type == 4 || i.type == 5) {
                //居中弹幕
                // 根据时间判断是否显示弹幕
                if (i.time > playbackTime || i.time + CENTER_SECOND < playbackTime) {
                    i.canShow = false;
                    continue;
                }

                // 画弹幕
                nvgFontSize(vg, DANMAKU_STYLE_FONTSIZE * i.fontSize);
                i.draw(vg, x + width / 2 - i.length / 2, y + i.line * lineHeight + 5, alpha);

                continue;
            } else if (i.type == 7) {
                if (!i.advancedAnimation.has_value() || !i.advancedAnimation->alpha.isRunning()) {
                    i.canShow = false;
                    continue;
                }
                nvgFontSize(vg, DANMAKU_STYLE_FONTSIZE * i.fontSize);
                nvgSave(vg);
                nvgTranslate(vg, x + i.advancedAnimation->transX, y + i.advancedAnimation->transY);
                nvgRotate(vg, i.advancedAnimation->rotateZ);
                if (i.advancedAnimation->rotateY > 0) {
                    // 近似模拟出 y 轴翻转的效果, 其实不太近似 :(
                    float ratio = fabs(1 - i.advancedAnimation->transX / width);
                    if (ratio > 1) ratio = 1;
                    float rotateY = i.advancedAnimation->rotateY * ratio / 2;
                    nvgScale(vg, 1 - rotateY / NVG_PI, 1.0f);
                    nvgSkewY(vg, rotateY);
                }
                i.draw(vg, 0, 0, i.advancedAnimation->alpha * alpha, true);
                nvgRestore(vg);
                continue;
            }
            //滑动弹幕
            float position = 0;
            if (!MPVCore::instance().isPlaying()) {
                // 暂停状态弹幕也要暂停
                position    = i.speed * (playbackTime - i.time);
                i.startTime = currentTime - (playbackTime - i.time) / videoSpeed * 1e6;
            } else {
                // position = i.speed * (playbackTime - i.time) 是最精确的弹幕位置
                // 但是因为 playbackTime 是根据视频帧率设定的，直接使用此值会导致弹幕看起来卡顿
                // 这里以弹幕绘制的起始时间点为准，通过与当前时间值的差值计算来得到更精确的弹幕位置
                // 但是当 AV 不同步时，mpv会自动修正播放的进度，导致 playbackTime 和现实的时间脱离，不同弹幕间因此可能产生重叠
                position = i.speed * (currentTime - i.startTime) * videoSpeed / 1e6;
            }

            // 根据位置判断是否显示弹幕
            if (position > width + i.length) {
                i.showing    = false;
                danmakuIndex = j + 1;
                continue;
            }

            // 画弹幕
            nvgFontSize(vg, DANMAKU_STYLE_FONTSIZE * i.fontSize);
            i.draw(vg, x + width - position, y + i.line * lineHeight + 5, alpha);
            continue;
        }

        // 添加即将出现的弹幕
        if (i.time < playbackTime) {
            // 排除已经应该暂停显示的弹幕
            if (i.type == 4 || i.type == 5) {
                // 底部或顶部弹幕
                if (i.time + CENTER_SECOND < playbackTime) {
                    continue;
                }
            } else if (i.time + SECOND < playbackTime) {
                // 滚动弹幕
                danmakuIndex = j + 1;
                continue;
            }

            /// 过滤弹幕
            i.canShow = false;
            // 1. 过滤显示的弹幕级别
            if (i.level < DANMAKU_FILTER_LEVEL) continue;

            if (i.type == 4) {
                // 2. 过滤底部弹幕
                if (!DANMAKU_FILTER_SHOW_BOTTOM) continue;
            } else if (i.type == 5) {
                // 3. 过滤顶部弹幕
                if (!DANMAKU_FILTER_SHOW_TOP) continue;
            } else if (i.type == 7) {
                // 4. 过滤高级弹幕
                if (!DANMAKU_FILTER_SHOW_ADVANCED) continue;
            } else {
                // 5. 过滤滚动弹幕
                if (!DANMAKU_FILTER_SHOW_SCROLL) continue;
            }

            // 6. 过滤彩色弹幕
            if (!i.isDefaultColor && !DANMAKU_FILTER_SHOW_COLOR) continue;

            // 7. 过滤失效弹幕
            if (i.type < 0) continue;

            // 处理高级弹幕动画
            if (i.type == 7) {
                if (!i.advancedAnimation.has_value()) continue;
                if (fabs(playbackTime - i.time) > 0.1) continue;
                i.showing = true;
                i.canShow = true;
                auto &ani = i.advancedAnimation;
                ani->alpha.stop();
                ani->transX.stop();
                ani->transY.stop();
                if (ani->path.size() < 2) continue;

                // 是否使用线形动画
                brls::EasingFunction easing =
                    ani->linear ? brls::EasingFunction::linear : brls::EasingFunction::cubicIn;

                // 是否使用相对坐标
                float relativeSizeX = 1.0f, relativeSizeY = 1.0f;
                if (ani->relativeLayout) {
                    relativeSizeX = width;
                    relativeSizeY = height;
                }

                ani->transX.reset(ani->path[0].x * relativeSizeX);
                ani->transY.reset(ani->path[0].y * relativeSizeY);
                ani->alpha.reset(ani->alpha1);

                // 起点停留
                if (ani->time1 > 0) {
                    ani->transX.addStep(ani->path[0].x * relativeSizeX, ani->time1);
                    ani->transY.addStep(ani->path[0].y * relativeSizeY, ani->time1);
                }

                // 路径动画
                if (ani->time2 > 0) {
                    float timeD = ani->time2 / (ani->path.size() - 1);
                    for (size_t p = 1; p < ani->path.size(); p++) {
                        ani->transX.addStep(ani->path[p].x * relativeSizeX, timeD, easing);
                        ani->transY.addStep(ani->path[p].y * relativeSizeY, timeD, easing);
                    }
                }

                // 结束点停留
                if (ani->time3 > 0) {
                    ani->transX.addStep(ani->path[ani->path.size() - 1].x * relativeSizeX, ani->time3);
                    ani->transY.addStep(ani->path[ani->path.size() - 1].y * relativeSizeY, ani->time3);
                }

                // 半透明
                ani->alpha.addStep(ani->alpha2, ani->wholeTime);
                ani->alpha.start();
                ani->transX.start();
                ani->transY.start();
                continue;
            }

            /// 处理即将要显示的弹幕
            if (i.image != DanmakuImageType::DANMAKU_IMAGE_NONE) {
                i.length = DANMAKU_STYLE_FONTSIZE * 3;
            } else {
                nvgFontSize(vg, DANMAKU_STYLE_FONTSIZE * i.fontSize);
                nvgTextBounds(vg, 0, 0, i.msg.c_str(), nullptr, bounds);
                i.length = bounds[2] - bounds[0];
            }
            i.speed   = (width + i.length) / SECOND;
            i.showing = true;
            for (size_t k = 0; k < LINES; k++) {
                if (i.type == 4) {
                    //底部
                    if (i.time < centerLines[LINES - k - 1]) continue;

                    i.line                     = LINES - k - 1;
                    centerLines[LINES - k - 1] = i.time + CENTER_SECOND;
                    i.canShow                  = true;
                    break;
                } else if (i.type == 5) {
                    //顶部
                    if (i.time < centerLines[k]) continue;

                    i.line         = k;
                    centerLines[k] = i.time + CENTER_SECOND;
                    i.canShow      = true;
                    break;
                } else {
                    //滚动
                    if (i.time < scrollLines[k].first || i.time + width / i.speed < scrollLines[k].second) continue;
                    i.line = k;
                    // 一条弹幕完全展示的时间点，同一行的其他弹幕需要在这之后出现
                    scrollLines[k].first = i.time + i.length / i.speed;
                    // 一条弹幕展示结束的时间点，同一行的其他弹幕到达屏幕左侧的时间应该在这之后。
                    scrollLines[k].second = i.time + SECOND;
                    i.canShow             = true;
                    i.startTime           = currentTime;
                    // 如果当前时间点弹幕已经出现在屏幕上了，那么反向推算出弹幕开始的现实时间
                    if (playbackTime - i.time > 0.2) i.startTime -= (playbackTime - i.time) / videoSpeed * 1e6;
                    break;
                }
            }
            // 循环之外的弹幕因为 canShow 为 false 所以不会显示
        } else {
            // 当前没有需要显示或等待显示的弹幕，结束循环
            break;
        }
    }

    // 清空遮罩
    clearMask(vg, x, y, width, height);
    nvgRestore(vg);
}

void WebMask::parseHeader1(const std::string &text) {
    // 检查头部
    std::memcpy(&version, text.data() + 4, sizeof(int32_t));
    std::memcpy(&check, text.data() + 8, sizeof(int32_t));
    std::memcpy(&length, text.data() + 12, sizeof(int32_t));
    version = ntohl(version);
    check   = ntohl(check);
    length  = ntohl(length);
}

void WebMask::parseHeader2(const std::string &text) {
    // 获取所有分片信息
    std::vector<MaskSlice> sliceList;

    sliceList.reserve(length);
    uint64_t time, offset, currentOffset = 0;
    for (int32_t i = 0; i < length; i++) {
        std::memcpy(&time, text.data() + currentOffset, sizeof(uint64_t));
        std::memcpy(&offset, text.data() + currentOffset + 8, sizeof(uint64_t));
        time   = ntohll(time);
        offset = ntohll(offset);
        sliceList.emplace_back(time, offset, 0);
        if (i != 0) sliceList[i - 1].offsetEnd = offset;
        if (i == length - 1) sliceList[i].offsetEnd = SIZE_T_MAX;
        currentOffset += 16;
    }

    // 同步数据
    brls::sync([this, sliceList]() {
        if (this->sliceData.empty())
            this->sliceData = sliceList;
        else
            brls::Logger::error("sliceData is not empty");
    });
}

void WebMask::clear() { this->sliceData.clear(); }

const MaskSlice &WebMask::getSlice(size_t index) {
    auto &slice = sliceData[index];
    if (slice.isLoaded()) {
        // 当前片段有数据，检查下一个片段是否存在数据，如果存在则直接返回当前片段
        index++;
        if (index >= sliceData.size() || sliceData[index].isLoaded()) {
            return slice;
        }
    }

    static bool requesting{false};
    if (requesting) return slice;
    requesting = true;

    // 生成预取数据长度，一次最多取十个片段: 100s
    size_t requestStart = index;
    size_t requestEnd   = index;
    for (; requestEnd < index + MAX_PREFETCH_MASK && requestEnd < sliceData.size(); requestEnd++) {
        if (sliceData[requestEnd].isLoaded()) break;
    }
    brls::Logger::debug("预取 web mask 数据片段: [{}, {})", index, requestEnd);

    // 请求数据
    auto slices = this->sliceData;
    BILI::get_webmask(
        url, sliceData[index].offsetStart, sliceData[requestEnd - 1].offsetEnd,
        [this, slices, requestStart, requestEnd](const std::string &text) {
            brls::Logger::debug("获取片段结束: {}", text.size());
            uint64_t offset = slices[requestStart].offsetStart;
            std::vector<MaskSlice> sliceList;
            sliceList.reserve(requestEnd - requestStart);
            for (size_t i = requestStart; i < requestEnd; i++) {
                MaskSlice slice = slices[i];
                // 解压分片数据
                std::string data;
                try {
                    if (slice.offsetEnd == SIZE_T_MAX) slice.offsetEnd = text.size() + offset;
                    data = wiliwili::decompressGzipData(
                        text.substr(slice.offsetStart - offset, slice.offsetEnd - slice.offsetStart));
                } catch (const std::runtime_error &e) {
                    brls::Logger::error("web mask decompress error: {}", e.what());
                } catch (const std::exception &e) {
                    brls::Logger::error("web mask decompress exception: {}", e.what());
                }

                // 获取svg数据
                size_t sliceOffset = 0;
                uint32_t sliceLength, sliceTime;
                while (sliceOffset < data.size()) {
                    std::memcpy(&sliceLength, data.data() + sliceOffset, sizeof(int32_t));
                    std::memcpy(&sliceTime, data.data() + sliceOffset + 8, sizeof(int32_t));
                    sliceLength = ntohl(sliceLength);
                    sliceTime   = ntohl(sliceTime);
                    sliceOffset += 12;
                    auto base64 =
                        pystring::replace(pystring::split(data.substr(sliceOffset, sliceLength), ",", 1)[1], "\n", "");
                    std::string svg;
                    wiliwili::base64Decode(base64, svg);
                    sliceOffset += sliceLength;

                    slice.svgData.emplace_back(svg, sliceTime);
                }

                sliceList.emplace_back(slice);
            }

            // 同步数据
            brls::sync([this, requestStart, sliceList]() {
                if (!sliceData.empty()) {
                    std::copy(sliceList.begin(), sliceList.end(), sliceData.begin() + requestStart);
                } else {
                    brls::Logger::warning("sliceData is empty, skip mask data update");
                }
                requesting = false;
            });
        },
        [](BILI_ERR) {
            brls::Logger::error("get web mask slice: {}", error);
            requesting = false;
        });
    return slice;
}

bool WebMask::isLoaded() const { return !this->sliceData.empty(); }

bool MaskSlice::isLoaded() const { return !this->svgData.empty(); }
