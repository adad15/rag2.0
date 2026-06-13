#include <doctest/doctest.h>
#include "query/query_analysis.h"

TEST_CASE("analyze_query extracts and normalizes a standard code with year") {
    QueryAnalysis a = analyze_query("JTC 5210-2018 第5.1.2条是什么规定");
    CHECK(a.standard_code == "JTC5210-2018");
    CHECK(a.clause_no == "5.1.2");
    CHECK(a.method_no.empty());
    CHECK(a.clean_text == "JTC 5210-2018 第5.1.2条是什么规定");
}

TEST_CASE("analyze_query accepts a year-less standard code") {
    QueryAnalysis a = analyze_query("JTG 3420 里水泥怎么取样");
    CHECK(a.standard_code == "JTG3420");
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
}

TEST_CASE("analyze_query extracts a method number, not a standard code") {
    QueryAnalysis a = analyze_query("T0302 需要哪些仪具");
    CHECK(a.method_no == "T0302");
    CHECK(a.standard_code.empty());
    CHECK(a.clause_no.empty());
}

TEST_CASE("analyze_query leaves all codes empty for a plain question") {
    QueryAnalysis a = analyze_query("路基沉降怎么评定");
    CHECK(a.standard_code.empty());
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
    CHECK(a.clean_text == "路基沉降怎么评定");
}

TEST_CASE("analyze_query does not treat a single-level number as a clause") {
    QueryAnalysis a = analyze_query("第5章讲了什么");
    CHECK(a.clause_no.empty());
}
