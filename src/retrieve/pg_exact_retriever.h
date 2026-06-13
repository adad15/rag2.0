#pragma once
#include "retrieve/retriever.h"
#include "db/pg_client.h"

// 方法号精确路：构造时注入解析出的 method_no（契约③签名不携带编号）。
// method_no 为空时返回空列表；非空时前缀匹配取该方法全部 chunk。
class PgExactRetriever : public Retriever {
public:
    PgExactRetriever(PgClient& pg, std::string method_no);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    PgClient& pg_;
    std::string method_no_;
};
