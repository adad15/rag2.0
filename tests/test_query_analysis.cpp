#include <doctest/doctest.h>
#include "query/query_analysis.h"
#include "query/query_terms.h"

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

TEST_CASE("analyze_query classifies a list-by-condition question") {
    QueryAnalysis a = analyze_query("公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平");
    CHECK(a.intent == QueryIntent::ListByCondition);
    CHECK(a.standard_code.empty());
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
}

TEST_CASE("method number outranks list marker for intent") {
    QueryAnalysis a = analyze_query("T0302 需要哪些仪具");
    CHECK(a.intent == QueryIntent::MethodLookup);
}

TEST_CASE("plain question is GeneralFact") {
    QueryAnalysis a = analyze_query("路基沉降怎么评定");
    CHECK(a.intent == QueryIntent::GeneralFact);
}

TEST_CASE("build_query_plan rewrites a list query to key terms + injected section hints") {
    QueryTerms terms;
    terms.instruments = {"天平"};
    // sections 留空 → 触发默认章节注入
    QueryAnalysis a = build_query_plan(
        "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平", terms);
    CHECK(a.intent == QueryIntent::ListByCondition);
    REQUIRE(a.key_terms.size() == 1);
    CHECK(a.key_terms[0] == "天平");
    REQUIRE(a.section_hints.size() == 2);
    CHECK(a.section_hints[0] == "仪具");
    CHECK(a.section_hints[1] == "材料");
    CHECK(a.sparse_text == "天平 仪具 材料");
    CHECK(a.dense_text == "查找试验方法中仪具和材料包含天平的段落");
}

TEST_CASE("build_query_plan leaves non-list queries on clean_text fallback") {
    QueryTerms terms; terms.instruments = {"天平"};
    QueryAnalysis a = build_query_plan("路基沉降怎么评定", terms);
    CHECK(a.intent == QueryIntent::GeneralFact);
    CHECK(a.sparse_text.empty());
    CHECK(a.dense_text.empty());
}

TEST_CASE("build_query_plan does not rewrite when no instrument matched") {
    QueryTerms terms; terms.instruments = {"天平"};
    QueryAnalysis a = build_query_plan("哪些试验需要养护", terms);
    CHECK(a.intent == QueryIntent::ListByCondition);
    CHECK(a.key_terms.empty());
    CHECK(a.sparse_text.empty());   // 回退
    CHECK(a.dense_text.empty());
}

TEST_CASE("build_query_plan uses caller-supplied section hint, no injection") {
    QueryTerms terms;
    terms.instruments = {"天平"};
    terms.sections    = {"仪具"};
    // 查询里本身含"仪具" → section_hints 由匹配得到，不触发默认注入
    QueryAnalysis a = build_query_plan("哪些试验的仪具用到天平", terms);
    CHECK(a.intent == QueryIntent::ListByCondition);
    REQUIRE(a.section_hints.size() == 1);
    CHECK(a.section_hints[0] == "仪具");
    CHECK(a.sparse_text == "天平 仪具");
    CHECK(a.dense_text  == "查找试验方法中仪具包含天平的段落");
}
