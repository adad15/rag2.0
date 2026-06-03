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

TEST_CASE("is_english_garble: 纯英文糊判 true、含汉字判 false") {
    CHECK(is_english_garble("sessmentSta"));
    CHECK(is_english_garble("highwamintenanceulitinicar"));
    CHECK_FALSE(is_english_garble("总则"));
    CHECK_FALSE(is_english_garble("2.0.1公路技术状况指数"));   // 含成段汉字
    CHECK_FALSE(is_english_garble(""));
}

TEST_CASE("tag_regions: 按 目次/首章/附录/条文说明 切区域") {
    auto H = [](const std::string& t){ ParseElement e; e.type=ElementType::Heading; e.title=t; e.source="ppstructure"; return e; };
    std::vector<ParseElement> els = {
        H("公路技术状况评定标准"),   // front_matter
        H("目次"),                   // -> toc
        H("5.2 沥青路面 …… 13"),     // toc 内
        H("1总则"),                  // -> body（首章）
        H("5.2.1龟裂"),              // body
        H("附录A 调查表"),           // -> appendix
        H("条文说明"),               // -> explanation
        H("3.2 本规程"),             // explanation 内
    };
    apply_clause_extraction(els);
    tag_regions(els);

    CHECK(els[0].region == Region::FrontMatter);
    CHECK(els[1].region == Region::Toc);
    CHECK(els[2].region == Region::Toc);
    CHECK(els[3].region == Region::Body);
    CHECK(els[4].region == Region::Body);
    CHECK(els[5].region == Region::Appendix);
    CHECK(els[6].region == Region::Explanation);
    CHECK(els[7].region == Region::Explanation);
}

TEST_CASE("flag_anomalies: 跳号标 seq、正文过短标 short") {
    auto C = [](const std::string& no, const std::string& txt){
        ParseElement e; e.type=ElementType::Heading; e.clause_no=no; e.text=txt;
        e.region=Region::Body; e.source="ppstructure"; return e; };
    std::vector<ParseElement> els = {
        C("7.1","一般规定"), C("7.2","评定方法说明充分"),
        C("7.33","路基技术状况评定"),   // 7.2 后跳到 7.33 -> seq
        C("7.4.9","算："),              // 正文过短 -> short
    };
    flag_anomalies(els);
    CHECK(els[2].suspect == "seq");
    CHECK(els[3].suspect == "short");
    CHECK(els[0].suspect == "");
}

TEST_CASE("flag_anomalies: 跨父级同深度不误报 seq") {
    auto C = [](const std::string& no){ ParseElement e; e.type=ElementType::Heading;
        e.clause_no=no; e.text="xxxxxxxxxx"; e.region=Region::Body; e.source="ppstructure"; return e; };
    std::vector<ParseElement> els = { C("5.3"), C("6.1") };   // 不同父级(5 vs 6)，不该标 seq
    flag_anomalies(els);
    CHECK(els[1].suspect == "");
}

TEST_CASE("normalize_parsed_doc: 丢弃独立英文糊块、跑全链") {
    ParsedDoc d;
    auto add = [&](ElementType ty, const std::string& label, const std::string& title){
        ParseElement e; e.type=ty; e.raw_label=label; e.title=title; e.source="ppstructure";
        d.elements.push_back(e); };
    add(ElementType::Heading, "doc_title", "sessmentSta");        // 英文糊 -> 丢
    add(ElementType::Heading, "paragraph_title", "1总则");
    add(ElementType::Heading, "paragraph_title", "5.2.1龟裂应按面积计算");
    add(ElementType::Heading, "table_title", "表4.0.1等级划分");   // caption

    normalize_parsed_doc(d);

    REQUIRE(d.elements.size() == 3);                  // 英文糊被丢
    CHECK(d.elements[0].clause_no == "1");
    CHECK(d.elements[1].clause_no == "5.2.1");
    CHECK(d.elements[2].is_caption == true);
}
