#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "retrieve/retrieval_filter.h"

// 契约③（M3a 演进）：query + 过滤条件 → 候选列表。
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<Candidate> retrieve(const std::string& query,
                                            const RetrievalFilter& filter,
                                            int top_k) = 0;
};
