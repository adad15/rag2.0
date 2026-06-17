#include "eval/retrieval_metrics.h"
#include <unordered_set>

int first_hit_rank(const std::vector<std::string>& candidate_keys,
                   const std::string& gold_key) {
    for (size_t i = 0; i < candidate_keys.size(); ++i)
        if (candidate_keys[i] == gold_key) return static_cast<int>(i) + 1;
    return 0;
}

double reciprocal_rank(int rank) {
    return rank > 0 ? 1.0 / rank : 0.0;
}

bool hit_at_k(int rank, int k) {
    return rank >= 1 && rank <= k;
}

int covered_count(const std::vector<std::string>& candidate_methods,
                  const std::vector<std::string>& gold_methods) {
    std::unordered_set<std::string> gold(gold_methods.begin(), gold_methods.end());
    std::unordered_set<std::string> seen;
    for (const auto& m : candidate_methods)
        if (gold.count(m)) seen.insert(m);
    return static_cast<int>(seen.size());
}
