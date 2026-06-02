#include "ingest/ingest_pipeline.h"
#include "ingest/clause_splitter.h"
#include "ingest/standard_meta.h"
#include "parse/parse_cache.h"
#include "util/path_utf8.h"
#include <spdlog/spdlog.h>
#include <functional>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection, const std::string& cache_dir) {
    ParsedDoc doc = parser.parse(file_path);

    std::string stem = path_utf8::stem(file_path);
    std::string standard_id = make_id(file_path);
    std::string page1 = doc.pages.empty() ? std::string() : doc.pages[0].text;

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = extract_standard_no(page1, stem);  // 修订④：首页抽真号，回退文件名(去扩展名)
    s.standard_name = stem;                            // M1 仍用文件名(去扩展名)，真名留给 M2
    doc.standard_no = s.standard_no;                   // 回填真实标准号到 IR
    // M2a：把富 IR（含 OCR 页元素/表格 + 真实标准号）写磁盘缓存，供下一轮结构化层消费（不重复 OCR）
    write_parse_cache(cache_dir + "/" + standard_id + ".json", doc);
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

            // 修订①：M1 只嵌条款正文（去掉占位文件名/条款号噪声）；M2 有真元数据后再升级为正规 retrieval_text
            std::vector<float> vec = embed.embed(c.text);
            mv.insert(collection, node_id, standard_id, vec);

            ++result.clause_count;
        }
    }
    spdlog::info("入库完成: {} 条条款 (standard_id={})", result.clause_count, standard_id);
    return result;
}
