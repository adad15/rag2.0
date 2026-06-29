#include <doctest/doctest.h>
#include "eval/retrieval_metrics.h"
#include <set>

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
    CHECK(hit_at_k(5, 5));   // 边界：rank==k 命中
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

TEST_CASE("covered_groups_at_k: single group covered by a matching candidate, respects k") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T9"}, {"m:T1"}, {"m:T2"}};
    CHECK(covered_groups_at_k(groups, cands, 3) == 1);
    CHECK(covered_groups_at_k(groups, cands, 1) == 0);   // T1 在 rank2，k=1 截断
}

TEST_CASE("covered_groups_at_k: equivalence within a group (any key hits)") {
    std::vector<std::set<std::string>> groups = {{"cid:c1", "m:T1"}};   // 组内等价标识
    std::vector<std::set<std::string>> cands = {{"cid:c1"}};            // 命中其一即覆盖
    CHECK(covered_groups_at_k(groups, cands, 1) == 1);
}

TEST_CASE("covered_groups_at_k: multi-group partial coverage") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}, {"m:T2"}, {"c:S|5.3"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}, {"c:S|5.3"}};
    CHECK(covered_groups_at_k(groups, cands, 2) == 2);   // T1、clause 命中；T2 没有
    CHECK(covered_groups_at_k(groups, cands, 10) == 2);
}

TEST_CASE("covered_groups_at_k: empty group key-set is never covered") {
    std::vector<std::set<std::string>> groups = {{}, {"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}, {"m:T2"}};
    CHECK(covered_groups_at_k(groups, cands, 10) == 1);   // 空组不算，T1 组算
}

TEST_CASE("covered_groups_at_k: k beyond candidate count clamps, no overflow") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}};
    CHECK(covered_groups_at_k(groups, cands, 20) == 1);
    CHECK(covered_groups_at_k({}, cands, 20) == 0);        // 空组集 → 0
}

TEST_CASE("count_in_set_at_k counts unique hits within k") {
    std::vector<std::string> cand = {"a", "d1", "b", "d2", "d1"};
    std::set<std::string> d = {"d1", "d2"};
    CHECK(count_in_set_at_k(cand, d, 3) == 1);   // 前3:a,d1,b → d1
    CHECK(count_in_set_at_k(cand, d, 5) == 2);   // d1,d2(d1重复不再计)
    CHECK(count_in_set_at_k(cand, d, 1) == 0);
}

TEST_CASE("first_rank_in_set returns 1-based rank or 0") {
    std::vector<std::string> cand = {"a", "b", "g1"};
    CHECK(first_rank_in_set(cand, {"g1"}) == 3);
    CHECK(first_rank_in_set(cand, {"zz"}) == 0);
}

TEST_CASE("distractor_before_gold logic") {
    CHECK(distractor_before_gold(2, 5) == true);   // 干扰更靠前
    CHECK(distractor_before_gold(5, 2) == false);  // gold 更靠前
    CHECK(distractor_before_gold(3, 0) == true);   // gold 未命中、干扰命中
    CHECK(distractor_before_gold(0, 4) == false);  // 干扰未命中
    CHECK(distractor_before_gold(0, 0) == false);
}

TEST_CASE("ndcg_at_k with dedup gains") {
    std::vector<double> achievable = {2.0, 1.0};
    std::vector<double> perfect = {2.0, 1.0, 0.0};
    CHECK(ndcg_at_k(perfect, achievable, 3) == doctest::Approx(1.0));
    std::vector<double> bad = {0.0, 0.0, 2.0, 1.0};
    CHECK(ndcg_at_k(bad, achievable, 4) < 1.0);
    CHECK(ndcg_at_k({0.0, 0.0}, {}, 2) == doctest::Approx(0.0));
}

TEST_CASE("redundancy_at_k flags evidence bringing nothing new") {
    std::vector<std::set<std::string>> sig = {{"g:0"}, {"g:0"}, {"g:1"}, {}};
    CHECK(redundancy_at_k(sig, 4) == doctest::Approx(1.0 / 3.0));  // 3有效,1冗余
    std::vector<std::set<std::string>> sig2 = {{"m:T0302"}, {"m:T0302"}};
    CHECK(redundancy_at_k(sig2, 2) == doctest::Approx(0.5));
    CHECK(redundancy_at_k({{}, {}}, 2) == doctest::Approx(0.0));
}
