#include "retrieve/retrieval_filter.h"

std::string to_milvus_expr(const RetrievalFilter& f) {
    // standard_id 由 find_standard_by_code 返回，源自文件路径哈希的十进制串，
    // 不含引号；若来源放宽需在此加转义。
    if (f.standard_id.empty()) return "";
    return "standard_id == \"" + f.standard_id + "\"";
}
