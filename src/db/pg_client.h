#pragma once
#include <string>
#include <vector>
#include <optional>

struct ClauseRow {
    std::string node_id;
    std::string standard_id;
    std::string clause_no;
    std::string title;
    std::string path;
    std::string text;
    int page_start = 0;
};

struct StandardRow {
    std::string standard_id;
    std::string standard_no;
    std::string standard_name;
    std::string status;
    std::string file_path;
};

// retrieval_chunks 表的一行。captions_json/formulas_json 是 JSON 数组文本，
// 序列化在 chunk_loader 的纯函数里做，db 层只透传。
struct RetrievalChunkRow {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    std::string chunk_type;
    std::string clause_no;
    std::string method_no;
    std::string title;
    std::string path_text;
    std::string atomic_text;
    std::string embedding_text;
    std::string context_text;
    std::string bm25_text;
    std::string captions_json = "[]";
    std::string formulas_json = "[]";
    int page_start = 0;
    int page_end = 0;
    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::string suspect;
};

class PgClient {
public:
    explicit PgClient(std::string conninfo);
    // 执行 schema.sql 内容建表
    void apply_schema(const std::string& schema_sql);
    // 连通性检查：SELECT 1，成功返回 true
    bool ping();
    void upsert_standard(const StandardRow& s);
    void insert_clause(const ClauseRow& c);
    // 按 node_id 回查（契约：Milvus 命中后以 PG 为权威源）
    std::optional<ClauseRow> get_clause(const std::string& node_id);
    std::optional<StandardRow> get_standard(const std::string& standard_id);
    // M2c-3：retrieval_chunks 落库与回查。先删后插的幂等键是 standard_id。
    int delete_chunks_by_standard(const std::string& standard_id);  // 返回删除行数
    void insert_chunk(const RetrievalChunkRow& row);
    std::optional<RetrievalChunkRow> get_chunk(const std::string& chunk_id);
    // M3a 精确路：归一化裸代号 → standard_id（现行优先；未找到返回空）
    std::string find_standard_by_code(const std::string& code);
    // 方法号前缀匹配取该方法全部 chunk（只填 chunk_id/standard_id，按 clause_no 排序）
    std::vector<RetrievalChunkRow> chunks_by_method(const std::string& method_prefix,
                                                    const std::string& standard_id);
    // 关键词内容直查：embedding_text 含 keyword 的全部片段（status 非空则按其过滤现行）。
    // 只填 chunk_id/standard_id，按 chunk_id 排序。用于列举题补全召回。
    std::vector<RetrievalChunkRow> chunks_containing(const std::string& keyword,
                                                     const std::string& status);
    // 条款号精确命中（standard_id 可空），返回 chunk_id 列表
    std::vector<std::string> chunk_ids_by_clause(const std::string& clause_no,
                                                 const std::string& standard_id);
private:
    std::string conninfo_;
};
