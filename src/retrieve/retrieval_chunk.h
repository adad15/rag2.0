#pragma once

#include "structure/clause_tree.h"

#include <string>
#include <vector>

struct RetrievalChunk {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    std::string standard_no;
    std::string chunk_type;
    std::string clause_no;
    std::string method_no;
    std::string title;
    std::string path_text;
    std::string atomic_text;
    std::string embedding_text;
    std::string context_text;
    std::vector<std::string> captions;
    std::vector<std::string> formulas;
    int page_start = 0;
    int page_end = 0;
    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::string suspect;
};

struct RetrievalChunkCache {
    int schema_version = 1;
    std::string standard_id;
    std::string standard_no;
    std::vector<RetrievalChunk> chunks;
};

RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree);
std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache);
RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text);
void write_chunk_cache(const std::string& cache_path, const RetrievalChunkCache& cache);
