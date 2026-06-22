#pragma once
#include <string>
#include <vector>
#include "eval/dataset.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"
#include "query/synonyms.h"
#include "query/query_planner.h"
#include "generate/deepseek_client.h"

struct GenCaseResult {
    std::string question;
    bool cite_scored = false;   // 本条是否打了引用分
    bool cite_hit = false;
    int  value_gold = 0;        // 本条 gold_values 数（0=非数值题）
    int  value_hits = 0;
};

struct GenerationReport {
    int cite_scored = 0,  cite_hits = 0;
    int value_gold_total = 0, value_hit_total = 0;
    std::vector<GenCaseResult> results;
};

// 对带生成 gold 的用例调 answer_query（带磁盘缓存），打引用/数值指标。
// infra-bound：需 Milvus/PG/embedding/DeepSeek 在线。不单测，靠实跑验证。
GenerationReport run_generation_eval(
    const std::vector<EvalCase>& cases,
    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
    const SynonymDict& syn, deepseek::DeepSeekClient& ds,
    const std::string& collection, int top_k,
    const QueryPlanner& planner, const std::string& answer_cache_dir);
