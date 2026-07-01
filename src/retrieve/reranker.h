#pragma once
#include <string>
#include <vector>
#include <functional>
#include "retrieve/candidate.h"
#include "db/pg_client.h"
#include "query/query_analysis.h"
#include "retrieve/rerank_api_client.h"

struct RerankParams {
    std::string mode = "off";   // off | light（model/hybrid 留给 M5.2）
    int pool_mult = 4;          // 非列举题候选池 = top_k * pool_mult
    int max_per_clause = 2;
};

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

// 纯函数：所有 reranker 输出的共用后处理。按已排好序的 ranked 做同条款去重
// (standard_id|clause_no 超过 max_per_clause 的降到队尾；max_per_clause<=0 表示不掐；
// clause_no 为空不受限)，再截断 top_k。返回 base 候选。
std::vector<Candidate> finalize_rerank(const std::vector<RerankCandidate>& ranked,
                                       int max_per_clause, int top_k);

// 纯函数：把模型打分套到 pool 上，生成最终排序。有效候选按 score 降序(稳定)；
// 重复 index 只取第一次；越界 index 忽略；未打分候选按 pool(RRF) 原序补尾；
// 再走 finalize_rerank(去重+截断)。
std::vector<Candidate> assemble_model_ranking(const std::vector<RerankCandidate>& pool,
                                              const std::vector<RerankScore>& scores,
                                              int max_per_clause, int top_k);

// 纯函数：以 pool 的 RRF 顺序为基底做小步加分(稳定排序)，再同条款(standard_id|clause_no)
// 去重(每键最多 max_per_clause，超出降末尾)，截断 top_k。返回 Candidate(base)。
std::vector<Candidate> light_rerank(const QueryAnalysis& qa,
                                    const std::vector<RerankCandidate>& pool,
                                    int top_k, int max_per_clause);

// 纯函数：rerank pool 为空时回退原 RRF 顺序，并截断到 top_k。
std::vector<Candidate> light_rerank_with_fallback(const QueryAnalysis& qa,
                                                  const std::vector<Candidate>& fused,
                                                  const std::vector<RerankCandidate>& pool,
                                                  int top_k, int max_per_clause);

// IO：按 fused 的 chunk_id 批量回查 PG，拼 RerankCandidate（保序、缺失跳过）。
std::vector<RerankCandidate> build_rerank_candidates(const std::vector<Candidate>& fused, PgClient& pg);

// document 构造版本号——改下面 compose 格式规则时必须 bump（缓存键含它）。
extern const char* kRerankDocVersion;

// 纯函数：把一个候选拼成给重排模型看的紧凑文档：标题 / 编号 / 正文。
// atomic 为主；title 只作定位；clause/method 非空才写；atomic 很短(<80 字)才补一小段 context；
// 整体按 UTF-8 字符上限截断。path_text、bm25_text、chunk id、score 一律不进。
std::string compose_rerank_document(const RerankCandidate& c);

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

// 模型打分回调：入参 query + documents(RRF序)，返回 index+score。失败抛异常或返回空。
using RerankCall = std::function<std::vector<RerankScore>(const std::string& query,
                                                          const std::vector<std::string>& docs)>;

// 兜底：直接返回 pool 的 base（RRF 原序）截断到 top_k，不去重。model 失败回退用。
class RrfPassthrough : public IReranker {
public:
    std::vector<Candidate> rerank(const QueryAnalysis& qa,
                                  const std::vector<RerankCandidate>& pool, int top_k) override;
};

// 模型重排：组文档 → 查缓存 → 调 score_call → 成功 assemble_model_ranking、失败走注入的兜底。
class ModelReranker : public IReranker {
public:
    ModelReranker(RerankCall call, std::string model, std::string instruction,
                  std::string cache_dir, int max_per_clause, IReranker* fallback)
        : call_(std::move(call)), model_(std::move(model)), instruction_(std::move(instruction)),
          cache_dir_(std::move(cache_dir)), max_per_clause_(max_per_clause), fallback_(fallback) {}
    std::vector<Candidate> rerank(const QueryAnalysis& qa,
                                  const std::vector<RerankCandidate>& pool, int top_k) override;
private:
    RerankCall call_;
    std::string model_, instruction_, cache_dir_;
    int max_per_clause_;
    IReranker* fallback_;
};
