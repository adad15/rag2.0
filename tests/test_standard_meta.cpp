#include <doctest/doctest.h>
#include "ingest/standard_meta.h"

TEST_CASE("extract_standard_no pulls a JTG number from cover text") {
    std::string page =
        "中华人民共和国行业标准\n"
        "公路桥涵设计通用规范\n"
        "JTG D60-2015\n"
        "2015-09-01 发布\n";
    CHECK(extract_standard_no(page, "fallback") == "JTG D60-2015");
}

TEST_CASE("extract_standard_no handles GB and GB/T forms") {
    CHECK(extract_standard_no("GB 50010-2010 混凝土结构设计规范", "fb") == "GB 50010-2010");
    CHECK(extract_standard_no("GB/T 50081-2019 普通混凝土力学性能试验方法", "fb")
          == "GB/T 50081-2019");
}

TEST_CASE("extract_standard_no falls back when no number is present") {
    CHECK(extract_standard_no("前言 本规范由交通运输部提出。", "公路桥涵设计通用规范")
          == "公路桥涵设计通用规范");
}
