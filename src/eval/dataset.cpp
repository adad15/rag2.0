#include "eval/dataset.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

namespace {

QueryType parse_query_type(const std::string& s) {
    if (s == "clause_method_locate") return QueryType::ClauseMethodLocate;
    if (s == "single_fact")          return QueryType::SingleFact;
    if (s == "procedure")            return QueryType::Procedure;
    if (s == "condition")            return QueryType::Condition;
    if (s == "param_formula_table")  return QueryType::ParamFormulaTable;
    if (s == "compare")              return QueryType::Compare;
    if (s == "multi_evidence")       return QueryType::MultiEvidence;
    if (s == "cross_clause")         return QueryType::CrossClause;
    return QueryType::Unknown;
}
Difficulty parse_difficulty(const std::string& s) {
    if (s == "easy")   return Difficulty::Easy;
    if (s == "medium") return Difficulty::Medium;
    if (s == "hard")   return Difficulty::Hard;
    return Difficulty::Unknown;
}
std::vector<std::string> str_array(const json& item, const char* key) {
    std::vector<std::string> v;
    if (item.contains(key) && item[key].is_array())
        for (const auto& e : item[key]) if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}

void parse_rich(const json& item, EvalCase& c) {
    c.case_id          = item.value("case_id", "");
    c.query_type       = parse_query_type(item.value("query_type", ""));
    c.difficulty       = parse_difficulty(item.value("difficulty", ""));
    c.answerable       = item.value("answerable", true);
    c.language_variant = item.value("language_variant", "");
    c.source_standard_ids = str_array(item, "source_standard_ids");
    c.acceptable_chunks   = str_array(item, "acceptable_chunks");
    c.distractor_chunks   = str_array(item, "distractor_chunks");

    if (item.contains("must_have_groups") && item["must_have_groups"].is_array()) {
        for (const auto& g : item["must_have_groups"]) {
            if (!g.is_object()) continue;
            EvidenceGroup eg;
            eg.group_id  = g.value("group_id", "");
            eg.chunk_ids = str_array(g, "chunk_ids");
            if (g.contains("stable_refs") && g["stable_refs"].is_array())
                for (const auto& r : g["stable_refs"]) {
                    if (!r.is_object()) continue;
                    StableRef sr;
                    sr.standard_no = r.value("standard_no", "");
                    sr.method_no   = r.value("method_no", "");
                    sr.clause_no   = r.value("clause_no", "");
                    eg.stable_refs.push_back(std::move(sr));
                }
            c.must_have_groups.push_back(std::move(eg));
        }
    }
    if (item.contains("generation") && item["generation"].is_object()) {
        const auto& g = item["generation"];
        c.generation.gold_values      = str_array(g, "gold_values");
        c.generation.cite_required    = g.value("cite_required", false);
        c.generation.reference_answer = g.value("reference_answer", "");
    }
    if (item.contains("provenance") && item["provenance"].is_object()) {
        const auto& p = item["provenance"];
        c.generator_version = p.value("generator_version", "");
        c.validation_status = p.value("validation_status", "");
        c.expert_review     = p.value("expert_review", "");
    }
}

// 旧扁平字段 → 统一模型。富格式已给出 must_have_groups 时不覆盖。
void normalize_legacy(EvalCase& c) {
    if (c.must_have_groups.empty()) {
        if (!c.gold_methods.empty()) {                  // 覆盖查 → N 组，不评引用
            for (size_t i = 0; i < c.gold_methods.size(); ++i) {
                EvidenceGroup g; g.group_id = "m" + std::to_string(i);
                StableRef r; r.method_no = c.gold_methods[i];
                g.stable_refs.push_back(std::move(r));
                c.must_have_groups.push_back(std::move(g));
            }
        } else if (!c.gold_method_no.empty()) {         // 点查·方法 → 1 组 + 评引用
            EvidenceGroup g; g.group_id = "m0";
            StableRef r; r.method_no = c.gold_method_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        } else if (!c.gold_clause_no.empty() && !c.gold_standard_no.empty()) {  // 点查·条款
            EvidenceGroup g; g.group_id = "c0";
            StableRef r; r.standard_no = c.gold_standard_no; r.clause_no = c.gold_clause_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        }
        // gold_clause_no 缺 gold_standard_no：不构造组、不评引用（与旧行为一致，不计分）。
    }
    if (c.generation.gold_values.empty() && !c.gold_values.empty())
        c.generation.gold_values = c.gold_values;       // 兼容顶层旧写法
}

}  // namespace

std::vector<EvalCase> parse_dataset(const std::string& json_text) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        throw std::runtime_error("eval dataset must be a JSON array");

    std::vector<EvalCase> out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        EvalCase c;
        c.question         = item.value("question", "");
        c.note             = item.value("note", "");
        c.gold_standard_no = item.value("gold_standard_no", "");
        c.gold_clause_no   = item.value("gold_clause_no", "");
        c.gold_method_no   = item.value("gold_method_no", "");
        c.gold_methods     = str_array(item, "gold_methods");
        c.gold_values      = str_array(item, "gold_values");
        parse_rich(item, c);
        normalize_legacy(c);
        out.push_back(std::move(c));
    }
    return out;
}
