#include <doctest/doctest.h>
#include "structure/region_segmenter.h"

static ParseElement region_el(Region r, const std::string& text) {
    ParseElement e;
    e.region = r;
    e.text = text;
    return e;
}

TEST_CASE("segment_regions skips front matter and TOC") {
    ParsedDoc d;
    d.elements = {
        region_el(Region::FrontMatter, "cover"),
        region_el(Region::Toc, "contents"),
        region_el(Region::Body, "5.1.1 body one"),
        region_el(Region::Body, "5.1.2 body two"),
        region_el(Region::Explanation, "5.1.1 explanation"),
        region_el(Region::Appendix, "appendix A"),
    };

    auto groups = segment_regions(d);

    REQUIRE(groups.size() == 3);
    CHECK(groups[0].region == Region::Body);
    CHECK(groups[0].elements.size() == 2);
    CHECK(groups[1].region == Region::Explanation);
    CHECK(groups[2].region == Region::Appendix);
}

TEST_CASE("segment_regions promotes appendix-tagged explanation marker to explanation") {
    ParsedDoc d;
    d.elements = {
        region_el(Region::Appendix, "附录C 路面弯沉标准值计算方法"),
        region_el(Region::Appendix, "C.0.4公路沥青路面结构性修复设计年限应根据设计文件确定。"),
        region_el(Region::Appendix, "条文说明"),
        region_el(Region::Appendix, "1总则"),
        region_el(Region::Appendix, "1.0.1本标准属于现行公路工程标准体系。"),
    };

    auto groups = segment_regions(d);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].region == Region::Appendix);
    CHECK(groups[0].elements.size() == 2);
    CHECK(groups[1].region == Region::Explanation);
    CHECK(groups[1].elements.size() == 3);
}
