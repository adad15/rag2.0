#include <doctest/doctest.h>
#include "ingest/clause_splitter.h"

TEST_CASE("split_clauses extracts clause_no and text by numeric headers") {
    std::string page =
        "4.2.1 桥涵设计应符合本规范的规定。\n"
        "4.2.2 设计洪水频率应按表4.2.2取值。\n"
        "4.3.1 荷载组合应符合下列要求。\n";
    auto clauses = split_clauses(page, /*page_no=*/12);

    REQUIRE(clauses.size() == 3);
    CHECK(clauses[0].clause_no == "4.2.1");
    CHECK(clauses[0].text.find("桥涵设计应符合") != std::string::npos);
    CHECK(clauses[0].page_start == 12);
    CHECK(clauses[1].clause_no == "4.2.2");
    CHECK(clauses[2].clause_no == "4.3.1");
}

TEST_CASE("split_clauses ignores lines without a leading clause number") {
    std::string page = "前言\n本规范由交通运输部提出。\n1.0.1 为规范设计，制定本规范。\n";
    auto clauses = split_clauses(page, 1);
    REQUIRE(clauses.size() == 1);
    CHECK(clauses[0].clause_no == "1.0.1");
}

TEST_CASE("split_clauses appends continuation lines to current clause") {
    std::string page = "4.2.1 第一行。\n续行内容仍属于4.2.1。\n4.2.2 下一条。\n";
    auto clauses = split_clauses(page, 5);
    REQUIRE(clauses.size() == 2);
    CHECK(clauses[0].text.find("续行内容") != std::string::npos);
}
