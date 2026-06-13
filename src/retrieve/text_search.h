#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"

// M3a 编排：查询理解 → 标准号收窄 → dense 路 + 方法号路 → RRF → 条款号置顶。
// 返回最终候选（chunk_id 维度，已截断 top_k）。
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv,
                                     EmbeddingClient& embed,
                                     PgClient& pg,
                                     const std::string& collection,
                                     int per_path_k, int top_k);
