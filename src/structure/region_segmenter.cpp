#include "structure/region_segmenter.h"

namespace {

bool is_content_region(Region r) {
    return r == Region::Body || r == Region::Explanation || r == Region::Appendix;
}

std::string display_text(const ParseElement& e) {
    if (!e.text.empty()) return e.text;
    if (!e.title.empty()) return e.title;
    return e.caption;
}

std::string trim_ascii_space(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool is_explanation_marker(const ParseElement& e) {
    static const std::string explanation = "\xE6\x9D\xA1\xE6\x96\x87\xE8\xAF\xB4\xE6\x98\x8E";
    return trim_ascii_space(display_text(e)) == explanation;
}

}  // namespace

std::vector<RegionGroup> segment_regions(const ParsedDoc& doc) {
    std::vector<RegionGroup> groups;
    bool force_explanation_after_appendix_marker = false;

    for (const auto& e : doc.elements) {
        if (!is_content_region(e.region)) continue;

        Region region = e.region;
        if (e.region == Region::Appendix && is_explanation_marker(e)) {
            force_explanation_after_appendix_marker = true;
        }
        if (force_explanation_after_appendix_marker && e.region == Region::Appendix) {
            region = Region::Explanation;
        }

        if (groups.empty() || groups.back().region != region) {
            groups.push_back(RegionGroup{region, {}});
        }
        groups.back().elements.push_back(&e);
    }
    return groups;
}
