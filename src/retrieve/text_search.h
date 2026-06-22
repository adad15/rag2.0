#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"
#include "query/synonyms.h"
#include "query/query_planner.h"

// M3b 编排：查询理解 → 标准号收窄 → dense + BM25 + 方法号 → RRF → 条款号置顶。
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv,
                                     EmbeddingClient& embed,
                                     PgClient& pg,
                                     const SynonymDict& syn,
                                     const std::string& collection,
                                     int per_path_k, int top_k,
                                     const QueryPlanner& planner);
