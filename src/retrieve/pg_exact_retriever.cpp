#include "retrieve/pg_exact_retriever.h"

PgExactRetriever::PgExactRetriever(PgClient& pg, std::string method_no)
    : pg_(pg), method_no_(std::move(method_no)) {}

std::vector<Candidate> PgExactRetriever::retrieve(const std::string& /*query*/,
                                                  const RetrievalFilter& filter,
                                                  int /*top_k*/) {
    std::vector<Candidate> out;
    if (method_no_.empty()) return out;
    auto rows = pg_.chunks_by_method(method_no_, filter.standard_id);
    for (const auto& r : rows) {
        Candidate c;
        c.standard_id = r.standard_id;
        c.chunk_id = r.chunk_id;
        c.score = 1.0f;
        c.source = "exact";
        out.push_back(c);
    }
    return out;
}
