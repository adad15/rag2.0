#include "ingest/ingest_pipeline.h"
#include "ingest/chunk_loader.h"
#include "ingest/standard_meta.h"
#include "parse/parse_cache.h"
#include "retrieve/retrieval_chunk.h"
#include "structure/clause_tree.h"
#include "structure/tree_builder.h"
#include "util/path_utf8.h"
#include <spdlog/spdlog.h>
#include <functional>
#include <vector>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection,
                         const std::vector<std::string>& user_dict,
                         const std::string& cache_dir) {
    ParsedDoc doc = parser.parse(file_path);

    std::string stem = path_utf8::stem(file_path);
    std::string standard_id = make_id(file_path);
    std::string page1 = doc.pages.empty() ? std::string() : doc.pages[0].text;
    doc.standard_no = extract_standard_no(page1, stem);
    write_parse_cache(cache_dir + "/" + standard_id + ".json", doc);

    // M2c-1/M2c-2：建树 + 受控 chunk，顺手落缓存便于 treecheck/chunkcheck 排查
    ClauseTree tree = build_clause_tree(doc, standard_id);
    write_tree_cache("data/tree_cache/" + standard_id + ".json", tree);
    RetrievalChunkCache chunks = build_retrieval_chunk_cache(tree);
    write_chunk_cache("data/chunk_cache/" + standard_id + ".json", chunks);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = tree.standard_no.empty() ? doc.standard_no : tree.standard_no;
    s.standard_name = stem;
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);

    mv.ensure_collection_text(collection, embed.dim(), user_dict);
    ChunkLoadResult r = load_chunks(chunks, pg, mv, embed, collection, s.status);

    IngestResult result;
    result.standard_id = standard_id;
    result.clause_count = r.chunk_count;
    result.embedded_count = r.embedded_count;
    spdlog::info("入库完成: chunks={} embedded={} deleted_old={} (standard_id={})",
                 r.chunk_count, r.embedded_count, r.deleted_count, standard_id);
    return result;
}
