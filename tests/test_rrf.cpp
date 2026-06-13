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
