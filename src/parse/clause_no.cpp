#include "parse/clause_no.h"
#include <regex>

// 单位词/符号前缀：rest 以这些开头则判为数值，不当条款号。
static bool rest_starts_like_unit(const std::string& rest) {
    if (rest.empty()) return false;
    static const std::string unit_bytes = "%";
    if (unit_bytes.find(rest[0]) != std::string::npos) return true;
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

static bool starts_with_cjk(const std::string& s) {
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
            if (no != "0" && starts_with_cjk(rest)) {
                out.matched = true; out.clause_no = no; out.rest = rest;
                return out;
            }
        }
    }
    return out;
}
