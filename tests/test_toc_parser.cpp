#include <doctest/doctest.h>
#include "structure/toc_parser.h"

static ParseElement toc_el(Region r, const std::string& text) {
    ParseElement e;
    e.region = r;
    e.text = text;
    return e;
}

TEST_CASE("toc detection returns A_decimal for decimal entries") {
    ParsedDoc d;
    d.elements.push_back(toc_el(Region::Toc, "contents"));
    d.elements.push_back(toc_el(Region::Toc, "1 general\n5 damage\n5.1 subgrade\n5.2 pavement"));

    TocResult r = detect_format_from_toc(d);

    CHECK(r.detected);
    CHECK(r.profile == FormatProfile::A_decimal);
}

TEST_CASE("toc detection returns B_testno for method numbers") {
    ParsedDoc d;
    d.elements.push_back(toc_el(Region::Toc,
        "4 aggregate tests\nT 0302—2024 sieve analysis\nT 0306—1994 moisture test"));

    TocResult r = detect_format_from_toc(d);

    CHECK(r.detected);
    CHECK(r.profile == FormatProfile::B_testno);
}

TEST_CASE("toc detection reports undetected when TOC is absent") {
    ParsedDoc d;
    d.elements.push_back(toc_el(Region::Body, "5.1.1 body"));

    TocResult r = detect_format_from_toc(d);

    CHECK_FALSE(r.detected);
}

TEST_CASE("body fallback detects method profile") {
    ParsedDoc b;
    b.elements.push_back(toc_el(Region::Body, "T 0306—1994 moisture test"));
    CHECK(detect_format_from_body(b) == FormatProfile::B_testno);

    ParsedDoc a;
    a.elements.push_back(toc_el(Region::Body, "5.1.1 subgrade damage"));
    CHECK(detect_format_from_body(a) == FormatProfile::A_decimal);
}
