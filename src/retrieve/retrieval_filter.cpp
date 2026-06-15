#include "retrieve/retrieval_filter.h"

std::string to_milvus_expr(const RetrievalFilter& f) {
    // status/standard_id 均来自受控来源（"现行"/"作废"、文件路径哈希十进制串），
    // 不含引号；若来源放宽需在此加转义。
    std::string expr;
    if (!f.status.empty())
        expr = "status == \"" + f.status + "\"";
    if (!f.standard_id.empty()) {
        if (!expr.empty()) expr += " and ";
        expr += "standard_id == \"" + f.standard_id + "\"";
    }
    return expr;
}
