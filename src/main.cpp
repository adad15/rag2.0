#include <iostream>
#include <string>
#include "rag_config.h"
#include "logging.h"
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "generate/deepseek_client.h"
#include "parse/poppler_parser.h"
#include "embedding/cloud_embedding.h"
#include "ingest/ingest_pipeline.h"
#include "retrieve/dense_retriever.h"
#include "generate/answer_pipeline.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static int cmd_smoke(const Config& cfg) {
    int failures = 0;

    // 1. PostgreSQL
    try {
        PgClient pg(cfg.pg_conninfo);
        if (pg.ping()) spdlog::info("[OK] PostgreSQL 连接成功");
        else { spdlog::error("[FAIL] PostgreSQL ping 失败"); ++failures; }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] PostgreSQL: {}", e.what()); ++failures;
    }

    // 2. Milvus
    try {
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        if (mv.ping()) spdlog::info("[OK] Milvus 连接成功");
        else { spdlog::error("[FAIL] Milvus ping 失败"); ++failures; }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] Milvus: {}", e.what()); ++failures;
    }

    // 3. DeepSeek（真实调用一次）
    try {
        deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                    cfg.deepseek_model, cfg.deepseek_key);
        std::string ans = ds.chat("你是测试助手，只回复 OK。", "请回复 OK");
        spdlog::info("[OK] DeepSeek 响应: {}", ans);
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] DeepSeek: {}", e.what()); ++failures;
    }

    // 4. poppler 打开 PDF
    try {
        if (cfg.doc_path.empty()) {
            spdlog::error("[FAIL] poppler: 未设置 RAG_DOC_PATH"); ++failures;
        } else {
            PopplerParser parser;
            ParsedDoc d = parser.parse(cfg.doc_path);
            spdlog::info("[OK] poppler 解析 {} 页，首页前 40 字: {}",
                         d.pages.size(),
                         d.pages.empty() ? std::string() : d.pages[0].text.substr(0, 40));
        }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] poppler: {}", e.what()); ++failures;
    }

    if (failures == 0) { spdlog::info("==== M0 冒烟全部通过 ===="); return 0; }
    spdlog::error("==== M0 冒烟失败 {} 项 ====", failures);
    return 1;
}

static int cmd_ingest(const Config& cfg) {
    if (cfg.doc_path.empty()) { spdlog::error("未设置 RAG_DOC_PATH"); return 1; }
    PgClient pg(cfg.pg_conninfo);
    pg.apply_schema(read_file("src/db/schema.sql"));   // 幂等建表

    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    PopplerParser parser;

    auto r = ingest_file(cfg.doc_path, parser, pg, mv, embed, cfg.milvus_collection);
    spdlog::info("ingest 完成: standard_id={}, clauses={}", r.standard_id, r.clause_count);
    return 0;
}

static int cmd_query(const Config& cfg, const std::string& question) {
    PgClient pg(cfg.pg_conninfo);
    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    DenseRetriever retriever(mv, embed, cfg.milvus_collection);
    deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                cfg.deepseek_model, cfg.deepseek_key);

    std::string ans = answer_query(question, retriever, pg, ds, /*top_k=*/5);
    std::cout << "\n===== 回答 =====\n" << ans << "\n";
    return 0;
}

int main(int argc, char** argv) {
    logging::init();
    if (argc < 2) {
        std::cout << "usage: rag2 <smoke|ingest|query> [args]\n";
        return 1;
    }
    Config cfg = Config::from_env();
    std::string cmd = argv[1];

    if (cmd == "smoke") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) {
            for (auto& m : missing) spdlog::error("缺少必填环境变量: {}", m);
            return 1;
        }
        return cmd_smoke(cfg);
    }
    if (cmd == "ingest") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("缺少 {}", m); return 1; }
        return cmd_ingest(cfg);
    }
    if (cmd == "query") {
        if (argc < 3) { std::cout << "usage: rag2 query \"你的问题\"\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("缺少 {}", m); return 1; }
        return cmd_query(cfg, argv[2]);
    }
    std::cout << "unknown command: " << cmd << "\n";
    return 1;
}
