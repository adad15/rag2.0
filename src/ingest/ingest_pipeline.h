#pragma once
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "parse/parser.h"

struct IngestResult {
    std::string standard_id;
    int clause_count = 0;    // M2c-3 起表示 chunk 数
    int embedded_count = 0;  // 成功 embed 并写入 Milvus 的 chunk 数
};

// M2c-3：解析 1 份文件（带 parse_cache）→ 建条款树 → 生成受控 chunk
// → 写 PG retrieval_chunks → embed embedding_text → 写 Milvus。
// 同时落 tree_cache/chunk_cache 副产品，与 treecheck/chunkcheck 产物一致。
// IngestResult.clause_count 自本刀起表示 chunk 数。
IngestResult ingest_file(const std::string& file_path,
                         Parser& parser,
                         PgClient& pg,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         const std::string& collection,
                         const std::string& cache_dir = "data/parse_cache");
