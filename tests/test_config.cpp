#include <doctest/doctest.h>
#include "rag_config.h"

TEST_CASE("Config::from_map fills fields and applies defaults") {
    std::map<std::string, std::string> env = {
        {"RAG_PG_CONNINFO", "host=localhost dbname=rag"},
        {"RAG_EMBED_KEY", "sk-embed"},
        {"RAG_DEEPSEEK_KEY", "sk-deep"},
        {"RAG_DOC_PATH", "C:/docs/a.pdf"},
    };
    Config c = Config::from_map(env);
    CHECK(c.pg_conninfo == "host=localhost dbname=rag");
    CHECK(c.embed_key == "sk-embed");
    CHECK(c.deepseek_key == "sk-deep");
    CHECK(c.doc_path == "C:/docs/a.pdf");
    // 默认值
    CHECK(c.milvus_base_url == "http://localhost:19530");
    CHECK(c.embed_model == "text-embedding-v3");
    CHECK(c.embed_dim == 1024);
    CHECK(c.deepseek_model == "deepseek-chat");
    CHECK(c.milvus_collection == "clause_text");
}

TEST_CASE("Config::from_map reports missing required keys") {
    std::map<std::string, std::string> env;
    Config c = Config::from_map(env);
    auto missing = c.missing_required();
    CHECK(std::find(missing.begin(), missing.end(), "RAG_PG_CONNINFO") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "RAG_EMBED_KEY") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "RAG_DEEPSEEK_KEY") != missing.end());
}
