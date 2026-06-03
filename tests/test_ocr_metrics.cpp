#include <doctest/doctest.h>
#include "parse/ocr_metrics.h"

TEST_CASE("compute_ocr_metrics: 基本计数与填充率") {
    ParsedDoc d;
    auto mk = [&](Region r, ElementType ty, const std::string& title,
                  const std::string& no, bool cap, const std::string& sus, float conf){
        ParseElement e; e.region=r; e.type=ty; e.title=title; e.clause_no=no;
        e.is_caption=cap; e.suspect=sus; e.ocr_confidence=conf; e.source="ppstructure";
        d.elements.push_back(e); };
    // 3 个 body 数字开头标题做候选（含 7.4.9），其中 2 个抠到号 -> 填充率 2/3
    mk(Region::Body, ElementType::Heading, "5.2.1龟裂", "5.2.1", false, "", 0.9f);
    mk(Region::Body, ElementType::Heading, "7总则坏行", "",      false, "", 0.8f);
    // caption 泄漏：title 以"表"起却带了 clause_no
    mk(Region::Body, ElementType::Heading, "表4.0.1划分", "4.0.1", false, "", 0.95f);
    // 一个 suspect
    mk(Region::Body, ElementType::Heading, "7.4.9算：", "7.4.9", false, "short", 0.7f);

    OcrMetrics m = compute_ocr_metrics(d);
    CHECK(m.body_candidate == 3);    // 3 个"数字开头"的 body 标题（"表4.0.1"以表起，不算候选）
    CHECK(m.body_filled == 2);
    CHECK(m.caption_leak == 1);
    CHECK(m.suspect == 1);
    CHECK(m.all_conf_one == false);
}

TEST_CASE("compute_ocr_metrics: toc 计数 / 置信度全1 / format 冒烟") {
    ParsedDoc d;
    ParseElement a; a.region=Region::Toc; a.type=ElementType::Heading; a.title="目次";
    a.source="ppstructure"; a.ocr_confidence=1.0f;
    ParseElement b; b.region=Region::Body; b.type=ElementType::Heading; b.title="1总则";
    b.clause_no="1"; b.source="ppstructure"; b.ocr_confidence=1.0f;
    d.elements.push_back(a); d.elements.push_back(b);

    OcrMetrics m = compute_ocr_metrics(d);
    CHECK(m.toc_count == 1);
    CHECK(m.all_conf_one == true);                       // 两个都是 1.0
    CHECK(m.conf_mean == doctest::Approx(1.0));
    std::string s = format_ocr_metrics(m);
    CHECK(!s.empty());
    CHECK(s.find("OCR") != std::string::npos);
}
