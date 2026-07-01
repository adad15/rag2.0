#include "retrieve/reranker.h"
#include "util/text_utf8.h"
#include "query/query_planner.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <functional>
#include <spdlog/spdlog.h>

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

namespace {
std::string rerank_query_text(const QueryAnalysis& qa) {
    return qa.dense_text.empty() ? qa.clean_text : qa.dense_text;
}
std::string rerank_cache_path(const std::string& dir, const std::string& model,
                              const std::string& instruction, const std::string& query,
                              const std::vector<RerankCandidate>& pool) {
    std::vector<std::string> ids;
    ids.reserve(pool.size());
    for (const auto& c : pool) ids.push_back(c.base.chunk_id);
    std::sort(ids.begin(), ids.end());
    std::string joined;
    for (const auto& id : ids) { joined += id; joined += ','; }
    std::string key = model + "\x1f" + instruction + "\x1f" + normalize_question(query) +
                      "\x1f" + joined + "\x1f" + kRerankDocVersion;
    size_t h = std::hash<std::string>{}(key);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(h));
    return dir + "/" + name;
}
std::map<std::string, double> read_rerank_cache(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    std::map<std::string, double> m;
    if (j.is_object() && j.contains("scores") && j["scores"].is_object())
        for (auto it = j["scores"].begin(); it != j["scores"].end(); ++it)
            m[it.key()] = it.value().get<double>();
    return m;
}
void write_rerank_cache(const std::string& dir, const std::string& path,
                        const std::map<std::string, double>& scores) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    nlohmann::json j; j["scores"] = nlohmann::json::object();
    for (const auto& kv : scores) j["scores"][kv.first] = kv.second;
    std::ofstream f(path, std::ios::binary);
    if (f) f << j.dump();
}
std::vector<RerankScore> scores_from_map(const std::vector<RerankCandidate>& pool,
                                         const std::map<std::string, double>& m) {
    std::vector<RerankScore> out;
    for (int i = 0; i < static_cast<int>(pool.size()); ++i) {
        auto it = m.find(pool[i].base.chunk_id);
        if (it != m.end()) out.push_back({i, it->second});
    }
    return out;
}
}  // namespace

std::vector<Candidate> RrfPassthrough::rerank(const QueryAnalysis&,
                                              const std::vector<RerankCandidate>& pool, int top_k) {
    std::vector<Candidate> out;
    out.reserve(pool.size());
    for (const auto& c : pool) out.push_back(c.base);
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::vector<Candidate> ModelReranker::rerank(const QueryAnalysis& qa,
                                             const std::vector<RerankCandidate>& pool, int top_k) {
    const std::string query = rerank_query_text(qa);
    const std::string path = rerank_cache_path(cache_dir_, model_, instruction_, query, pool);

    // 1) 缓存命中：直接用缓存分。
    auto cached = read_rerank_cache(path);
    if (!cached.empty()) {
        auto scores = scores_from_map(pool, cached);
        if (!scores.empty())
            return assemble_model_ranking(pool, scores, max_per_clause_, top_k);
    }

    // 2) 调模型；任何失败/空 -> 走兜底。
    std::vector<std::string> docs;
    docs.reserve(pool.size());
    for (const auto& c : pool) docs.push_back(compose_rerank_document(c));
    std::vector<RerankScore> scores;
    try {
        scores = call_(query, docs);
    } catch (const std::exception& e) {
        spdlog::warn("[rerank] 模型调用失败，回退兜底: {}", e.what());
        return fallback_->rerank(qa, pool, top_k);
    }
    if (scores.empty()) {
        spdlog::warn("[rerank] 模型返回空分，回退兜底");
        return fallback_->rerank(qa, pool, top_k);
    }

    // 3) 成功：写缓存（chunk_id->score），再 assemble。
    std::map<std::string, double> to_cache;
    const int n = static_cast<int>(pool.size());
    for (const auto& s : scores)
        if (s.index >= 0 && s.index < n) to_cache[pool[s.index].base.chunk_id] = s.score;
    write_rerank_cache(cache_dir_, path, to_cache);
    return assemble_model_ranking(pool, scores, max_per_clause_, top_k);
}
