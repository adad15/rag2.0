#include "eval/generation_metrics.h"
#include <cctype>

std::string normalize_for_match(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        // 跳过 ASCII 空白
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        // 全角数字 U+FF10..U+FF19，UTF-8 = EF BC 90 .. EF BC 99
        if (c == 0xEF && i + 2 < s.size() &&
            static_cast<unsigned char>(s[i+1]) == 0xBC) {
            unsigned char c3 = static_cast<unsigned char>(s[i+2]);
            if (c3 >= 0x90 && c3 <= 0x99) { out += static_cast<char>('0' + (c3 - 0x90)); i += 3; continue; }
        }
        if (c < 0x80) out += static_cast<char>(std::tolower(c));
        else out += static_cast<char>(c);
        ++i;
    }
    return out;
}

bool citation_hit(const std::string& answer,
                  const std::string& gold_standard_code,
                  const std::string& gold_ref) {
    if (gold_ref.empty()) return false;
    std::string a = normalize_for_match(answer);
    std::string ref = normalize_for_match(gold_ref);
    if (a.find(ref) == std::string::npos) return false;
    if (!gold_standard_code.empty()) {
        std::string sc = normalize_for_match(gold_standard_code);
        if (a.find(sc) == std::string::npos) return false;
    }
    return true;
}

int count_value_hits(const std::string& answer,
                     const std::vector<std::string>& gold_values) {
    std::string a = normalize_for_match(answer);
    int n = 0;
    for (const auto& v : gold_values) {
        std::string nv = normalize_for_match(v);
        if (!nv.empty() && a.find(nv) != std::string::npos) ++n;
    }
    return n;
}
