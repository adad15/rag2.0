#include <doctest/doctest.h>
#include "parse/hybrid_parser.h"
#include "parse/ocr_normalize.h"
#include "parse/parse_cache.h"

// 集成测试：merge_doc（合并 poppler 基础与 OCR 元素）→ normalize_parsed_doc（M2b 全链）
// → parse_cache 往返。覆盖"仅在合并后才出现"的真实形态：
// PP-Structure 把每个 *_title 都映射成 Heading(level=1) + raw_label，混在条款流里。
TEST_CASE("M2b 集成：merge_doc -> normalize -> cache 往返") {
    // 两页都判为 OCR 页（base 页文本稀疏），OCR 元素模拟服务输出。
    ParsedDoc base;
    base.source_path = "x.pdf";
    base.pages.push_back({1, ""});
    base.pages.push_back({2, ""});

    auto H = [](int page, const std::string& label, const std::string& title){
        ParseElement e; e.type = ElementType::Heading; e.page_no = page; e.level = 1;
        e.raw_label = label; e.title = title; e.text = title;
        e.source = "ppstructure"; e.ocr_confidence = 0.9f; return e; };
    std::vector<ParseElement> ocr = {
        H(1, "paragraph_title", "1总则"),
        H(2, "table_title",     "表4.0.1等级划分"),          // 图表题：应标 caption、不抠号
        H(2, "paragraph_title", "5.2.1龟裂应按面积计算"),
    };

    ParsedDoc merged = merge_doc(base, /*ocr_pages=*/{1, 2}, ocr);
    normalize_parsed_doc(merged);

    REQUIRE(merged.elements.size() == 3);
    CHECK(merged.schema_version == 2);
    // "1总则" → 单级章号，正文起点
    CHECK(merged.elements[0].clause_no == "1");
    CHECK(merged.elements[0].region == Region::Body);
    // 表题 → caption，不抠号
    CHECK(merged.elements[1].is_caption == true);
    CHECK(merged.elements[1].clause_no == "");
    // 普通条款 → 抠到 5.2.1
    CHECK(merged.elements[2].clause_no == "5.2.1");
    CHECK(merged.elements[2].region == Region::Body);

    // 往返：归一化后的富 IR 落盘再读回，关键字段不丢
    ParsedDoc back = parsed_doc_from_json(parsed_doc_to_json(merged));
    REQUIRE(back.elements.size() == 3);
    CHECK(back.schema_version == 2);
    CHECK(back.elements[0].clause_no == "1");
    CHECK(back.elements[1].is_caption == true);
    CHECK(back.elements[1].raw_label == "table_title");
    CHECK(back.elements[2].clause_no == "5.2.1");
    CHECK(back.elements[2].region == Region::Body);
}
