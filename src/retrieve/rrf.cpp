#include "retrieve/rrf.h"
#include <algorithm>
#include <map>
#include <set>

std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k) {
    struct Acc { Candidate c; double score = 0.0; std::string sources; };
    std::map<std::string, Acc> by_id;
    std::vector<std::string> order;   // 保持首次出现顺序，使排序稳定

    for (const auto& list : lists) {
        for (size_t rank = 0; rank < list.size(); ++rank) {
            const Candidate& c = list[rank];
            auto it = by_id.find(c.chunk_id);
            if (it == by_id.end()) {
                Acc a;
                a.c = c;
                a.c.source.clear();
                by_id.emplace(c.chunk_id, a);
                order.push_back(c.chunk_id);
                it = by_id.find(c.chunk_id);
            }
            it->second.score += 1.0 / (k + static_cast<int>(rank) + 1);
            if (it->second.sources.find(c.source) == std::string::npos)
                it->second.sources += (it->second.sources.empty() ? "" : "+") + c.source;
        }
    }

    std::vector<Candidate> out;
    out.reserve(order.size());
    for (const auto& id : order) {
        Acc& a = by_id[id];
        a.c.score = static_cast<float>(a.score);
        a.c.source = a.sources;
        out.push_back(a.c);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::vector<Candidate> pin_exact_clause(const std::vector<Candidate>& fused,
                                        const std::vector<std::string>& pinned_chunk_ids,
                                        int top_k) {
    std::vector<Candidate> out;
    std::set<std::string> pinned_set(pinned_chunk_ids.begin(), pinned_chunk_ids.end());

    // 先放置顶项（按 pinned 给定顺序，去重）
    std::set<std::string> placed;
    for (const auto& id : pinned_chunk_ids) {
        if (placed.count(id)) continue;
        placed.insert(id);
        auto it = std::find_if(fused.begin(), fused.end(),
                               [&](const Candidate& c) { return c.chunk_id == id; });
        if (it != fused.end()) {
            Candidate c = *it;
            c.source += "+pin";
            out.push_back(c);
        } else {
            Candidate c;
            c.chunk_id = id;
            c.score = 1.0f;
            c.source = "exact_pin";
            out.push_back(c);
        }
    }
    // 再放其余融合结果（跳过已置顶）
    for (const auto& c : fused) {
        if (pinned_set.count(c.chunk_id)) continue;
        out.push_back(c);
    }
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::vector<Candidate> demote_without_keyterms(const std::vector<Candidate>& fused,
                                               const std::vector<std::string>& key_hit_ids,
                                               int top_k) {
    std::set<std::string> hits(key_hit_ids.begin(), key_hit_ids.end());
    std::vector<Candidate> front, back;
    front.reserve(fused.size());
    for (const auto& c : fused) {
        if (hits.count(c.chunk_id)) front.push_back(c);
        else back.push_back(c);
    }
    std::vector<Candidate> out;
    out.reserve(fused.size());
    out.insert(out.end(), front.begin(), front.end());
    out.insert(out.end(), back.begin(), back.end());
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}
