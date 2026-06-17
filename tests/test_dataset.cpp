#include <doctest/doctest.h>
#include <stdexcept>
#include "eval/dataset.h"

TEST_CASE("parse_dataset reads point-query and coverage cases") {
    std::string json = R"([
      {"question":"T0521 需要哪些仪具","gold_method_no":"T0521-2005","note":"method"},
      {"question":"哪些试验用到天平","gold_methods":["T0502-2005","T0590-2020"],"note":"coverage"},
      {"question":"JTG 3420 第5.1.2条","gold_standard_no":"JTG 3420","gold_clause_no":"5.1.2"}
    ])";
    auto cases = parse_dataset(json);
    REQUIRE(cases.size() == 3);

    CHECK(cases[0].question == "T0521 需要哪些仪具");
    CHECK(cases[0].gold_method_no == "T0521-2005");
    CHECK(cases[0].note == "method");
    CHECK(cases[0].gold_methods.empty());

    REQUIRE(cases[1].gold_methods.size() == 2);
    CHECK(cases[1].gold_methods[0] == "T0502-2005");
    CHECK(cases[1].gold_methods[1] == "T0590-2020");

    CHECK(cases[2].gold_standard_no == "JTG 3420");
    CHECK(cases[2].gold_clause_no == "5.1.2");
    CHECK(cases[2].gold_method_no.empty());
}

TEST_CASE("parse_dataset throws on a non-array top level") {
    CHECK_THROWS_AS(parse_dataset(R"({"question":"x"})"), std::runtime_error);
}

TEST_CASE("parse_dataset handles an empty array") {
    auto cases = parse_dataset("[]");
    CHECK(cases.empty());
}

TEST_CASE("parse_dataset defaults missing fields to empty") {
    auto cases = parse_dataset("[{}]");
    REQUIRE(cases.size() == 1);
    CHECK(cases[0].question.empty());
    CHECK(cases[0].gold_methods.empty());
}

TEST_CASE("parse_dataset skips malformed elements gracefully") {
    // 非对象元素跳过；gold_methods 里的非字符串元素跳过；都不抛
    auto cases = parse_dataset(R"([1, {"question":"q","gold_methods":["T1",2,null,"T2"]}])");
    REQUIRE(cases.size() == 1);
    CHECK(cases[0].question == "q");
    REQUIRE(cases[0].gold_methods.size() == 2);
    CHECK(cases[0].gold_methods[0] == "T1");
    CHECK(cases[0].gold_methods[1] == "T2");
}
