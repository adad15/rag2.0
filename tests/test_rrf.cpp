#include <doctest/doctest.h>
#include "retrieve/rrf.h"

static Candidate cand(const std::string& sid, const std::string& cid,
                      float score, const std::string& src) {
    Candidate c; c.standard_id = sid; c.chunk_id = cid; c.score = score; c.source = src;
    return c;
}

TEST_CASE("rrf_fuse merges per-list ranks and dedups by chunk_id") {
    std::vector<Candidate> dense = {
        cand("S", "S:4.2.1#main", 0.9f, "dense"),
        cand("S", "S:4.3.1#main", 0.8f, "dense") };
    std::vector<Candidate> exact = {
        cand("S", "S:4.3.1#main", 1.0f, "exact"),
        cand("S", "S:1.0.1#main", 1.0f, "exact") };

    auto fused = rrf_fuse({dense, exact}, /*k=*/60, /*top_k=*/10);

    REQUIRE(fused.size() == 3);
    CHECK(fused[0].chunk_id == "S:4.3.1#main");
    CHECK(fused[0].source.find("dense") != std::string::npos);
    CHECK(fused[0].source.find("exact") != std::string::npos);
}

TEST_CASE("rrf_fuse truncates to top_k") {
    std::vector<Candidate> a = {
        cand("S","S:1#main",1,"dense"), cand("S","S:2#main",1,"dense"),
        cand("S","S:3#main",1,"dense") };
    CHECK(rrf_fuse({a}, 60, 2).size() == 2);
}

TEST_CASE("pin_exact_clause moves an existing hit to the front") {
    std::vector<Candidate> fused = {
        cand("S","S:4.2.1#main",0.9f,"dense"),
        cand("S","S:5.1.2#main",0.5f,"dense"),
        cand("S","S:4.3.1#main",0.4f,"dense") };

    auto out = pin_exact_clause(fused, {"S:5.1.2#main"}, 10);

    REQUIRE(out.size() == 3);
    CHECK(out[0].chunk_id == "S:5.1.2#main");
    CHECK(out[0].source.find("pin") != std::string::npos);
    int count = 0;
    for (auto& c : out) if (c.chunk_id == "S:5.1.2#main") ++count;
    CHECK(count == 1);
}

TEST_CASE("pin_exact_clause inserts a pinned id that was not recalled") {
    std::vector<Candidate> fused = { cand("S","S:4.2.1#main",0.9f,"dense") };
    auto out = pin_exact_clause(fused, {"S:9.9.9#main"}, 10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:9.9.9#main");
    CHECK(out[0].source == "exact_pin");
}

TEST_CASE("pin_exact_clause respects top_k after pinning") {
    std::vector<Candidate> fused = {
        cand("S","S:1#main",0.9f,"dense"), cand("S","S:2#main",0.8f,"dense") };
    auto out = pin_exact_clause(fused, {"S:9#main"}, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:9#main");
}

TEST_CASE("demote_without_keyterms keeps key-term hits in front, others after") {
    std::vector<Candidate> fused = {
        cand("S","S:no1#main",0.9f,"dense+bm25"),   // 不含关键词，但分高
        cand("S","S:yes1#main",0.5f,"keyterm"),     // 含关键词
        cand("S","S:no2#main",0.4f,"bm25"),
        cand("S","S:yes2#main",0.3f,"dense+keyterm") };
    auto out = demote_without_keyterms(fused, {"S:yes1#main","S:yes2#main"}, 10);
    REQUIRE(out.size() == 4);
    CHECK(out[0].chunk_id == "S:yes1#main");   // 命中的提前，组内保持原顺序
    CHECK(out[1].chunk_id == "S:yes2#main");
    CHECK(out[2].chunk_id == "S:no1#main");    // 未命中的压后，组内保持原顺序
    CHECK(out[3].chunk_id == "S:no2#main");
}

TEST_CASE("demote_without_keyterms truncates to top_k after reordering") {
    std::vector<Candidate> fused = {
        cand("S","S:no1#main",0.9f,"dense"),
        cand("S","S:yes1#main",0.5f,"keyterm"),
        cand("S","S:no2#main",0.4f,"dense") };
    auto out = demote_without_keyterms(fused, {"S:yes1#main"}, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:yes1#main");   // 命中的先进 top-2
    CHECK(out[1].chunk_id == "S:no1#main");
}

TEST_CASE("demote_without_keyterms with empty hit set preserves order") {
    std::vector<Candidate> fused = {
        cand("S","S:a#main",0.9f,"dense"), cand("S","S:b#main",0.5f,"bm25") };
    auto out = demote_without_keyterms(fused, {}, 10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:a#main");
    CHECK(out[1].chunk_id == "S:b#main");
}
