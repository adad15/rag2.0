#include "parse/method_no.h"
#include <regex>

namespace {

// 把 UTF-8 全角破折号（U+2014 em / U+2013 en）规整为 ASCII '-'。
std::string normalize_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 2 < s.size() &&
            static_cast<unsigned char>(s[i]) == 0xE2 &&
            static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0x94 ||
             static_cast<unsigned char>(s[i + 2]) == 0x93)) {
            out += '-';
            i += 3;
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string remove_ascii_spaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\t') out += c;
    }
    return out;
}

}  // namespace

std::string extract_method_no(const std::string& text) {
    // 年份段可选：入库标题 "T 0302-2024" 贪婪匹配到完整带年份；
    // 查询 "T0302" 仅匹配前缀。
    static const std::regex re(R"(T\s*\d{4}(?:\s*-\s*\d{4})?)");
    std::smatch m;
    std::string normalized = normalize_dashes(text);
    if (!std::regex_search(normalized, m, re)) return "";
    return remove_ascii_spaces(m.str(0));
}
