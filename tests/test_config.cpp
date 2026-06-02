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
    CHECK(c.embed_model == "Qwen/Qwen3-Embedding-8B");
    CHECK(c.embed_dim == 4096);
    CHECK(c.deepseek_model == "deepseek-v4-pro");
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

TEST_CASE("Config::from_json_string parses keys and applies defaults") {
    std::string json = R"({
        "RAG_PG_CONNINFO": "host=localhost dbname=rag",
        "RAG_EMBED_KEY": "sk-embed",
        "RAG_DEEPSEEK_KEY": "sk-deep",
        "RAG_EMBED_DIM": "2048",
        "RAG_DOC_PATH": "C:/docs/a.pdf"
    })";
    Config c = Config::from_json_string(json);
    CHECK(c.pg_conninfo == "host=localhost dbname=rag");
    CHECK(c.embed_key == "sk-embed");
    CHECK(c.deepseek_key == "sk-deep");
    CHECK(c.embed_dim == 2048);
    CHECK(c.doc_path == "C:/docs/a.pdf");
    // 未提供的项回退默认
    CHECK(c.milvus_base_url == "http://localhost:19530");
    CHECK(c.embed_model == "Qwen/Qwen3-Embedding-8B");
}

TEST_CASE("Config::from_json_string accepts numeric values (e.g. dim as number)") {
    Config c = Config::from_json_string(R"({"RAG_EMBED_DIM": 4096})");
    CHECK(c.embed_dim == 4096);
}

TEST_CASE("Config::from_json_string on invalid json yields empty config (missing required)") {
    Config c = Config::from_json_string("not valid json");
    auto missing = c.missing_required();
    CHECK(std::find(missing.begin(), missing.end(), "RAG_PG_CONNINFO") != missing.end());
}

TEST_CASE("Config parses M2a parser keys with defaults") {
    Config c = Config::from_json_string("{}");
    CHECK(c.parse_mode == "auto");
    CHECK(c.ocr_engine == "ppstructure");
    CHECK(c.ppstruct_base_url == "http://localhost:8001");
    CHECK(c.scan_chars_threshold == 100);
}

TEST_CASE("Config M2a parser keys can be overridden") {
    Config c = Config::from_json_string(
        R"({"RAG_PARSE_MODE":"ocr","RAG_OCR_ENGINE":"ppstructure",
            "RAG_PPSTRUCT_BASE_URL":"http://x:9","RAG_SCAN_CHARS_THRESHOLD":"50"})");
    CHECK(c.parse_mode == "ocr");
    CHECK(c.ppstruct_base_url == "http://x:9");
    CHECK(c.scan_chars_threshold == 50);
}
