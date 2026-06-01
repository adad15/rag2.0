#include "generate/answer_pipeline.h"
#include "generate/context.h"
#include "generate/prompt_builder.h"
#include <vector>

std::string answer_query(const std::string& question, Retriever& retriever, PgClient& pg,
                         deepseek::DeepSeekClient& ds, int top_k) {
    auto candidates = retriever.retrieve(question, top_k);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        // 底座原则：以 PG 回查为权威源
        auto clause = pg.get_clause(c.clause_id);
        if (!clause) continue;
        auto std_row = pg.get_standard(clause->standard_id);

        ContextFragment f;
        f.source_id = "S" + std::to_string(idx++);
        f.standard_no = std_row ? std_row->standard_no : "";
        f.standard_name = std_row ? std_row->standard_name : "";
        f.status = std_row ? std_row->status : "";
        f.clause_no = clause->clause_no;
        f.path = clause->path;
        f.is_mandatory = false;   // M1 未识别强制性，M2 补
        f.text = clause->text;
        fragments.push_back(std::move(f));
    }
    if (fragments.empty())
        return "检索命中但回查规范原文为空，无法作答。";

    std::string sys = build_system_prompt();
    std::string user = build_user_prompt(question, fragments);
    return ds.chat(sys, user);
}
