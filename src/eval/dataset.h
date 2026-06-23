#pragma once
#include <string>
#include <vector>

// 持久定位：标准号 + 方法号/条款号。chunk 重建后可据此回解析 chunk_id。
struct StableRef {
    std::string standard_no;   // 如 "JTG 3420-2020"（可空）
    std::string method_no;     // 如 "T0521-2005"（可空）
    std::string clause_no;     // 如 "5.3"（可空）
};

// 必要证据组：组内 chunk 等价（命中任一即覆盖），组间共同必要。
struct EvidenceGroup {
    std::string group_id;
    std::vector<std::string> chunk_ids;     // 首选；可空时由 stable_refs 解析
    std::vector<StableRef>  stable_refs;     // 持久锚点；引用评分也用它
};

enum class QueryType { ClauseMethodLocate, SingleFact, Procedure, Condition,
                       ParamFormulaTable, Compare, MultiEvidence, CrossClause, Unknown };
enum class Difficulty { Easy, Medium, Hard, Unknown };

// 生成答案轴 gold（M4.2）。
struct GenerationGold {
    std::vector<std::string> gold_values;   // 数值准确率（逐值）
    bool cite_required = false;             // 是否评引用（规范化时按是否点查置位）
    std::string reference_answer;           // 仅人工审核，不参与打分
};

// 评估样本（统一模型）。检索证据轴 = must_have_groups/acceptable/distractor；
// 生成答案轴 = generation。扁平字段为过渡期兼容，Task 5 删除。
struct EvalCase {
    std::string question;
    std::string note;

    // —— legacy 扁平字段（过渡保留；parse 仍填充，Task 5 删）——
    std::string gold_standard_no;
    std::string gold_clause_no;
    std::string gold_method_no;
    std::vector<std::string> gold_methods;
    std::vector<std::string> gold_values;

    // —— 统一模型（新）——
    std::string case_id;
    QueryType   query_type = QueryType::Unknown;
    Difficulty  difficulty = Difficulty::Unknown;
    bool        answerable = true;
    std::string language_variant;
    std::vector<std::string> source_standard_ids;
    std::vector<EvidenceGroup> must_have_groups;
    std::vector<std::string>   acceptable_chunks;
    std::vector<std::string>   distractor_chunks;
    GenerationGold generation;
    std::string generator_version, validation_status, expert_review;
};

// 解析评估集 JSON（对象数组）。缺字段取默认（空）。非数组/解析失败抛 std::runtime_error。纯函数。
std::vector<EvalCase> parse_dataset(const std::string& json_text);

// ——— 从统一模型派生"现状 evaluator 视图"，用于零变化迁移 ———

enum class LegacyKind { None, Coverage, PointMethod, PointClause };

// 检索侧：把 must_have_groups 还原为 run_eval 需要的 gold 视图。
// 规则：>1 组 → Coverage；==1 组 → 看其首条 stable_ref 是 method 还是 clause。
struct LegacyRetrievalView {
    LegacyKind kind = LegacyKind::None;
    std::vector<std::string> gold_methods;   // Coverage
    std::string gold_method_no;              // PointMethod
    std::string gold_clause_no;              // PointClause
    std::string gold_standard_no;            // PointClause
};
LegacyRetrievalView derive_legacy_view(const EvalCase& c);

// 生成侧：引用目标（已对方法号取 stem 去年份）+ 数值 gold。
struct CiteTarget { std::string gold_standard_code; std::string gold_ref; };
struct GenerationView {
    std::vector<CiteTarget> cite_targets;    // 空=不评引用
    std::vector<std::string> gold_values;
};
GenerationView derive_generation_view(const EvalCase& c);
