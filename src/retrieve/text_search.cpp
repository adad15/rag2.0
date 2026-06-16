#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/bm25_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/retrieval_filter.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"
#include "query/query_terms.h"
#include <spdlog/spdlog.h>

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, PgClient& pg,
                                     const SynonymDict& syn, const std::string& collection,
                                     int per_path_k, int top_k) {
    static const QueryTerms query_terms = load_query_terms("config/query_terms.txt");
    QueryAnalysis qa = build_query_plan(question, query_terms);
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
    if (!qa.method_no.empty()) {
        PgExactRetriever exact(pg, qa.method_no);
        lists.push_back(exact.retrieve(qa.clean_text, filter, per_path_k));
    }

    std::vector<Candidate> fused = rrf_fuse(lists, /*k=*/60, top_k);

    if (!qa.clause_no.empty()) {
        std::vector<std::string> pinned = pg.chunk_ids_by_clause(qa.clause_no, filter.standard_id);
        fused = pin_exact_clause(fused, pinned, top_k);
    }
    return fused;
}
