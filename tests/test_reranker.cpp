#include <doctest/doctest.h>
#include "retrieve/reranker.h"
#include <filesystem>
#include <memory>
#include <string>

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

TEST_CASE("finalize_rerank: clause dedup demotes 3rd same-clause, truncate") {
    std::vector<RerankCandidate> ranked = {
        mk("a","dense",0,"5.1"), mk("b","dense",1,"5.1"),
        mk("c","dense",2,"5.1"), mk("d","dense",3,"9.9")};
    auto out = finalize_rerank(ranked, 2, 10);
    REQUIRE(out.size() == 4);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
    CHECK(out[2].chunk_id == "d");   // c 被降尾
    CHECK(out[3].chunk_id == "c");
}

TEST_CASE("finalize_rerank: max_per_clause<=0 means no cap") {
    std::vector<RerankCandidate> ranked = {
        mk("a","dense",0,"5.1"), mk("b","dense",1,"5.1"), mk("c","dense",2,"5.1")};
    auto out = finalize_rerank(ranked, 0, 10);
    REQUIRE(out.size() == 3);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");
    CHECK(out[2].chunk_id == "c");   // 不降尾
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

TEST_CASE("compose_rerank_document: 标题/编号/正文，忽略 path_text 与 bm25_text") {
    RerankCandidate c = mk("id1", "dense", 0, "5.1", "T0709", "马歇尔稳定度");
    c.atomic_text = "本方法规定了马歇尔稳定度试验的技术要求。";
    c.path_text = "不应出现的路径";
    c.bm25_text = "不应出现的BM25文本";
    std::string doc = compose_rerank_document(c);
    CHECK(doc.find("马歇尔稳定度") != std::string::npos);   // title
    CHECK(doc.find("条款 5.1") != std::string::npos);
    CHECK(doc.find("方法 T0709") != std::string::npos);
    CHECK(doc.find("技术要求") != std::string::npos);        // atomic
    CHECK(doc.find("不应出现的路径") == std::string::npos);   // path_text 不进
    CHECK(doc.find("不应出现的BM25文本") == std::string::npos);
}

TEST_CASE("compose_rerank_document: 空编号不写编号行；长正文短则补 context") {
    RerankCandidate c = mk("id2", "dense", 0, "", "", "");   // 无 title/clause/method
    c.atomic_text = "很短的正文";                            // < 80 字 -> 补 context
    c.context_text = "这是补充上下文内容。";
    std::string doc = compose_rerank_document(c);
    CHECK(doc.find("编号:") == std::string::npos);
    CHECK(doc.find("标题:") == std::string::npos);
    CHECK(doc.find("补充上下文") != std::string::npos);
}

TEST_CASE("assemble_model_ranking: score desc, dup index first-only, OOB ignore, missing tail") {
    std::vector<RerankCandidate> pool = {
        mk("a","dense",0), mk("b","dense",1), mk("c","dense",2)};
    std::vector<RerankScore> scores = {
        {1, 0.9}, {0, 0.8}, {1, 0.1}, {5, 0.99}};   // b>a；重复 index1 忽略第二次；index5 越界忽略；c 缺分
    auto out = assemble_model_ranking(pool, scores, 2 /*cap*/, 10 /*top_k*/);
    REQUIRE(out.size() == 3);
    CHECK(out[0].chunk_id == "b");
    CHECK(out[1].chunk_id == "a");
    CHECK(out[2].chunk_id == "c");   // 未打分 -> RRF 原序补尾
}

TEST_CASE("assemble_model_ranking: dedup + truncate applied") {
    std::vector<RerankCandidate> pool = {
        mk("a","dense",0,"5.1"), mk("b","dense",1,"5.1"),
        mk("c","dense",2,"5.1"), mk("d","dense",3,"9.9")};
    std::vector<RerankScore> scores = {{0,0.9},{1,0.8},{2,0.7},{3,0.6}};
    auto out = assemble_model_ranking(pool, scores, 2, 10);
    REQUIRE(out.size() == 4);
    CHECK(out[2].chunk_id == "d");   // c(第3个5.1)降尾
    CHECK(out[3].chunk_id == "c");
}

// 计数并可控抛异常的假打分器
struct FakeScorer {
    int calls = 0;
    bool throw_next = false;
    std::vector<RerankScore> ret;
    std::vector<RerankScore> operator()(const std::string&, const std::vector<std::string>&) {
        ++calls;
        if (throw_next) throw std::runtime_error("boom");
        return ret;
    }
};

static std::string tmp_cache_dir(const std::string& name) {
    auto d = std::filesystem::temp_directory_path() / ("rr_" + name);
    std::filesystem::remove_all(d);
    return d.string();
}

TEST_CASE("ModelReranker: success uses model order, writes+reads cache") {
    QueryAnalysis qa; qa.clean_text = "查询X";
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense",1)};
    auto scorer = std::make_shared<FakeScorer>();
    scorer->ret = {{1, 0.9}, {0, 0.1}};   // b 优先
    std::string dir = tmp_cache_dir("ok");
    RrfPassthrough fb;
    RerankCall call = [scorer](const std::string& q, const std::vector<std::string>& d){ return (*scorer)(q,d); };
    ModelReranker mr(call, "M", "指令", dir, 2, &fb);

    auto out1 = mr.rerank(qa, pool, 10);
    REQUIRE(out1.size() == 2);
    CHECK(out1[0].chunk_id == "b");
    CHECK(scorer->calls == 1);

    // 第二次同输入：命中缓存，不再调用打分器
    auto out2 = mr.rerank(qa, pool, 10);
    CHECK(out2[0].chunk_id == "b");
    CHECK(scorer->calls == 1);           // 未增加 -> 缓存命中
    std::filesystem::remove_all(dir);
}

TEST_CASE("ModelReranker: model failure falls back to injected reranker (RRF passthrough)") {
    QueryAnalysis qa; qa.clean_text = "查询Y";
    std::vector<RerankCandidate> pool = {mk("a","dense",0), mk("b","dense",1)};
    auto scorer = std::make_shared<FakeScorer>();
    scorer->throw_next = true;
    std::string dir = tmp_cache_dir("fail");
    RrfPassthrough fb;
    RerankCall call = [scorer](const std::string& q, const std::vector<std::string>& d){ return (*scorer)(q,d); };
    ModelReranker mr(call, "M", "", dir, 2, &fb);

    auto out = mr.rerank(qa, pool, 10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "a");       // RRF 原序
    CHECK(out[1].chunk_id == "b");
    std::filesystem::remove_all(dir);
}

TEST_CASE("RrfPassthrough: returns pool base in order, truncated, no dedup") {
    QueryAnalysis qa;
    std::vector<RerankCandidate> pool = {
        mk("a","dense",0,"5.1"), mk("b","dense",1,"5.1"), mk("c","dense",2,"5.1")};
    RrfPassthrough fb;
    auto out = fb.rerank(qa, pool, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "a");
    CHECK(out[1].chunk_id == "b");       // 不去重、不降尾，仅截断
}
