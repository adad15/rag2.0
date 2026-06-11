#pragma once
#include <string>

// 所有召回候选统一归一到 standard_id + clause_id，作为去重键。
struct Candidate {
    std::string standard_id;
    std::string clause_id;   // == retrieval_chunks.chunk_id（M3 统一改名）
    float score = 0.0f;
    std::string source;      // "dense" | "bm25" | "pg_exact" | "visual" ...
};
