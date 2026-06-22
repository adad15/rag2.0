#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include "query/query_analysis.h"
#include "query/query_terms.h"

enum class PlannerMode { Rule, Llm, Auto };
PlannerMode parse_planner_mode(const std::string& s);   // "rule"/"llm"/"auto"；未知→Auto
const char* planner_mode_name(PlannerMode m);

// LLM 返回并解析后的查询计划。
struct LlmPlan {
    QueryIntent intent = QueryIntent::GeneralFact;
    std::vector<std::string> key_terms;
    std::vector<std::string> section_hints;
    std::string sparse_text;
    std::string dense_text;
};

// 纯函数：解析 LLM 的 JSON。非对象/解析失败/intent 非法/ListByCondition 却空 key_terms
// → 返回 nullopt（视为失败，调用方走保底）。
std::optional<LlmPlan> parse_llm_plan(const std::string& json_text);

// 缓存键归一化：trim + 折叠内部连续空白为单空格 + 转小写（仅 ASCII 大写转小写）。纯函数。
std::string normalize_question(const std::string& q);

// LLM system prompt（固定，含 few-shot）。改它要同时改 kQueryPlannerPromptVersion。
extern const char* kQueryPlannerSystemPrompt;
extern const char* kQueryPlannerPromptVersion;

using LlmCall = std::function<std::string(const std::string& system, const std::string& user)>;

struct QueryPlanner {
    PlannerMode mode = PlannerMode::Rule;
    QueryTerms terms;                         // 规则保底用
    LlmCall llm_call;                         // Rule 模式可空
    std::string cache_dir = "data/query_plan_cache";
    std::string prompt_version = kQueryPlannerPromptVersion;

    QueryAnalysis plan(const std::string& question) const;   // 实现在后续任务
};
