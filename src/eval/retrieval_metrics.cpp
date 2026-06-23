#include "eval/retrieval_metrics.h"
#include <algorithm>
#include <set>
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

int covered_groups_at_k(const std::vector<std::set<std::string>>& group_keys,
                        const std::vector<std::set<std::string>>& cand_keys_by_rank,
                        int k) {
    int kk = std::min(static_cast<int>(cand_keys_by_rank.size()), std::max(0, k));
    int covered = 0;
    for (const auto& gk : group_keys) {
        bool hit = false;
        for (int r = 0; r < kk && !hit; ++r)
            for (const auto& key : cand_keys_by_rank[r])
                if (gk.count(key)) { hit = true; break; }
        if (hit) ++covered;
    }
    return covered;
}
