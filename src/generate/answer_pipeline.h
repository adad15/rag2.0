#pragma once
#include <string>
#include "retrieve/retriever.h"
#include "db/pg_client.h"
#include "generate/deepseek_client.h"

// query → 检索候选 → 用 clause_id 回查 PG 拿权威字段 → 组装 ContextFragment
// → DeepSeek 生成带溯源回答。候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         Retriever& retriever,
                         PgClient& pg,
                         deepseek::DeepSeekClient& ds,
                         int top_k);
