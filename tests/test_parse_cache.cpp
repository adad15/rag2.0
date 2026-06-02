#include <doctest/doctest.h>
#include "parse/parse_cache.h"

TEST_CASE("parsed_doc JSON round-trip preserves pages and elements") {
    ParsedDoc d;
    d.source_path = "C:/a.pdf";
    d.title = "a.pdf";
    d.standard_no = "JTG 3432-2024";
    { ParsedPage p; p.page_no = 1; p.text = "\xe7\xac\xac\xe4\xb8\x80\xe9\xa1\xb5\xe6\x96\x87\xe6\x9c\xac"; d.pages.push_back(p); }
    { ParseElement e; e.type = ElementType::Table; e.page_no = 1;
      e.table_html = "<table><tr><td>x</td></tr></table>"; e.caption = "\xe8\xa1\xa8\x31";
      e.source = "ppstructure"; e.ocr_confidence = 0.9f; d.elements.push_back(e); }

    std::string json = parsed_doc_to_json(d);
    ParsedDoc r = parsed_doc_from_json(json);

    CHECK(r.source_path == std::string("C:/a.pdf"));
    CHECK(r.standard_no == std::string("JTG 3432-2024"));
    REQUIRE(r.pages.size() == 1);
    CHECK(r.pages[0].text == std::string("\xe7\xac\xac\xe4\xb8\x80\xe9\xa1\xb5\xe6\x96\x87\xe6\x9c\xac"));
    REQUIRE(r.elements.size() == 1);
    CHECK(r.elements[0].type == ElementType::Table);
    CHECK(r.elements[0].table_html.find("<table>") != std::string::npos);
    CHECK(r.elements[0].caption == std::string("\xe8\xa1\xa8\x31"));
    CHECK(r.elements[0].source == std::string("ppstructure"));
}
