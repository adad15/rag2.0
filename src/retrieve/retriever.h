#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"

// 契约③：query → 候选列表。M1 仅 dense；M3 注册 BM25 / PG 精确，融合不改此接口。
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<Candidate> retrieve(const std::string& query, int top_k) = 0;
};
