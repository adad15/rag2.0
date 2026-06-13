#pragma once
#include <string>

// 所有召回候选统一归一到 standard_id + chunk_id，作为去重键。
struct Candidate {
    std::string standard_id;
    std::string chunk_id;    // == retrieval_chunks.chunk_id
    float score = 0.0f;
    std::string source;      // "dense" | "exact" | "dense+exact" | "exact_pin" ...
};
