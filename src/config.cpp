#include "rag_config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

static std::string get(const std::map<std::string, std::string>& e,
                       const std::string& k, const std::string& def = "") {
    auto it = e.find(k);
    return it != e.end() && !it->second.empty() ? it->second : def;
}

Config Config::from_map(const std::map<std::string, std::string>& e) {
    Config c;
    c.pg_conninfo      = get(e, "RAG_PG_CONNINFO");
    c.milvus_base_url  = get(e, "RAG_MILVUS_BASE_URL", "http://localhost:19530");
    c.milvus_token     = get(e, "RAG_MILVUS_TOKEN", "root:Milvus");
    c.milvus_collection= get(e, "RAG_MILVUS_COLLECTION", "clause_text");
    c.embed_base_url   = get(e, "RAG_EMBED_BASE_URL", "https://api.siliconflow.cn");
    c.embed_path       = get(e, "RAG_EMBED_PATH", "/v1/embeddings");
    c.embed_model      = get(e, "RAG_EMBED_MODEL", "Qwen/Qwen3-Embedding-8B");
    c.embed_dim        = std::stoi(get(e, "RAG_EMBED_DIM", "4096"));
    c.embed_key        = get(e, "RAG_EMBED_KEY");
    c.deepseek_base_url= get(e, "RAG_DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    c.deepseek_path    = get(e, "RAG_DEEPSEEK_PATH", "/chat/completions");
    c.deepseek_model   = get(e, "RAG_DEEPSEEK_MODEL", "deepseek-v4-pro");
    c.deepseek_key     = get(e, "RAG_DEEPSEEK_KEY");
    c.doc_path         = get(e, "RAG_DOC_PATH");
    c.parse_mode       = get(e, "RAG_PARSE_MODE", "auto");
    c.ocr_engine       = get(e, "RAG_OCR_ENGINE", "ppstructure");
    c.ppstruct_base_url= get(e, "RAG_PPSTRUCT_BASE_URL", "http://localhost:8001");
    c.scan_chars_threshold = std::stoi(get(e, "RAG_SCAN_CHARS_THRESHOLD", "100"));
    return c;
}

Config Config::from_json_string(const std::string& json_text) {
    std::map<std::string, std::string> e;
    auto j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it->is_string())      e[it.key()] = it->get<std::string>();
            else if (!it->is_null())  e[it.key()] = it->dump();  // 数字/布尔 -> 字符串
        }
    }
    return from_map(e);
}

Config Config::from_json_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return from_map({});  // 文件缺失 -> 空配置（由 missing_required 报缺项）
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json_string(ss.str());
}

std::vector<std::string> Config::missing_required() const {
    std::vector<std::string> m;
    if (pg_conninfo.empty()) m.push_back("RAG_PG_CONNINFO");
    if (embed_key.empty())   m.push_back("RAG_EMBED_KEY");
    if (deepseek_key.empty())m.push_back("RAG_DEEPSEEK_KEY");
    return m;
}
