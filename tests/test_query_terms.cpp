#include <doctest/doctest.h>
#include "query/query_terms.h"
#include <fstream>
#include <cstdio>

TEST_CASE("match_terms returns dict entries that are substrings, dict order, deduped") {
    std::vector<std::string> dict = {"天平", "烘箱", "试验筛"};
    auto out = match_terms("哪些混凝土试验用到了天平", dict);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "天平");
}

TEST_CASE("match_terms keeps a longer term intact (no substring corruption)") {
    // 提取式：'试验筛' 整体命中保留，不会被切成 '筛'
    std::vector<std::string> dict = {"试验筛"};
    auto out = match_terms("负压筛法用到试验筛", dict);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "试验筛");
}

TEST_CASE("load_query_terms parses a sectioned file") {
    const char* path = "test_query_terms_fixture.txt";
    {
        std::ofstream f(path, std::ios::binary);
        f << "[instrument]\n天平\n# comment\n烘箱\n[section]\n仪具\n";
    }
    QueryTerms t = load_query_terms(path);
    std::remove(path);
    REQUIRE(t.instruments.size() == 2);
    CHECK(t.instruments[0] == "天平");
    CHECK(t.instruments[1] == "烘箱");
    REQUIRE(t.sections.size() == 1);
    CHECK(t.sections[0] == "仪具");
}

TEST_CASE("load_query_terms returns empty on missing file") {
    QueryTerms t = load_query_terms("definitely_missing_file_xyz.txt");
    CHECK(t.instruments.empty());
    CHECK(t.sections.empty());
}
