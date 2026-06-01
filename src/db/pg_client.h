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
private:
    std::string conninfo_;
};
