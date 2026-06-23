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

TEST_CASE("extract_standard_no extracts a number embedded mid-sentence") {
    CHECK(extract_standard_no("本规范应与 GB 50010-2010 配合使用。", "fb") == "GB 50010-2010");
}

TEST_CASE("extract_standard_no handles the JTG/T slash-T form") {
    CHECK(extract_standard_no("JTG/T D70-2010 公路隧道设计规范", "fb") == "JTG/T D70-2010");
}

TEST_CASE("normalize_standard_code unifies slash/dash/space/underscore variants") {
    // 官方代号(带斜杠+半角连字符) 与 文件名退化形(无斜杠+全角破折号) 归一应一致
    CHECK(normalize_standard_code("JTG/T 3650-2020") == normalize_standard_code("JTGT 3650—2020"));
    // OCR 封面形(空格→下划线, '-'→全角破折号) 与 干净形 应一致
    CHECK(normalize_standard_code("JTG_3432—2024") == normalize_standard_code("JTG 3432-2024"));
    // 嵌在全称括号里, 归一化后裸代号应为子串(find_standard_by_code 据此匹配)
    std::string full = normalize_standard_code("《公路桥涵施工技术规范》(JTGT 3650—2020）");
    CHECK(full.find(normalize_standard_code("JTG/T 3650-2020")) != std::string::npos);
}
