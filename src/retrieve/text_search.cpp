#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/retrieval_filter.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"
#include <spdlog/spdlog.h>

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, PgClient& pg,
                                     const std::string& collection,
                                     int per_path_k, int top_k) {
    QueryAnalysis qa = analyze_query(question);

    // 标准号收窄：解析出代号则查 standard_id 下推；查不到回退全库并告警
    RetrievalFilter filter;
    if (!qa.standard_code.empty()) {
        std::string sid = pg.find_standard_by_code(qa.standard_code);
        if (!sid.empty()) filter.standard_id = sid;
        else spdlog::warn("查询提到标准号 {} 但库中未找到，回退全库检索", qa.standard_code);
    }

    // dense 路（必跑）+ 方法号路（有方法号才跑）
    std::vector<std::vector<Candidate>> lists;
    DenseRetriever dense(mv, embed, collection);
    lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
    if (!qa.method_no.empty()) {
        PgExactRetriever exact(pg, qa.method_no);
        lists.push_back(exact.retrieve(qa.clean_text, filter, per_path_k));
    }

    std::vector<Candidate> fused = rrf_fuse(lists, /*k=*/60, top_k);

    // 条款号置顶
    if (!qa.clause_no.empty()) {
        std::vector<std::string> pinned = pg.chunk_ids_by_clause(qa.clause_no, filter.standard_id);
        fused = pin_exact_clause(fused, pinned, top_k);
    }
    return fused;
}
