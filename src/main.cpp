#include <iostream>
#include <string>
#include "rag_config.h"
#include "logging.h"
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "generate/deepseek_client.h"
#include "parse/poppler_parser.h"
#include "parse/hybrid_parser.h"
#include "parse/parser_factory.h"
#include "parse/ocr_backend.h"
#include "parse/parse_cache.h"
#include "parse/ocr_metrics.h"
#include "structure/clause_tree.h"
#include "structure/tree_builder.h"
#include "retrieve/retrieval_chunk.h"
#include "util/path_utf8.h"
#include <memory>
#include "embedding/cloud_embedding.h"
#include "ingest/ingest_pipeline.h"
#include "ingest/chunk_loader.h"
#include "retrieve/dense_retriever.h"
#include "generate/answer_pipeline.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// 按 config 构建解析器：poppler + 选定 OCR 后端 + 路由模式。backend 的所有权交给调用方持有，
// 须与返回的 HybridParser 同生命周期（HybridParser 内部持引用）。make_ocr_backend 选未实现引擎会抛。
static HybridParser make_parser(const Config& cfg, PopplerParser& poppler,
                                std::unique_ptr<OcrBackend>& backend) {
    backend = make_ocr_backend(cfg.ocr_engine, cfg.ppstruct_base_url);
    return HybridParser(poppler, *backend,
                        parse_mode_from_string(cfg.parse_mode),
                        cfg.scan_chars_threshold);
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
    try {
        PgClient pg(cfg.pg_conninfo);
        pg.apply_schema(read_file("src/db/schema.sql"));   // 幂等建表

        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        PopplerParser poppler;
        std::unique_ptr<OcrBackend> backend;
        HybridParser parser = make_parser(cfg, poppler, backend);

        auto r = ingest_file(cfg.doc_path, parser, pg, mv, embed, cfg.milvus_collection);
        spdlog::info("ingest 完成: standard_id={}, chunks={}", r.standard_id, r.clause_count);
        if (r.embedded_count < r.clause_count) {
            spdlog::warn("部分 chunk 未完成 embedding（{}/{}），重跑 ingest 或 chunkload 可修复",
                         r.embedded_count, r.clause_count);
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] ingest 失败: {}", e.what());
        return 1;
    }
}

static int cmd_query(const Config& cfg, const std::string& question) {
    try {
        PgClient pg(cfg.pg_conninfo);
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                    cfg.deepseek_model, cfg.deepseek_key);

        std::string ans = answer_query(question, mv, embed, pg, ds,
                                       cfg.milvus_collection, /*top_k=*/5);
        std::cout << "\n===== 回答 =====\n" << ans << "\n";
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] query 失败: {}", e.what());
        return 1;
    }
}

// 诊断命令：解析 PDF，打印页数与抽取字节数，把全文写进 dump.txt，并打印首个非空页样本。
static int cmd_dump(const Config& cfg) {
    if (cfg.doc_path.empty()) { spdlog::error("未设置 RAG_DOC_PATH"); return 1; }
    try {
        PopplerParser poppler;
        std::unique_ptr<OcrBackend> backend;
        HybridParser parser = make_parser(cfg, poppler, backend);
        ParsedDoc d = parser.parse(cfg.doc_path);
        size_t total = 0;
        for (auto& pg : d.pages) total += pg.text.size();
        spdlog::info("dump: {} 页, 抽取文字共 {} 字节", d.pages.size(), total);

        std::ofstream out("dump.txt", std::ios::binary);
        for (auto& pg : d.pages)
            out << "===== 第 " << pg.page_no << " 页 =====\n" << pg.text << "\n";
        out.close();
        spdlog::info("已写入 dump.txt（用编辑器打开查看抽取结果）");

        for (auto& pg : d.pages) {
            if (pg.text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            std::cout << "\n----- 第 " << pg.page_no << " 页样本(前600字节) -----\n"
                      << pg.text.substr(0, 600) << "\n";
            break;
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] dump: {}", e.what());
        return 1;
    }
}

// 体检命令：读一份 parse_cache JSON，打印 OCR 干不干净的指标表。
static int cmd_ocrcheck(const std::string& cache_path) {
    try {
        if (!std::filesystem::exists(cache_path)) {
            spdlog::error("缓存文件不存在: {}", cache_path); return 1;
        }
        std::string js = read_file(cache_path);
        if (js.empty()) { spdlog::error("缓存文件为空: {}", cache_path); return 1; }
        ParsedDoc d = parsed_doc_from_json(js);
        spdlog::info("缓存: {} | schema_version={} | pages={} elements={}",
                     cache_path, d.schema_version, d.pages.size(), d.elements.size());
        std::cout << format_ocr_metrics(compute_ocr_metrics(d)) << "\n";
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] ocrcheck: {}", e.what()); return 1;
    }
}

