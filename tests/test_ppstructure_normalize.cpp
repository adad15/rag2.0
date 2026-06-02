#include <doctest/doctest.h>
#include "parse/ppstructure_backend.h"

TEST_CASE("parse_ppstructure_json maps service JSON to elements") {
    std::string json = R"({
      "elements": [
        {"type":"Heading","page_no":91,"level":1,"title":"6 允许误差","text":"6 允许误差",
         "clause_no":"","table_html":"","caption":"","ocr_confidence":0.97},
        {"type":"Text","page_no":91,"text":"6.2 允许误差为平均值的10%。","ocr_confidence":0.95},
        {"type":"Table","page_no":50,"table_html":"<table><tr><td>96</td></tr></table>",
         "caption":"表5","ocr_confidence":1.0}
      ]
    })";
    auto els = parse_ppstructure_json(json);
    REQUIRE(els.size() == 3);
    CHECK(els[0].type == ElementType::Heading);
    CHECK(els[0].page_no == 91);
    CHECK(els[1].text.find("允许误差为平均值") != std::string::npos);
    CHECK(els[2].type == ElementType::Table);
    CHECK(els[2].table_html.find("<table>") != std::string::npos);
    CHECK(els[2].caption == "表5");
    CHECK(els[0].source == "ppstructure");
}

TEST_CASE("parse_ppstructure_json on error payload throws") {
    CHECK_THROWS(parse_ppstructure_json(R"({"error":"boom"})"));
}
