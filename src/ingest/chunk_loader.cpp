#include "ingest/chunk_loader.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

RetrievalChunkRow chunk_to_row(const RetrievalChunk& c) {
    RetrievalChunkRow row;
    row.chunk_id = c.chunk_id;
    row.node_id = c.node_id;
    row.standard_id = c.standard_id;
    row.chunk_type = c.chunk_type;
    row.clause_no = c.clause_no;
    row.method_no = c.method_no;
    row.title = c.title;
    row.path_text = c.path_text;
    row.atomic_text = c.atomic_text;
    row.embedding_text = c.embedding_text;
    row.context_text = c.context_text;
    row.captions_json = json(c.captions).dump();
    row.formulas_json = json(c.formulas).dump();
    row.page_start = c.page_start;
    row.page_end = c.page_end;
    row.has_table = c.has_table;
    row.has_formula = c.has_formula;
    row.has_figure = c.has_figure;
    row.suspect = c.suspect;
    return row;
}

ChunkLoadResult load_chunks(const RetrievalChunkCache& cache, PgClient& pg,
                            milvus::MilvusRest& mv, EmbeddingClient& embed,
                            const std::string& collection, const std::string& status) {
    ChunkLoadResult result;
    result.chunk_count = static_cast<int>(cache.chunks.size());

    mv.delete_by_standard(collection, cache.standard_id);
    result.deleted_count = pg.delete_chunks_by_standard(cache.standard_id);

    for (const auto& c : cache.chunks) {
        pg.insert_chunk(chunk_to_row(c));
        try {
            std::vector<float> vec = embed.embed(c.embedding_text);
            mv.insert_full(collection, c.chunk_id, c.node_id, c.standard_id,
                           status, c.embedding_text, vec);
            ++result.embedded_count;
        } catch (const std::exception& e) {
            spdlog::warn("chunk embed/写入失败，跳过: {} ({})", c.chunk_id, e.what());
        }
    }
    return result;
}
