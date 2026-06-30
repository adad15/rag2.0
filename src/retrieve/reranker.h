#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "db/pg_client.h"
#include "query/query_analysis.h"

struct RerankCandidate {
    Candidate base;
    std::string title;
    std::string path_text;
    std::string atomic_text;
    std::string context_text;
    std::string bm25_text;
    std::string clause_no;
    std::string method_no;
    int page_start = 0;
    int page_end = 0;
};

// 纯函数：按 fused 顺序把回查到的行拼成 RerankCandidate；回查不到的 chunk 跳过。
std::vector<RerankCandidate> assemble_rerank_candidates(
    const std::vector<Candidate>& fused,
    const std::vector<RetrievalChunkRow>& rows);

// 纯函数：以 pool 的 RRF 顺序为基底做小步加分(稳定排序)，再同条款(standard_id|clause_no)
// 去重(每键最多 max_per_clause，超出降末尾)，截断 top_k。返回 Candidate(base)。
std::vector<Candidate> light_rerank(const QueryAnalysis& qa,
                                    const std::vector<RerankCandidate>& pool,
                                    int top_k, int max_per_clause);

class IReranker {
public:
    virtual ~IReranker() = default;
    virtual std::vector<Candidate> rerank(const QueryAnalysis& qa,
                                          const std::vector<RerankCandidate>& pool,
                                          int top_k) = 0;
};

class LightReranker : public IReranker {
public:
    explicit LightReranker(int max_per_clause = 2) : max_per_clause_(max_per_clause) {}
    std::vector<Candidate> rerank(const QueryAnalysis& qa,
                                  const std::vector<RerankCandidate>& pool,
                                  int top_k) override {
        return light_rerank(qa, pool, top_k, max_per_clause_);
    }
private:
    int max_per_clause_;
};
