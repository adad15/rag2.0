#include <doctest/doctest.h>
#include <stdexcept>
#include "eval/dataset.h"

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
    CHECK(cases[0].must_have_groups.empty());
}

TEST_CASE("parse_dataset skips malformed elements gracefully") {
    // 非对象元素跳过；gold_methods 里的非字符串元素跳过；都不抛
    auto cases = parse_dataset(R"([1, {"question":"q","gold_methods":["T1",2,null,"T2"]}])");
    REQUIRE(cases.size() == 1);
    CHECK(cases[0].question == "q");
    REQUIRE(cases[0].must_have_groups.size() == 2);
    CHECK(cases[0].must_have_groups[0].stable_refs[0].method_no == "T1");
    CHECK(cases[0].must_have_groups[1].stable_refs[0].method_no == "T2");
}

TEST_CASE("parse normalizes legacy point-method into one evidence group") {
    auto cs = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])");
    REQUIRE(cs.size() == 1);
    REQUIRE(cs[0].must_have_groups.size() == 1);
    REQUIRE(cs[0].must_have_groups[0].stable_refs.size() == 1);
    CHECK(cs[0].must_have_groups[0].stable_refs[0].method_no == "T0521-2005");
    CHECK(cs[0].generation.cite_required == true);
}

TEST_CASE("parse normalizes legacy point-clause into one evidence group") {
    auto cs = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])");
    REQUIRE(cs[0].must_have_groups.size() == 1);
    auto& r = cs[0].must_have_groups[0].stable_refs[0];
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.clause_no == "7.3.1");
    CHECK(cs[0].generation.cite_required == true);
}

TEST_CASE("parse normalizes legacy coverage into N groups, no citation") {
    auto cs = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005"]}])");
    REQUIRE(cs[0].must_have_groups.size() == 2);
    CHECK(cs[0].must_have_groups[0].stable_refs[0].method_no == "T0316-2024");
    CHECK(cs[0].must_have_groups[1].stable_refs[0].method_no == "T0350-2005");
    CHECK(cs[0].generation.cite_required == false);
}

TEST_CASE("parse maps legacy gold_values into generation") {
    auto cs = parse_dataset(R"([{"question":"q","gold_values":["45","390"]}])");
    REQUIRE(cs[0].generation.gold_values.size() == 2);
    CHECK(cs[0].generation.gold_values[0] == "45");
    CHECK(cs[0].must_have_groups.empty());
}

TEST_CASE("parse reads rich evidence-group format directly") {
    auto cs = parse_dataset(R"([{
        "question":"q","query_type":"multi_evidence","difficulty":"hard","answerable":true,
        "must_have_groups":[
          {"group_id":"g1","chunk_ids":["c1","c2"],"stable_refs":[{"standard_no":"S","method_no":"T1"}]},
          {"group_id":"g2","chunk_ids":["c3"],"stable_refs":[{"standard_no":"S","clause_no":"5.3"}]}
        ],
        "acceptable_chunks":["a1"],"distractor_chunks":["d1"],
        "generation":{"gold_values":["25"],"cite_required":true}
    }])");
    REQUIRE(cs[0].must_have_groups.size() == 2);
    CHECK(cs[0].must_have_groups[0].chunk_ids.size() == 2);
    CHECK(cs[0].must_have_groups[1].stable_refs[0].clause_no == "5.3");
    CHECK(cs[0].acceptable_chunks[0] == "a1");
    CHECK(cs[0].distractor_chunks[0] == "d1");
    CHECK(cs[0].query_type == QueryType::MultiEvidence);
    CHECK(cs[0].difficulty == Difficulty::Hard);
    CHECK(cs[0].generation.cite_required == true);
    CHECK(cs[0].generation.gold_values[0] == "25");
}

TEST_CASE("parse: clause without standard_no makes no group and no citation") {
    auto cs = parse_dataset(R"([{"question":"q","gold_clause_no":"5.3"}])");
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].must_have_groups.empty());          // 缺 standard_no → 不构造证据组
    CHECK(cs[0].generation.cite_required == false);  // → 不评引用
}

TEST_CASE("derive_legacy_view round-trips legacy point-method") {
    auto c = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::PointMethod);
    CHECK(v.gold_method_no == "T0521-2005");
}

TEST_CASE("derive_legacy_view round-trips legacy point-clause") {
    auto c = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::PointClause);
    CHECK(v.gold_clause_no == "7.3.1");
    CHECK(v.gold_standard_no == "JTC 5210-2018");
}

TEST_CASE("derive_legacy_view round-trips legacy coverage in order") {
    auto c = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005","T0506-2005"]}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::Coverage);
    REQUIRE(v.gold_methods.size() == 3);
    CHECK(v.gold_methods[0] == "T0316-2024");
    CHECK(v.gold_methods[2] == "T0506-2005");
}

TEST_CASE("derive_generation_view: point-method gives one cite target, stemmed, empty std") {
    auto c = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])")[0];
    auto v = derive_generation_view(c);
    REQUIRE(v.cite_targets.size() == 1);
    CHECK(v.cite_targets[0].gold_ref == "T0521");
    CHECK(v.cite_targets[0].gold_standard_code == "");
    CHECK(v.gold_values.empty());
}

TEST_CASE("derive_generation_view: point-clause gives std+clause cite target") {
    auto c = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])")[0];
    auto v = derive_generation_view(c);
    REQUIRE(v.cite_targets.size() == 1);
    CHECK(v.cite_targets[0].gold_standard_code == "JTC 5210-2018");
    CHECK(v.cite_targets[0].gold_ref == "7.3.1");
}

TEST_CASE("derive_generation_view: coverage has no cite targets (not cite-scored)") {
    auto c = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005"]}])")[0];
    auto v = derive_generation_view(c);
    CHECK(v.cite_targets.empty());
}

TEST_CASE("derive_generation_view: numeric values pass through") {
    auto c = parse_dataset(R"([{"question":"q","gold_values":["45","390"]}])")[0];
    auto v = derive_generation_view(c);
    CHECK(v.cite_targets.empty());
    REQUIRE(v.gold_values.size() == 2);
    CHECK(v.gold_values[0] == "45");
}

TEST_CASE("derive_legacy_view: empty must_have_groups -> None (unscored stays unscored)") {
    auto c = parse_dataset(R"([{"question":"q","gold_clause_no":"5.3"}])")[0];  // clause 缺 standard → 无组
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::None);
    CHECK(v.gold_methods.empty());
    CHECK(v.gold_method_no.empty());
}
