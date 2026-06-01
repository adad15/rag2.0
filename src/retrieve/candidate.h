#pragma once
#include <string>

// 所有召回候选统一归一到 standard_id + clause_id（node_id），作为去重键。
struct Candidate {
    std::string standard_id;
    std::string clause_id;   // == clause_nodes.node_id
    float score = 0.0f;
    std::string source;      // "dense" | "bm25" | "pg_exact" | "visual" ...
};
