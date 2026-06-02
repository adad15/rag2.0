#include <doctest/doctest.h>
#include "parse/hybrid_parser.h"

TEST_CASE("pick_ocr_pages: auto mode picks pages below threshold") {
    std::vector<int> chars = {2000, 6, 1500};   // 第2页稀疏（1-based）
    auto pages = pick_ocr_pages(chars, ParseMode::Auto, 100);
    REQUIRE(pages.size() == 1);
    CHECK(pages[0] == 2);
}
TEST_CASE("pick_ocr_pages: poppler mode picks none, ocr mode picks all") {
    std::vector<int> chars = {2000, 6, 1500};
    CHECK(pick_ocr_pages(chars, ParseMode::Poppler, 100).empty());
    CHECK(pick_ocr_pages(chars, ParseMode::Ocr, 100).size() == 3);
}

TEST_CASE("merge_doc: ocr pages replaced, poppler pages kept, elements merged") {
    ParsedDoc base;
    base.source_path = "C:/a.pdf";
    { ParsedPage p; p.page_no=1; p.text="文字页一"; base.pages.push_back(p); }
    { ParsedPage p; p.page_no=2; p.text="";        base.pages.push_back(p); } // 扫描页(空)
    std::vector<int> ocr_pages = {2};
    std::vector<ParseElement> ocr_els;
    { ParseElement e; e.type=ElementType::Text; e.page_no=2; e.text="OCR出来的二页"; ocr_els.push_back(e); }
    { ParseElement e; e.type=ElementType::Table; e.page_no=2; e.table_html="<table></table>"; ocr_els.push_back(e); }

    ParsedDoc out = merge_doc(base, ocr_pages, ocr_els);

    REQUIRE(out.pages.size() == 2);
    CHECK(out.pages[0].text == "文字页一");
    CHECK(out.pages[1].text.find("OCR出来的二页") != std::string::npos);
    int p1=0,p2=0; for (auto& e: out.elements){ if(e.page_no==1)++p1; if(e.page_no==2)++p2; }
    CHECK(p1 == 1);   // 第1页：1个 poppler Text 元素
    CHECK(p2 == 2);   // 第2页：2个 ocr 元素
}
