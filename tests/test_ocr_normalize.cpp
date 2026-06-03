#include <doctest/doctest.h>
#include "parse/ocr_normalize.h"

TEST_CASE("is_caption_label: 靠 raw_label 或前缀判图表题") {
    CHECK(is_caption_label("table_title", "表4.0.1公路技术状况等级划分标准"));
    CHECK(is_caption_label("figure_title", "图3.0.3指标体系"));
    CHECK(is_caption_label("", "表 A-1路基损坏调查表"));     // 前缀兜底
    CHECK(is_caption_label("", "续表7.5.1"));                 // 前缀兜底
    CHECK_FALSE(is_caption_label("paragraph_title", "5.2.1龟裂应按面积计算"));
}

TEST_CASE("apply_clause_extraction: 给非 caption 元素抠号、给 caption 打标") {
    std::vector<ParseElement> els(3);
    els[0].type = ElementType::Heading; els[0].raw_label = "paragraph_title";
    els[0].title = "5.2.1龟裂应按面积计算"; els[0].source = "ppstructure";
    els[1].type = ElementType::Heading; els[1].raw_label = "table_title";
    els[1].title = "表4.0.1公路技术状况等级划分标准"; els[1].source = "ppstructure";
    els[2].type = ElementType::Heading; els[2].raw_label = "paragraph_title";
    els[2].title = "1总则"; els[2].source = "ppstructure";

    apply_clause_extraction(els);

    CHECK(els[0].clause_no == "5.2.1");
    CHECK(els[0].is_caption == false);
    CHECK(els[1].is_caption == true);
    CHECK(els[1].clause_no == "");      // caption 不抠号
    CHECK(els[2].clause_no == "1");     // 单级章号
}
