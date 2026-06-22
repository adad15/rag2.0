#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/bm25_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/retrieval_filter.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"
#include <spdlog/spdlog.h>
#include <set>

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, PgClient& pg,
                                     const SynonymDict& syn, const std::string& collection,
                                     int per_path_k, int top_k,
                                     const QueryPlanner& planner) {
    QueryAnalysis qa = planner.plan(question);
    spdlog::info("[queryplan] intent={} sparse=\"{}\" dense=\"{}\"",
                 query_intent_name(qa.intent), qa.sparse_text, qa.dense_text);

    RetrievalFilter filter;   // 默认 status==现行
    if (!qa.standard_code.empty()) {
        std::string sid = pg.find_standard_by_code(qa.standard_code);
        if (!sid.empty()) filter.standard_id = sid;
        else spdlog::warn("查询提到标准号 {} 但库中未找到，回退全库检索", qa.standard_code);
    }

    // dense 路 + BM25 路（均必跑）+ 方法号路（有方法号才跑）
    std::vector<std::vector<Candidate>> lists;
    DenseRetriever dense(mv, embed, collection);
    Bm25Retriever bm25(mv, syn, collection);
    const std::string& dtext = qa.dense_text.empty()  ? qa.clean_text : qa.dense_text;
    const std::string& stext = qa.sparse_text.empty() ? qa.clean_text : qa.sparse_text;
    lists.push_back(dense.retrieve(dtext, filter, per_path_k));
    lists.push_back(bm25.retrieve(stext, filter, per_path_k));
    std::vector<std::string> method_pins;   // 方法号精确命中的 chunk，融合后置顶用
    if (!qa.method_no.empty()) {
        PgExactRetriever exact(pg, qa.method_no);
        std::vector<Candidate> ex = exact.retrieve(qa.clean_text, filter, per_path_k);
        for (const auto& c : ex) method_pins.push_back(c.chunk_id);
        lists.push_back(std::move(ex));
    }

    // M4.1：列举 + 命中关键词 → 关键词直查补全召回 + 错片下压
    std::vector<std::string> key_hit_ids;
    const bool list_recall =
        (qa.intent == QueryIntent::ListByCondition && !qa.key_terms.empty());
    if (list_recall) {
        std::vector<Candidate> kt;
        std::set<std::string> seen;
        for (const auto& term : qa.key_terms) {
            for (const auto& row : pg.chunks_containing(term, filter.status)) {
                if (seen.insert(row.chunk_id).second) {
                    Candidate c;
                    c.standard_id = row.standard_id;
                    c.chunk_id = row.chunk_id;
                    c.score = 0.0f;
                    c.source = "keyterm";
                    kt.push_back(c);
                    key_hit_ids.push_back(row.chunk_id);
                }
            }
        }
        lists.push_back(std::move(kt));
        spdlog::info("[queryplan] keyterm 直查命中 {} 片段", key_hit_ids.size());
    }

    // 列举时融合到大池、稍后下压再截断；非列举维持原 top_k 截断行为。
    const int fuse_k = list_recall ? 1000000 : top_k;
    std::vector<Candidate> fused = rrf_fuse(lists, /*k=*/60, fuse_k);
    if (list_recall)
        fused = demote_without_keyterms(fused, key_hit_ids, top_k);

    // 方法号精确命中置顶——与条款号 pin 对称，修 T0702/T0316 被泛 chunk 埋在 RRF 深处。
    if (!qa.method_no.empty())
        fused = pin_exact_clause(fused, method_pins, top_k);

    if (!qa.clause_no.empty()) {
        std::vector<std::string> pinned = pg.chunk_ids_by_clause(qa.clause_no, filter.standard_id);
        fused = pin_exact_clause(fused, pinned, top_k);
    }
    return fused;
}
