#include "retrieve/bm25_retriever.h"

Bm25Retriever::Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn,
                             std::string collection)
    : mv_(mv), syn_(syn), collection_(std::move(collection)) {}

std::vector<Candidate> Bm25Retriever::retrieve(const std::string& query,
                                               const RetrievalFilter& filter, int top_k) {
    std::string expanded = syn_.expand(query);   // 同义词扩展仅作用于 BM25 路
    auto hits = mv_.search_bm25(collection_, expanded, top_k, to_milvus_expr(filter));
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id;
        c.chunk_id = h.chunk_id;
        c.score = h.score;
        c.source = "bm25";
        out.push_back(c);
    }
    return out;
}
