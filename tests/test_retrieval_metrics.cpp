#include <doctest/doctest.h>
#include "eval/retrieval_metrics.h"

TEST_CASE("first_hit_rank returns 1-based rank of first match") {
    std::vector<std::string> keys = {"a", "b", "gold", "gold"};
    CHECK(first_hit_rank(keys, "gold") == 3);
    CHECK(first_hit_rank(keys, "a") == 1);
}

TEST_CASE("first_hit_rank returns 0 when absent") {
    std::vector<std::string> keys = {"a", "b"};
    CHECK(first_hit_rank(keys, "z") == 0);
    CHECK(first_hit_rank({}, "z") == 0);
}

TEST_CASE("reciprocal_rank and hit_at_k") {
    CHECK(reciprocal_rank(1) == doctest::Approx(1.0));
    CHECK(reciprocal_rank(4) == doctest::Approx(0.25));
    CHECK(reciprocal_rank(0) == doctest::Approx(0.0));
    CHECK(hit_at_k(3, 5));
    CHECK_FALSE(hit_at_k(6, 5));
    CHECK_FALSE(hit_at_k(0, 5));
}

TEST_CASE("covered_count counts distinct gold methods present in candidates") {
    std::vector<std::string> cand = {"T1", "T2", "T2", "Tx", "T3"};
    std::vector<std::string> gold = {"T1", "T2", "T3", "T9"};
    CHECK(covered_count(cand, gold) == 3);   // T1,T2,T3 命中；T2 重复只算一次；T9 未召回
}

TEST_CASE("covered_count is 0 for empty inputs") {
    CHECK(covered_count({}, {"T1"}) == 0);
    CHECK(covered_count({"T1"}, {}) == 0);
}