// 体检命令：读 parse_cache，构建条款树，打印统计，并写入 tree_cache。
static int cmd_treecheck(const std::string& cache_path) {
    try {
        if (!std::filesystem::exists(cache_path)) {
            spdlog::error("缓存文件不存在: {}", cache_path);
            return 1;
        }
        std::string js = read_file(cache_path);
        if (js.empty()) {
            spdlog::error("缓存文件为空: {}", cache_path);
            return 1;
        }

        ParsedDoc doc = parsed_doc_from_json(js);
        std::string sid = path_utf8::stem(cache_path);
        ClauseTree t = build_clause_tree(doc, sid);

        std::map<int, int> level_hist;
        int leaves = 0;
        int suspects = 0;
        for (const auto& n : t.nodes) {
            ++level_hist[n.level];
            if (n.is_leaf) ++leaves;
            if (!n.suspect.empty()) ++suspects;
        }

        spdlog::info("treecheck {} | format={} standard_no={}", sid, t.format_profile, t.standard_no);
        spdlog::info("  节点总数={} 叶子(检索单元)={} 可疑={}", t.nodes.size(), leaves, suspects);
        for (const auto& kv : level_hist) {
            spdlog::info("  L{} 数量={}", kv.first, kv.second);
        }
        spdlog::info("  page_clause_map 覆盖页数={}", t.page_clause_map.size());

        std::string out = "data/tree_cache/" + sid + ".json";
        write_tree_cache(out, t);
        spdlog::info("  已写 {}", out);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] treecheck: {}", e.what());
        return 1;
    }
}

// 体检命令：读 tree_cache，生成检索 chunk，打印三文本统计，并写入 chunk_cache。
static int cmd_chunkcheck(const std::string& tree_cache_path) {
    try {
        if (!std::filesystem::exists(tree_cache_path)) {
            spdlog::error("树缓存文件不存在: {}", tree_cache_path);
            return 1;
        }

        std::string js = read_file(tree_cache_path);
        if (js.empty()) {
            spdlog::error("树缓存文件为空: {}", tree_cache_path);
            return 1;
        }

        ClauseTree tree = clause_tree_from_json(js);
        RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

        size_t atomic_chars = 0;
        size_t embedding_chars = 0;
        size_t context_chars = 0;
        int with_caption = 0;
        int with_formula = 0;
        int with_table = 0;
        int suspects = 0;
        int empty_embedding = 0;
        int long_embedding = 0;

        for (const auto& c : cache.chunks) {
            atomic_chars += c.atomic_text.size();
            embedding_chars += c.embedding_text.size();
            context_chars += c.context_text.size();
            if (!c.captions.empty()) ++with_caption;
            if (c.has_formula || !c.formulas.empty()) ++with_formula;
            if (c.has_table) ++with_table;
            if (!c.suspect.empty()) ++suspects;
            if (c.embedding_text.empty()) ++empty_embedding;
            if (c.embedding_text.size() > 2000) ++long_embedding;
        }

        size_t count = cache.chunks.size();
        auto avg = [count](size_t total) -> size_t {
            return count == 0 ? 0 : total / count;
        };

        std::string sid = tree.standard_id.empty() ? path_utf8::stem(tree_cache_path) : tree.standard_id;
        std::string out = "data/chunk_cache/" + sid + ".json";
        write_chunk_cache(out, cache);

        spdlog::info("chunkcheck {} | standard_no={} format={}", sid, tree.standard_no, tree.format_profile);
        spdlog::info("  chunks={}", count);
        spdlog::info("  avg_atomic_chars={}", avg(atomic_chars));
        spdlog::info("  avg_embedding_chars={}", avg(embedding_chars));
        spdlog::info("  avg_context_chars={}", avg(context_chars));
        spdlog::info("  with_caption={} with_formula={} with_table={}", with_caption, with_formula, with_table);
        spdlog::info("  suspect={} empty_embedding={} long_embedding_over_2000={}",
                     suspects, empty_embedding, long_embedding);
        spdlog::info("  已写 {}", out);
        return empty_embedding == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] chunkcheck: {}", e.what());
        return 1;
    }
}

