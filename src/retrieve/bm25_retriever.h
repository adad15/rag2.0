#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "query/synonyms.h"

// BM25 关键词路：查询经同义词扩展后走 Milvus 全文检索（sparse 字段）。
class Bm25Retriever : public Retriever {
public:
    Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    milvus::MilvusRest& mv_;
    const SynonymDict& syn_;
    std::string collection_;
};
