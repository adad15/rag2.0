#include <doctest/doctest.h>
#include "parse/parser_factory.h"

TEST_CASE("parse_mode_from_string maps strings") {
    CHECK(parse_mode_from_string("auto") == ParseMode::Auto);
    CHECK(parse_mode_from_string("poppler") == ParseMode::Poppler);
    CHECK(parse_mode_from_string("ocr") == ParseMode::Ocr);
    CHECK(parse_mode_from_string("???") == ParseMode::Auto);  // 兜底 auto
}

TEST_CASE("make_ocr_backend builds ppstructure, throws for unimplemented") {
    auto b = make_ocr_backend("ppstructure", "http://localhost:8001");
    CHECK(b != nullptr);
    CHECK_THROWS(make_ocr_backend("mineru", "x"));
    CHECK_THROWS(make_ocr_backend("vlapi", "x"));
    CHECK_THROWS(make_ocr_backend("tesseract", "x"));
}
