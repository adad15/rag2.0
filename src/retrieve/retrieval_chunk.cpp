#include "retrieve/retrieval_chunk.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

namespace {

std::string string_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

int int_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number_integer()) return 0;
    return j[key].get<int>();
}

bool bool_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_boolean()) return false;
    return j[key].get<bool>();
}

std::vector<std::string> string_array_value(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j[key].is_array()) return out;
    for (const auto& v : j[key]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

}  // namespace

RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree) {
    RetrievalChunkCache cache;
    cache.standard_id = tree.standard_id;
    cache.standard_no = tree.standard_no;
    return cache;
}

std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache) {
    json j;
    j["schema_version"] = cache.schema_version;
    j["standard_id"] = cache.standard_id;
    j["standard_no"] = cache.standard_no;
    j["chunks"] = json::array();
    for (const auto& c : cache.chunks) {
        j["chunks"].push_back({
            {"chunk_id", c.chunk_id},
            {"node_id", c.node_id},
            {"standard_id", c.standard_id},
            {"standard_no", c.standard_no},
            {"chunk_type", c.chunk_type},
            {"clause_no", c.clause_no},
            {"method_no", c.method_no},
            {"title", c.title},
            {"path_text", c.path_text},
            {"atomic_text", c.atomic_text},
            {"embedding_text", c.embedding_text},
            {"context_text", c.context_text},
            {"captions", c.captions},
            {"formulas", c.formulas},
            {"page_start", c.page_start},
            {"page_end", c.page_end},
            {"has_table", c.has_table},
            {"has_formula", c.has_formula},
            {"has_figure", c.has_figure},
            {"suspect", c.suspect},
        });
    }
    return j.dump(2);
}

RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, false);
    RetrievalChunkCache cache;
    if (!j.is_object()) return cache;

    cache.schema_version = j.value("schema_version", 1);
    cache.standard_id = string_value(j, "standard_id");
    cache.standard_no = string_value(j, "standard_no");

    if (j.contains("chunks") && j["chunks"].is_array()) {
        for (const auto& e : j["chunks"]) {
            if (!e.is_object()) continue;
            RetrievalChunk c;
            c.chunk_id = string_value(e, "chunk_id");
            c.node_id = string_value(e, "node_id");
            c.standard_id = string_value(e, "standard_id");
            c.standard_no = string_value(e, "standard_no");
            c.chunk_type = string_value(e, "chunk_type");
            c.clause_no = string_value(e, "clause_no");
            c.method_no = string_value(e, "method_no");
            c.title = string_value(e, "title");
            c.path_text = string_value(e, "path_text");
            c.atomic_text = string_value(e, "atomic_text");
            c.embedding_text = string_value(e, "embedding_text");
            c.context_text = string_value(e, "context_text");
            c.captions = string_array_value(e, "captions");
            c.formulas = string_array_value(e, "formulas");
            c.page_start = int_value(e, "page_start");
            c.page_end = int_value(e, "page_end");
            c.has_table = bool_value(e, "has_table");
            c.has_formula = bool_value(e, "has_formula");
            c.has_figure = bool_value(e, "has_figure");
            c.suspect = string_value(e, "suspect");
            cache.chunks.push_back(std::move(c));
        }
    }

    return cache;
}

void write_chunk_cache(const std::string& cache_path, const RetrievalChunkCache& cache) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary);
    if (!f) {
        spdlog::warn("chunk cache write failed: {}", cache_path);
        return;
    }
    f << retrieval_chunk_cache_to_json(cache);
}
