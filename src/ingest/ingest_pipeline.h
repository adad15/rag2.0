#pragma once
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "parse/parser.h"

struct IngestResult {
    std::string standard_id;
    int clause_count = 0;
};

// 解析 1 份文件 → 切分条款 → 写 PG → 生成向量 → 写 Milvus。
// standard_no/standard_name 在 M1 用文件名占位（M2 由元数据抽取替换）。
IngestResult ingest_file(const std::string& file_path,
                         Parser& parser,
                         PgClient& pg,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         const std::string& collection);
