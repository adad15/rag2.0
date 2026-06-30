#include "generate/answer_pipeline.h"
#include "generate/prompt_builder.h"
#include "retrieve/text_search.h"
#include <vector>

ContextFragment fragment_from_chunk(int idx, const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row) {
    ContextFragment f;
    f.source_id = "S" + std::to_string(idx);
    f.standard_no = std_row ? std_row->standard_no : "";
    f.standard_name = std_row ? std_row->standard_name : "";
    f.status = std_row ? std_row->status : "";
    f.clause_no = chunk.clause_no;
    f.path = chunk.path_text;
    f.is_mandatory = false;   // 强制性条文识别留 M5
    // context_text 自带"路径：…"首行，与 f.path 在 prompt JSON 中重复一次；
    // 去重属 M5 prompt 改版范围
    f.text = chunk.context_text;
    return f;
}

std::string answer_query(const std::string& question, milvus::MilvusRest& mv,
                         EmbeddingClient& embed, PgClient& pg, const SynonymDict& syn,
                         deepseek::DeepSeekClient& ds, const std::string& collection,
                         int top_k, const QueryPlanner& planner, const RerankParams& rerank) {
    auto candidates = text_retrieve(question, mv, embed, pg, syn, collection,
                                    /*per_path_k=*/top_k * 4, top_k, planner, rerank);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        auto chunk = pg.get_chunk(c.chunk_id);   // 底座原则：以 PG 回查为权威源
        if (!chunk) continue;
        auto std_row = pg.get_standard(chunk->standard_id);
        // 默认只回现行：dense/BM25 路已在 Milvus 标量过滤，但 PG 方法号/条款号路
        // 不经 Milvus，须在此兜底剔除作废标准（M6 引入真作废数据后生效）
        if (std_row && std_row->status != "现行") continue;
        fragments.push_back(fragment_from_chunk(idx++, *chunk, std_row));
    }
    if (fragments.empty())
        return "检索命中但回查规范原文为空，无法作答。";

    std::string sys = build_system_prompt();
    std::string user = build_user_prompt(question, fragments);
    return ds.chat(sys, user);
}
