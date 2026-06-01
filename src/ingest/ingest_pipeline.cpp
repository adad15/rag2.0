#include "ingest/ingest_pipeline.h"
#include "ingest/clause_splitter.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <functional>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection) {
    ParsedDoc doc = parser.parse(file_path);

    std::string fname = std::filesystem::path(file_path).filename().string();
    std::string standard_id = make_id(file_path);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = fname;          // M1 占位
    s.standard_name = doc.title;    // M1 占位
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);

    mv.ensure_collection(collection, embed.dim());

    IngestResult result;
    result.standard_id = standard_id;

    for (auto& page : doc.pages) {
        auto clauses = split_clauses(page.text, page.page_no);
        for (auto& c : clauses) {
            std::string node_id = standard_id + ":" + c.clause_no;

            ClauseRow row;
            row.node_id = node_id;
            row.standard_id = standard_id;
            row.clause_no = c.clause_no;
            row.title = "";
            row.path = c.clause_no;     // M1 占位，M2 用层级路径
            row.text = c.text;
            row.page_start = c.page_start;
            pg.insert_clause(row);

            // 检索文本：M1 简单拼标准号 + 条款号 + 正文（retrieval_text 雏形）
            std::string retrieval_text = s.standard_no + " " + c.clause_no + " " + c.text;
            std::vector<float> vec = embed.embed(retrieval_text);
            mv.insert(collection, node_id, standard_id, vec);

            ++result.clause_count;
        }
    }
    spdlog::info("入库完成: {} 条条款 (standard_id={})", result.clause_count, standard_id);
    return result;
}