// 落库命令：读 chunk_cache，按 standard_id 先删后插写入 PG + Milvus。
// embedding 只吃 embedding_text。部分失败返回 1，重跑即可全量修复。
static int cmd_chunkload(const Config& cfg, const std::string& chunk_cache_path) {
    try {
        if (!std::filesystem::exists(chunk_cache_path)) {
            spdlog::error("chunk 缓存文件不存在: {}", chunk_cache_path);
            return 1;
        }
        std::string js = read_file(chunk_cache_path);
        if (js.empty()) {
            spdlog::error("chunk 缓存文件为空: {}", chunk_cache_path);
            return 1;
        }
        RetrievalChunkCache cache = retrieval_chunk_cache_from_json(js);
        if (cache.chunks.empty()) {
            spdlog::error("chunk 缓存中没有 chunk: {}", chunk_cache_path);
            return 1;
        }

        PgClient pg(cfg.pg_conninfo);
        pg.apply_schema(read_file("src/db/schema.sql"));   // 幂等建表
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);

        // standards 占位行，保证 retrieval_chunks 外键成立（ingest 全链路会写真行覆盖）
        if (!pg.get_standard(cache.standard_id)) {
            StandardRow s;
            s.standard_id = cache.standard_id;
            s.standard_no = cache.standard_no;
            s.standard_name = cache.standard_no;
            s.status = "现行";
            pg.upsert_standard(s);
        }

        mv.ensure_collection(cfg.milvus_collection, embed.dim());
        ChunkLoadResult r = load_chunks(cache, pg, mv, embed, cfg.milvus_collection);

        spdlog::info("chunkload {} | standard_no={}", cache.standard_id, cache.standard_no);
        spdlog::info("  chunks={} embedded={} deleted_old={}",
                     r.chunk_count, r.embedded_count, r.deleted_count);
        if (r.embedded_count < r.chunk_count) {
            spdlog::warn("部分 chunk 未完成 embedding，重跑 chunkload 可全量修复");
            if (r.embedded_count == 0) {
                spdlog::warn("若重跑后仍为 0，可能是旧结构 collection（主键 node_id）残留，"
                             "需先 drop 该 collection 再重跑（一次性升级步骤，"
                             "见 docs/superpowers/plans/2026-06-11-m2c3-chunk-persistence.md Task 6）");
            }
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] chunkload: {}", e.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    logging::init();
    if (argc < 2) {
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck|chunkload> [args]\n";
        return 1;
    }
    Config cfg = Config::from_json_file("config.json");
    std::string cmd = argv[1];

    if (cmd == "smoke") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) {
            for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m);
            return 1;
        }
        return cmd_smoke(cfg);
    }
    if (cmd == "ingest") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        return cmd_ingest(cfg);
    }
    if (cmd == "query") {
        if (argc < 3) { std::cout << "usage: rag2 query \"你的问题\"\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        return cmd_query(cfg, argv[2]);
    }
    if (cmd == "dump") {
        return cmd_dump(cfg);
    }
    if (cmd == "ocrcheck") {
        if (argc < 3) { std::cout << "usage: rag2 ocrcheck <parse_cache.json>\n"; return 1; }
        return cmd_ocrcheck(argv[2]);
    }
    if (cmd == "treecheck") {
        if (argc < 3) { std::cout << "usage: rag2 treecheck <parse_cache.json>\n"; return 1; }
        return cmd_treecheck(argv[2]);
    }
    if (cmd == "chunkcheck") {
        if (argc < 3) { std::cout << "usage: rag2 chunkcheck <tree_cache.json>\n"; return 1; }
        return cmd_chunkcheck(argv[2]);
    }
    if (cmd == "chunkload") {
        if (argc < 3) { std::cout << "usage: rag2 chunkload <chunk_cache.json>\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        return cmd_chunkload(cfg, argv[2]);
    }
    std::cout << "unknown command: " << cmd << "\n";
    return 1;
}
