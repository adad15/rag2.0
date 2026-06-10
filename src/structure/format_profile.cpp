#include "structure/format_profile.h"

#include <regex>

namespace {

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

bool is_n0k(const std::string& number) {
    static const std::regex re(R"(^\d+\.0\.\d+(?:-[0-9A-Za-z]+)?$)");
    return std::regex_match(number, re);
}

}  // namespace

int decimal_depth(const std::string& number) {
    if (number.empty()) return -1;
    static const std::regex re(R"(^\d+(?:\.\d+)*(?:-[0-9A-Za-z]+)?$)");
    if (!std::regex_match(number, re)) return -1;

    int dots = 0;
    for (char c : number) {
        if (c == '-') break;
        if (c == '.') ++dots;
    }
    return dots;
}

bool is_test_number(const std::string& s) {
    static const std::regex re(R"(^\s*T\s*\d{4}\s*-\s*\d{4})");
    return std::regex_search(normalize_dashes(s), re);
}

int level_for(FormatProfile profile, const std::string& number, bool in_test_scope) {
    if (profile == FormatProfile::B_testno && is_test_number(number)) return 2;

    int d = decimal_depth(number);
    if (d < 0) return 0;
    if (profile == FormatProfile::B_testno && in_test_scope) return d + 3;
    if (is_n0k(number)) return d;
    return d + 1;
}

int retrieval_depth(FormatProfile /*profile*/) {
    return 3;
}

FormatProfile profile_from_string(const std::string& s) {
    return s == "B_testno" ? FormatProfile::B_testno : FormatProfile::A_decimal;
}

std::string profile_to_string(FormatProfile p) {
    return p == FormatProfile::B_testno ? "B_testno" : "A_decimal";
}
