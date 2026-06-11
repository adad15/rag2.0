#pragma once
#include <optional>
#include <string>
#include "retrieve/retriever.h"
#include "db/pg_client.h"
#include "generate/context.h"
#include "generate/deepseek_client.h"

// 纯函数：chunk 行 + standards 行 -> LLM 上下文片段。
// text 取 context_text（small-to-big 回填），引用元数据取 chunk 定位字段。
ContextFragment fragment_from_chunk(int idx,
                                    const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row);

// query → 检索候选（clause_id 即 chunk_id）→ 回查 PG retrieval_chunks
// → 组装 ContextFragment → DeepSeek 生成带溯源回答。
// 候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         Retriever& retriever,
                         PgClient& pg,
                         deepseek::DeepSeekClient& ds,
                         int top_k);
