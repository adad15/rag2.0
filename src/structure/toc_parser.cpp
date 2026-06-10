#include "structure/toc_parser.h"

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

bool contains_test_number(const std::string& s) {
    static const std::regex re(R"(T\s*\d{4}\s*-\s*\d{4})");
    return std::regex_search(normalize_dashes(s), re);
}

bool contains_decimal_entry(const std::string& s) {
    static const std::regex re(R"(\d+\.\d+)");
    return std::regex_search(s, re);
}

std::string element_text(const ParseElement& e) {
    std::string out = e.clause_no;
    out += "\n";
    out += e.title;
    out += "\n";
    out += e.text;
    return out;
}

}  // namespace

TocResult detect_format_from_toc(const ParsedDoc& doc) {
    std::string toc;
    for (const auto& e : doc.elements) {
        if (e.region == Region::Toc) {
            toc += element_text(e);
            toc += "\n";
        }
    }

    TocResult r;
    if (toc.empty()) return r;
    if (contains_test_number(toc)) {
        r.detected = true;
        r.profile = FormatProfile::B_testno;
        return r;
    }
    if (contains_decimal_entry(toc)) {
        r.detected = true;
        r.profile = FormatProfile::A_decimal;
        return r;
    }
    return r;
}

FormatProfile detect_format_from_body(const ParsedDoc& doc) {
    for (const auto& e : doc.elements) {
        if (contains_test_number(element_text(e))) return FormatProfile::B_testno;
    }
    return FormatProfile::A_decimal;
}
