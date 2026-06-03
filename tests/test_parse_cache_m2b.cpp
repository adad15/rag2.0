#include <doctest/doctest.h>
#include "parse/parse_cache.h"

TEST_CASE("parse_cache 往返保留 M2b 新字段") {
    ParsedDoc d;
    d.schema_version = 2;
    ParseElement e;
    e.type = ElementType::Heading; e.page_no = 13; e.clause_no = "5.2.1";
    e.text = "龟裂应按面积计算"; e.raw_label = "paragraph_title";
    e.region = Region::Body; e.is_caption = false; e.suspect = "short";
    e.source = "ppstructure"; e.ocr_confidence = 0.87f;
    d.elements.push_back(e);

    std::string js = parsed_doc_to_json(d);
    ParsedDoc back = parsed_doc_from_json(js);

    REQUIRE(back.elements.size() == 1);
    CHECK(back.schema_version == 2);
    CHECK(back.elements[0].raw_label == "paragraph_title");
    CHECK(back.elements[0].region == Region::Body);
    CHECK(back.elements[0].suspect == "short");
    CHECK(back.elements[0].ocr_confidence == doctest::Approx(0.87f));
}

TEST_CASE("parse_cache 容忍旧缓存（缺 M2b 字段）") {
    std::string old_js = R"({"elements":[{"type":"Text","page_no":1,"text":"x","source":"poppler"}]})";
    ParsedDoc back = parsed_doc_from_json(old_js);
    REQUIRE(back.elements.size() == 1);
    CHECK(back.elements[0].region == Region::Body);   // 默认值
    CHECK(back.elements[0].raw_label == "");
}
