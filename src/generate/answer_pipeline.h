#pragma once
#include <optional>
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "generate/context.h"
#include "generate/deepseek_client.h"
#include "query/synonyms.h"
#include "query/query_planner.h"
#include "retrieve/reranker.h"

// 纯函数：chunk 行 + standards 行 -> LLM 上下文片段。
ContextFragment fragment_from_chunk(int idx,
                                    const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row);

// M3b 问答：text_retrieve 三角色召回 → PG 回查 → ContextFragment → DeepSeek。
// 候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         PgClient& pg,
                         const SynonymDict& syn,
                         deepseek::DeepSeekClient& ds,
                         const std::string& collection,
                         int top_k,
                         const QueryPlanner& planner,
                         const RerankParams& rerank);
