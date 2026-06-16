#pragma once
#include <string>
#include <vector>
#include "query/query_terms.h"

enum class QueryIntent { GeneralFact, ClauseLookup, MethodLookup, ListByCondition };

struct QueryAnalysis {
    std::string clean_text;    // 原始问题（dense 回退输入，不抠编号）
    std::string standard_code; // 归一化裸代号，如 "JTC5210-2018"（空=未指定）
    std::string clause_no;     // 归一化条款号，如 "5.1.2"（空=未指定）
    std::string method_no;     // 归一化方法号，如 "T0302"（空=未指定）

    // 方案 B 查询计划（Phase 1）：
    QueryIntent intent = QueryIntent::GeneralFact;
    std::vector<std::string> key_terms;     // 命中的核心条件词
    std::vector<std::string> section_hints; // 章节提示（含默认注入）
    std::string sparse_text;   // BM25 输入；空 = 调用方回退 clean_text
    std::string dense_text;    // dense 输入；空 = 调用方回退 clean_text
};

// 查询理解（纯函数）：抽取标准号/条款号/方法号并归一化，并判定 intent。
QueryAnalysis analyze_query(const std::string& question);

// 在 analyze_query 基础上补全查询计划（key_terms/section_hints/sparse_text/dense_text）。
// 纯函数。仅 ListByCondition 且命中仪器白名单时生成改写文本，否则留空（回退）。
QueryAnalysis build_query_plan(const std::string& question, const QueryTerms& terms);

// 意图名（日志/调试用）。
const char* query_intent_name(QueryIntent intent);
