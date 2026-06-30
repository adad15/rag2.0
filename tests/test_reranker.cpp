#include <doctest/doctest.h>
#include "retrieve/reranker.h"

static RerankCandidate mk(const std::string& cid, const std::string& src, int rrf_rank,
                          const std::string& clause = "", const std::string& method = "",
                          const std::string& title = "", const std::string& standard = "s1") {
    RerankCandidate c;
    c.base.standard_id = standard;
    c.base.chunk_id = cid;
    c.base.source = src;
    c.base.score = 1.0f / (rrf_rank + 1);
    c.clause_no = clause;
    c.method_no = method;
    c.title = title;
    return c;
}

TEST_CASE("light_rerank: no signals keeps RRF order") {
    QueryAnalysis qa;
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","bm25",1), mk("c","dense",2)};
    auto out = light_rerank(qa, pool, 10, 2);
    REQUIRE(out.size() == 3);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
    CHECK(out[2].chunk_id == "c");
}

TEST_CASE("light_rerank: key_term hit lifts a lower candidate") {
    QueryAnalysis qa; qa.key_terms = {"安定性"};
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense",1)};
    pool[1].title = "安定性";
    auto out = light_rerank(qa, pool, 10, 2);
    CHECK(out[0].chunk_id == "b");
    CHECK(out[1].chunk_id == "a");
}

TEST_CASE("light_rerank: multi-source (+) gives small lift") {
    QueryAnalysis qa;
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense+bm25",1)};
    auto out = light_rerank(qa, pool, 10, 2);
    CHECK(out[0].chunk_id == "b");
}

TEST_CASE("light_rerank: method_no match lifts strongly") {
    QueryAnalysis qa; qa.method_no = "T0302";
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense",1,"","T0302")};
    auto out = light_rerank(qa, pool, 10, 2);
    CHECK(out[0].chunk_id == "b");
}

TEST_CASE("light_rerank: clause dedup demotes 3rd same-clause to end") {
    QueryAnalysis qa;
    std::vector<RerankCandidate> pool = {
        mk("a","dense",0,"5.1"), mk("b","dense",1,"5.1"),
        mk("c","dense",2,"5.1"), mk("d","dense",3,"9.9")};
    auto out = light_rerank(qa, pool, 10, 2);
    REQUIRE(out.size() == 4);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
    CHECK(out[2].chunk_id == "d");
    CHECK(out[3].chunk_id == "c");
}

TEST_CASE("light_rerank: stable on equal adjustment + truncate") {
    QueryAnalysis qa;
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense",1), mk("c","dense",2)};
    auto out = light_rerank(qa, pool, 2, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
}

TEST_CASE("assemble_rerank_candidates preserves fused order and skips missing") {
    std::vector<Candidate> fused;
    Candidate c1; c1.chunk_id="x"; c1.standard_id="s"; fused.push_back(c1);
    Candidate c2; c2.chunk_id="missing"; fused.push_back(c2);
    Candidate c3; c3.chunk_id="y"; fused.push_back(c3);
    std::vector<RetrievalChunkRow> rows;
    RetrievalChunkRow rx; rx.chunk_id="x"; rx.title="标题X"; rx.clause_no="1.1"; rows.push_back(rx);
    RetrievalChunkRow ry; ry.chunk_id="y"; ry.bm25_text="bm25Y"; rows.push_back(ry);
    auto out = assemble_rerank_candidates(fused, rows);
    REQUIRE(out.size() == 2);
    CHECK(out[0].base.chunk_id == "x");
    CHECK(out[0].title == "标题X");
    CHECK(out[1].base.chunk_id == "y");
    CHECK(out[1].bm25_text == "bm25Y");
}

TEST_CASE("light_rerank_with_fallback returns RRF order when PG lookup yields no pool") {
    QueryAnalysis qa;
    std::vector<Candidate> fused;
    Candidate a; a.chunk_id = "a"; a.standard_id = "s"; a.source = "dense"; fused.push_back(a);
    Candidate b; b.chunk_id = "b"; b.standard_id = "s"; b.source = "bm25"; fused.push_back(b);
    Candidate c; c.chunk_id = "c"; c.standard_id = "s"; c.source = "dense"; fused.push_back(c);

    auto out = light_rerank_with_fallback(qa, fused, {}, 2, 2);

    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
}
