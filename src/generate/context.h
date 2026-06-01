#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// §11.3 注入 DeepSeek 的固定结构化片段。
struct ContextFragment {
    std::string source_id;     // S1, S2 ...
    std::string standard_no;
    std::string standard_name;
    std::string status;
    std::string clause_no;
    std::string path;
    bool is_mandatory = false;
    std::string text;
    std::vector<std::string> tables;    // M1 留空，M5 填表格片段
    std::vector<std::string> formulas;  // M1 留空
};

inline nlohmann::json to_json(const ContextFragment& f) {
    return {
        {"source_id", f.source_id},
        {"standard_no", f.standard_no},
        {"standard_name", f.standard_name},
        {"status", f.status},
        {"clause_no", f.clause_no},
        {"path", f.path},
        {"is_mandatory", f.is_mandatory},
        {"text", f.text},
        {"tables", f.tables},
        {"formulas", f.formulas}
    };
}
