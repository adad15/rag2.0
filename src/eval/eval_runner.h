#pragma once
#include <string>
#include <vector>
#include "eval/dataset.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"
#include "query/synonyms.h"
#include "query/query_planner.h"

struct CaseResult {
    std::string question;
    bool is_coverage = false;
    bool scored = false;   // 点查是否真的算了分（有有效 gold）
    int rank = 0;          // 点查：首命中 1-based 排名（0=miss）
    int covered = 0;       // 覆盖查：覆盖到的 gold method 数
    int gold_total = 0;    // 覆盖查：gold method 总数
};

struct EvalReport {
    int point_cases = 0;       // 点查样本数
    int point_hits = 0;        // hit@k 命中数
    double mrr_sum = 0.0;      // 点查 reciprocal rank 之和（MRR = mrr_sum/point_cases）
    int coverage_cases = 0;    // 覆盖查样本数
    std::vector<CaseResult> results;
};

// 对每条样本跑 text_retrieve（per_path_k=k*4, top_k=k），回查 chunk 元数据算指标。
// 复用现有检索管道，不改检索逻辑。需 Milvus/PG/embedding 在线。
EvalReport run_eval(const std::vector<EvalCase>& cases,
                    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                    const SynonymDict& syn, const std::string& collection, int k,
                    const QueryPlanner& planner);

// 富检索指标（--rich）：每条用例在 ks 各 k 上覆盖的证据组数。
struct RichCaseResult {
    std::string question;
    int group_total = 0;                 // G
    std::vector<int> covered;            // 与 ks 对齐：Group Recall 覆盖组数
    int distractor_total = 0;            // |distractor_chunks|
    std::vector<int> distractor_in_k;    // 与 ks 对齐：top-k 命中干扰项数
    std::vector<double> ndcg;            // 与 ks 对齐
    std::vector<double> redundancy;      // 与 ks 对齐
    int first_distractor_rank = 0;       // 0=未命中
    int first_gold_rank = 0;             // 0=未命中（top-20 内首个覆盖任一组的排名）
};
struct RichReport {
    std::vector<int> ks;                 // {1,3,5,10,20}
    std::vector<RichCaseResult> results; // 仅含 must_have_groups 非空的用例
};

// 独立做 top-20 召回，按证据组算 Group Recall@k/Complete@k 的覆盖数据。
// 与 run_eval 互不影响（各自召回）。infra-bound：需 Milvus/PG/embedding 在线。
RichReport run_rich_eval(const std::vector<EvalCase>& cases,
                         milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                         const SynonymDict& syn, const std::string& collection,
                         const QueryPlanner& planner);
