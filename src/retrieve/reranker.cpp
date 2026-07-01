#include "retrieve/reranker.h"
#include "util/text_utf8.h"
#include <algorithm>
#include <map>

const char* kRerankDocVersion = "v1";

std::string compose_rerank_document(const RerankCandidate& c) {
    std::string out;
    if (!c.title.empty()) out += "标题: " + c.title + "\n";
    std::string num;
    if (!c.clause_no.empty()) num += "条款 " + c.clause_no;
    if (!c.method_no.empty()) { if (!num.empty()) num += "；"; num += "方法 " + c.method_no; }
    if (!num.empty()) out += "编号: " + num + "\n";
    std::string body = c.atomic_text;
    if (text_utf8::char_count(c.atomic_text) < 80 && !c.context_text.empty())
        body += "\n" + text_utf8::truncate(c.context_text, 200);   // 150-300 区间取 200
    out += "正文: " + body;
    return text_utf8::truncate(out, 1000);   // 单 document 上限（800-1200 区间取 1000）
}

std::vector<RerankCandidate> assemble_rerank_candidates(
    const std::vector<Candidate>& fused, const std::vector<RetrievalChunkRow>& rows) {
    std::map<std::string, const RetrievalChunkRow*> by_id;
    for (const auto& r : rows) by_id[r.chunk_id] = &r;
    std::vector<RerankCandidate> out;
    for (const auto& c : fused) {
        auto it = by_id.find(c.chunk_id);
        if (it == by_id.end()) continue;
        const RetrievalChunkRow& r = *it->second;
        RerankCandidate rc;
        rc.base = c;
        rc.title = r.title; rc.path_text = r.path_text; rc.atomic_text = r.atomic_text;
        rc.context_text = r.context_text; rc.bm25_text = r.bm25_text;
        rc.clause_no = r.clause_no; rc.method_no = r.method_no;
        rc.page_start = r.page_start; rc.page_end = r.page_end;
        out.push_back(std::move(rc));
    }
    return out;
}

std::vector<Candidate> finalize_rerank(const std::vector<RerankCandidate>& ranked,
                                       int max_per_clause, int top_k) {
    std::vector<Candidate> kept, overflow;
    std::map<std::string, int> clause_count;
    for (const auto& c : ranked) {
        if (max_per_clause > 0 && !c.clause_no.empty()) {
            std::string key = c.base.standard_id + "|" + c.clause_no;
            if (++clause_count[key] > max_per_clause) { overflow.push_back(c.base); continue; }
        }
        kept.push_back(c.base);
    }
    kept.insert(kept.end(), overflow.begin(), overflow.end());
    if (static_cast<int>(kept.size()) > top_k) kept.resize(top_k);
    return kept;
}

std::vector<Candidate> light_rerank(const QueryAnalysis& qa,
                                    const std::vector<RerankCandidate>& pool,
                                    int top_k, int max_per_clause) {
    struct Scored { size_t idx; double adj; };
    std::vector<Scored> sc;
    sc.reserve(pool.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        const RerankCandidate& c = pool[i];
        double adj = 0.0;
        if (c.base.source.find('+') != std::string::npos) adj += 1.5;
        if (!qa.method_no.empty() && c.method_no == qa.method_no) adj += 3.0;
        if (!qa.clause_no.empty() && c.clause_no == qa.clause_no) adj += 3.0;
        int kt = 0;
        for (const auto& t : qa.key_terms) {
            if (t.empty()) continue;
            if (c.title.find(t) != std::string::npos || c.path_text.find(t) != std::string::npos ||
                c.atomic_text.find(t) != std::string::npos || c.context_text.find(t) != std::string::npos ||
                c.bm25_text.find(t) != std::string::npos) {
                if (++kt >= 3) break;
            }
        }
        adj += static_cast<double>(kt);
        sc.push_back({i, adj});
    }
    std::stable_sort(sc.begin(), sc.end(),
                     [](const Scored& a, const Scored& b) { return a.adj > b.adj; });

    std::vector<RerankCandidate> ranked;
    ranked.reserve(sc.size());
    for (const auto& s : sc) ranked.push_back(pool[s.idx]);
    return finalize_rerank(ranked, max_per_clause, top_k);
}

std::vector<Candidate> light_rerank_with_fallback(const QueryAnalysis& qa,
                                                  const std::vector<Candidate>& fused,
                                                  const std::vector<RerankCandidate>& pool,
                                                  int top_k, int max_per_clause) {
    if (!pool.empty())
        return light_rerank(qa, pool, top_k, max_per_clause);

    std::vector<Candidate> out = fused;
    if (top_k < 0) top_k = 0;
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::vector<Candidate> assemble_model_ranking(const std::vector<RerankCandidate>& pool,
                                              const std::vector<RerankScore>& scores,
                                              int max_per_clause, int top_k) {
    const int n = static_cast<int>(pool.size());
    std::vector<char> seen(n, 0);
    struct SI { size_t idx; double score; };
    std::vector<SI> scored;
    for (const auto& rs : scores) {
        if (rs.index < 0 || rs.index >= n) continue;   // 越界忽略
        if (seen[rs.index]) continue;                   // 重复 index 取第一次
        seen[rs.index] = 1;
        scored.push_back({static_cast<size_t>(rs.index), rs.score});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const SI& a, const SI& b) { return a.score > b.score; });
    std::vector<RerankCandidate> ranked;
    ranked.reserve(n);
    for (const auto& s : scored) ranked.push_back(pool[s.idx]);
    for (int i = 0; i < n; ++i) if (!seen[i]) ranked.push_back(pool[i]);   // 补尾
    return finalize_rerank(ranked, max_per_clause, top_k);
}

std::vector<RerankCandidate> build_rerank_candidates(const std::vector<Candidate>& fused, PgClient& pg) {
    std::vector<std::string> ids;
    ids.reserve(fused.size());
    for (const auto& c : fused) ids.push_back(c.chunk_id);
    return assemble_rerank_candidates(fused, pg.get_chunks(ids));
}
