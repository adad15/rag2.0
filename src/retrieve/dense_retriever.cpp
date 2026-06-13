#include "retrieve/dense_retriever.h"

DenseRetriever::DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed,
                               std::string collection)
    : mv_(mv), embed_(embed), collection_(std::move(collection)) {}

std::vector<Candidate> DenseRetriever::retrieve(const std::string& query,
                                                const RetrievalFilter& filter, int top_k) {
    std::vector<float> qv = embed_.embed(query);
    auto hits = mv_.search(collection_, qv, top_k, to_milvus_expr(filter));
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id;
        c.chunk_id = h.chunk_id;   // M3a 起归一化键为 retrieval_chunks.chunk_id
        c.score = h.score;
        c.source = "dense";
        out.push_back(c);
    }
    return out;
}
