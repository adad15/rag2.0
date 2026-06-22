#include <doctest/doctest.h>
#include "query/query_planner.h"

TEST_CASE("parse_planner_mode maps strings, unknown -> Auto") {
    CHECK(parse_planner_mode("rule") == PlannerMode::Rule);
    CHECK(parse_planner_mode("llm")  == PlannerMode::Llm);
    CHECK(parse_planner_mode("auto") == PlannerMode::Auto);
    CHECK(parse_planner_mode("")     == PlannerMode::Auto);
    CHECK(parse_planner_mode("xyz")  == PlannerMode::Auto);
}

TEST_CASE("parse_llm_plan reads a valid ListByCondition plan") {
    auto p = parse_llm_plan(R"({"intent":"ListByCondition","key_terms":["天平"],
        "section_hints":["仪具","材料"],"sparse_text":"天平 仪具 材料","dense_text":"使用天平的试验"})");
    REQUIRE(p.has_value());
    CHECK(p->intent == QueryIntent::ListByCondition);
    REQUIRE(p->key_terms.size() == 1);
    CHECK(p->key_terms[0] == "天平");
    REQUIRE(p->section_hints.size() == 2);
    CHECK(p->sparse_text == "天平 仪具 材料");
}

TEST_CASE("parse_llm_plan accepts GeneralFact with empty fields") {
    auto p = parse_llm_plan(R"({"intent":"GeneralFact","key_terms":[],"section_hints":[],
        "sparse_text":"","dense_text":""})");
    REQUIRE(p.has_value());
    CHECK(p->intent == QueryIntent::GeneralFact);
    CHECK(p->key_terms.empty());
}

TEST_CASE("parse_llm_plan rejects garbage / bad intent / self-contradiction") {
    CHECK_FALSE(parse_llm_plan("not json").has_value());
    CHECK_FALSE(parse_llm_plan("[1,2]").has_value());
    CHECK_FALSE(parse_llm_plan(R"({"intent":"Nonsense"})").has_value());
    CHECK_FALSE(parse_llm_plan(R"({"intent":"ListByCondition","key_terms":[]})").has_value());
}

TEST_CASE("normalize_question trims, collapses whitespace, lowercases ASCII") {
    CHECK(normalize_question("  JTG  3420  里  天平 ") == "jtg 3420 里 天平");
    CHECK(normalize_question("天平") == "天平");
}

TEST_CASE("normalize_question handles empty and all-whitespace") {
    CHECK(normalize_question("") == "");
    CHECK(normalize_question("   \t \n ") == "");
}

TEST_CASE("parse_llm_plan defaults missing sparse/dense to empty") {
    auto p = parse_llm_plan(R"({"intent":"GeneralFact"})");
    REQUIRE(p.has_value());
    CHECK(p->sparse_text.empty());
    CHECK(p->dense_text.empty());
    CHECK(p->key_terms.empty());
}

TEST_CASE("parse_llm_plan skips non-string elements in key_terms") {
    auto p = parse_llm_plan(R"({"intent":"ListByCondition","key_terms":[1,"天平",null,"烘箱"]})");
    REQUIRE(p.has_value());
    REQUIRE(p->key_terms.size() == 2);
    CHECK(p->key_terms[0] == "天平");
    CHECK(p->key_terms[1] == "烘箱");
}

#include <atomic>
#include <filesystem>

// 构造一个用临时缓存目录、注入 llm_call 的 planner。
static QueryPlanner make_test_planner(LlmCall fn, PlannerMode mode = PlannerMode::Llm) {
    QueryPlanner p;
    p.mode = mode;
    p.llm_call = std::move(fn);
    p.cache_dir = (std::filesystem::temp_directory_path() /
                   ("qp_test_" + std::to_string(::time(nullptr)) + "_" +
                    std::to_string(rand()))).string();
    return p;
}

TEST_CASE("QueryPlanner uses LLM output for a no-code question") {
    auto p = make_test_planner([](const std::string&, const std::string&) {
        return std::string(R"({"intent":"ListByCondition","key_terms":["马歇尔"],
            "section_hints":["仪具","材料"],"sparse_text":"马歇尔 仪具 材料","dense_text":"使用马歇尔的试验"})");
    });
    QueryAnalysis a = p.plan("哪些试验用到马歇尔");
    CHECK(a.intent == QueryIntent::ListByCondition);
    REQUIRE(a.key_terms.size() == 1);
    CHECK(a.key_terms[0] == "马歇尔");
    CHECK(a.sparse_text == "马歇尔 仪具 材料");
    std::filesystem::remove_all(p.cache_dir);
}

TEST_CASE("QueryPlanner falls back to rule on garbage LLM output") {
    auto p = make_test_planner([](const std::string&, const std::string&) {
        return std::string("garbage not json");
    });
    p.terms.instruments = {"天平"};
    QueryAnalysis a = p.plan("哪些试验用到天平");
    CHECK(a.intent == QueryIntent::ListByCondition);
    REQUIRE(a.key_terms.size() == 1);
    CHECK(a.key_terms[0] == "天平");
    std::filesystem::remove_all(p.cache_dir);
}

TEST_CASE("QueryPlanner caches: identical question calls LLM only once") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto p = make_test_planner([calls](const std::string&, const std::string&) {
        (*calls)++;
        return std::string(R"({"intent":"GeneralFact","key_terms":[],"section_hints":[],"sparse_text":"","dense_text":""})");
    });
    p.plan("某个普通问题");
    p.plan("  某个普通问题 ");
    CHECK(*calls == 1);
    std::filesystem::remove_all(p.cache_dir);
}

TEST_CASE("QueryPlanner: numbered question bypasses LLM (rule path)") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto p = make_test_planner([calls](const std::string&, const std::string&) {
        (*calls)++; return std::string("{}");
    });
    QueryAnalysis a = p.plan("T0302 需要哪些仪具");
    CHECK(a.intent == QueryIntent::MethodLookup);
    CHECK(*calls == 0);
    std::filesystem::remove_all(p.cache_dir);
}

TEST_CASE("QueryPlanner rule mode never calls LLM") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto p = make_test_planner([calls](const std::string&, const std::string&) {
        (*calls)++; return std::string("{}");
    }, PlannerMode::Rule);
    p.plan("哪些试验用到天平");
    CHECK(*calls == 0);
    std::filesystem::remove_all(p.cache_dir);
}
