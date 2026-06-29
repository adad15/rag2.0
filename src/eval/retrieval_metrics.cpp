#include "eval/retrieval_metrics.h"
#include <algorithm>
#include <cmath>
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

int count_in_set_at_k(const std::vector<std::string>& cand, const std::set<std::string>& s, int k) {
    std::set<std::string> seen;
    int c = 0, n = std::min<int>(k, static_cast<int>(cand.size()));
    for (int i = 0; i < n; ++i)
        if (s.count(cand[i]) && !seen.count(cand[i])) { seen.insert(cand[i]); ++c; }
    return c;
}

int first_rank_in_set(const std::vector<std::string>& cand, const std::set<std::string>& s) {
    for (size_t i = 0; i < cand.size(); ++i)
        if (s.count(cand[i])) return static_cast<int>(i) + 1;
    return 0;
}

bool distractor_before_gold(int d, int g) {
    if (d <= 0) return false;
    return g <= 0 || d < g;
}

double ndcg_at_k(const std::vector<double>& gains,
                 const std::vector<double>& achievable, int k) {
    auto dcg = [](const std::vector<double>& g, int kk) {
        double s = 0.0; int n = std::min<int>(kk, static_cast<int>(g.size()));
        for (int i = 0; i < n; ++i) s += g[i] / std::log2(static_cast<double>(i) + 2.0);
        return s;
    };
    std::vector<double> ideal = achievable;
    std::sort(ideal.begin(), ideal.end(), std::greater<double>());
    double idcg = dcg(ideal, k);
    return idcg > 0.0 ? dcg(gains, k) / idcg : 0.0;
}

double redundancy_at_k(const std::vector<std::set<std::string>>& sig, int k) {
    std::set<std::string> seen;
    int eff = 0, red = 0, n = std::min<int>(k, static_cast<int>(sig.size()));
    for (int r = 0; r < n; ++r) {
        if (sig[r].empty()) continue;
        ++eff;
        bool brings_new = false;
        for (const auto& e : sig[r]) if (!seen.count(e)) { brings_new = true; break; }
        if (!brings_new) ++red;
        for (const auto& e : sig[r]) seen.insert(e);
    }
    return eff > 0 ? static_cast<double>(red) / eff : 0.0;
}
