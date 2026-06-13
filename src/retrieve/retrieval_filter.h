#pragma once
#include <string>

// 统一过滤条件。M3a 只有 standard_id（空=不过滤）；M3b 增 status 等字段。
struct RetrievalFilter {
    std::string standard_id;
};

// 空 filter → 空串（不下推）；否则形如 standard_id == "..."
std::string to_milvus_expr(const RetrievalFilter& f);
