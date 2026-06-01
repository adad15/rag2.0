#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"

class DenseRetriever : public Retriever {
public:
    DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query, int top_k) override;
private:
    milvus::MilvusRest& mv_;
    EmbeddingClient& embed_;
    std::string collection_;
};
