#pragma once
#include <string>

// 统一过滤条件。status 默认只召回现行；空串=不按状态过滤。
// standard_id 空=不按标准过滤。M6 扩展更多版本字段。
struct RetrievalFilter {
    std::string standard_id;
    std::string status = "现行";
};

// 拼 Milvus 标量过滤表达式：status 与 standard_id 各自非空则 AND 连接；
// 皆空 → 空串（不下推）。
std::string to_milvus_expr(const RetrievalFilter& f);
