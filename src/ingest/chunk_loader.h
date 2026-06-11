#pragma once

#include "db/pg_client.h"
#include "embedding/embedding_client.h"
#include "milvus/milvus_rest.h"
#include "retrieve/retrieval_chunk.h"

#include <string>

struct ChunkLoadResult {
    int chunk_count = 0;     // cache 中 chunk 总数
    int embedded_count = 0;  // 成功 embed 并写入 Milvus 的数量（两步均成功才计入）
    int deleted_count = 0;   // 先删后插阶段删除的 PG 旧 chunk 行数
};

// 纯函数：RetrievalChunk -> PG 行。captions/formulas 序列化为 JSON 数组文本。
RetrievalChunkRow chunk_to_row(const RetrievalChunk& c);

// chunk_cache -> PG retrieval_chunks + Milvus。按 standard_id 先删后插。
// dense 向量只 embed embedding_text。embed 单条失败记警告跳过，
// 调用方用 embedded_count < chunk_count 判断是否需要重跑。
ChunkLoadResult load_chunks(const RetrievalChunkCache& cache,
                            PgClient& pg,
                            milvus::MilvusRest& mv,
                            EmbeddingClient& embed,
                            const std::string& collection);
