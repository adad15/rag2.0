#include "rag_config.h"
#include <cstdlib>

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
    return c;
}

Config Config::from_env() {
    std::map<std::string, std::string> e;
    const char* keys[] = {
        "RAG_PG_CONNINFO","RAG_MILVUS_BASE_URL","RAG_MILVUS_TOKEN","RAG_MILVUS_COLLECTION",
        "RAG_EMBED_BASE_URL","RAG_EMBED_PATH","RAG_EMBED_MODEL","RAG_EMBED_DIM","RAG_EMBED_KEY",
        "RAG_DEEPSEEK_BASE_URL","RAG_DEEPSEEK_PATH","RAG_DEEPSEEK_MODEL","RAG_DEEPSEEK_KEY","RAG_DOC_PATH"
    };
    for (const char* k : keys) {
        const char* v = std::getenv(k);
        if (v) e[k] = v;
    }
    return from_map(e);
}

std::vector<std::string> Config::missing_required() const {
    std::vector<std::string> m;
    if (pg_conninfo.empty()) m.push_back("RAG_PG_CONNINFO");
    if (embed_key.empty())   m.push_back("RAG_EMBED_KEY");
    if (deepseek_key.empty())m.push_back("RAG_DEEPSEEK_KEY");
    return m;
}
