#include "parse/clause_no.h"
#include <regex>

// 单位词/符号前缀：rest 以这些开头则判为数值，不当条款号。
static bool rest_starts_like_unit(const std::string& rest) {
    if (rest.empty()) return false;
    if (rest[0] == '%') return true;
    // 单位词前缀（按语料确认的误报逐步扩充）：rest 以这些起判为数值，不抠号。
    static const char* units[] = {"mm", "cm", "km", "kN", "kg", "MPa", "kPa", "mL", "ml"};
    for (auto u : units) {
        size_t n = std::char_traits<char>::length(u);
        if (rest.compare(0, n, u) == 0) return true;
    }
    // UTF-8 per mille sign U+2030 (E2 80 B0)
    if (rest.size() >= 3 && (unsigned char)rest[0]==0xE2 && (unsigned char)rest[1]==0x80 && (unsigned char)rest[2]==0xB0) return true;
    // UTF-8 degree sign U+00B0 (C2 B0)
    if (rest.size() >= 2 && (unsigned char)rest[0]==0xC2 && (unsigned char)rest[1]==0xB0) return true;
    return false;
}

// 号后是否紧跟非 ASCII 字符（汉字/全角等）。排除 "5 个"(空格)、"5a"(ASCII) 这类非章标题。
static bool starts_with_nonascii(const std::string& s) {
    return !s.empty() && (unsigned char)s[0] >= 0x80;
}

static std::string ltrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a);
}

ClauseNoResult parse_clause_no(const std::string& text, bool is_heading) {
    ClauseNoResult out;
    std::string s = ltrim(text);

    // 多级号：至少含一个点，如 5.2、5.2.1、4.2.1-1
    static const std::regex multi(R"(^(\d+(?:\.\d+)+(?:-[0-9A-Za-z]+)?)(?=[^\d.]|$)(.*))");
    std::smatch m;
    if (std::regex_search(s, m, multi) && m.position(0) == 0) {
        std::string no = m[1].str();
        std::string rest = ltrim(m[2].str());
        if (no[0] != '0' && !rest_starts_like_unit(rest)) {
            out.matched = true; out.clause_no = no; out.rest = rest;
            return out;
        }
    }

    // 单级章号：仅在 is_heading==true 且后跟汉字时允许
    if (is_heading) {
        static const std::regex single(R"(^(\d{1,2})([^\d.].*)$)");
        if (std::regex_match(s, m, single)) {
            std::string no = m[1].str();
            std::string rest = ltrim(m[2].str());
            if (no != "0" && starts_with_nonascii(rest)) {
                out.matched = true; out.clause_no = no; out.rest = rest;
                return out;
            }
        }
    }

    // 档3：试验方法号（JTG 试验规程等），如 "T 0301—2024集料取样法" / "T0355-2000填料加热"。
    // 仅 Heading + 号后接汉字标题（排除英文/数值/材料级配如 "AC 13"——后者仅 2 位数字不匹配）。
    if (is_heading) {
        static const std::regex tmethod(R"(^([A-Za-z]{1,2})\s?(\d{3,5})((?:-|—)\d{2,4})?(.*)$)");
        if (std::regex_match(s, m, tmethod)) {
            std::string rest = ltrim(m[4].str());
            if (starts_with_nonascii(rest)) {
                std::string suffix = m[3].str();                 // "-2024" / "—2024"(全角) / ""
                if (suffix.size() >= 3 && (unsigned char)suffix[0] == 0xE2)
                    suffix = "-" + suffix.substr(3);             // 全角破折号 — 归一化为 '-'
                out.matched = true;
                out.clause_no = m[1].str() + m[2].str() + suffix; // 去内部空格 → "T0301-2024"
                out.rest = rest;
                return out;
            }
        }
    }
    return out;
}
