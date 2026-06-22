#include "query/query_planner.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

const char* kQueryPlannerPromptVersion = "qp-v1";

const char* kQueryPlannerSystemPrompt =
    "你是公路工程标准问答系统的查询分析器。给定一个中文问题，只输出一个严格的 JSON 对象，"
    "描述检索计划，不要任何解释或代码块围栏。字段：\n"
    "- intent: 四选一 \"GeneralFact\"|\"ListByCondition\"|\"ClauseLookup\"|\"MethodLookup\"。"
    "问\"哪些/有哪些…用到/需要某仪器或材料\"这类要列举多个试验的→ListByCondition；其余普通问答→GeneralFact。\n"
    "- key_terms: 问题里的核心判别词（仪器/材料/实体名）。用问题原文中的词形，不要扩展成全称。GeneralFact 可为空数组。\n"
    "- section_hints: 该判别词通常所在章节词（仪器→[\"仪具\",\"材料\"]）；无则空数组。\n"
    "- sparse_text: 给关键词检索的查询，仅含判别词+章节词，空格分隔，去掉\"哪些/的/了\"等虚词与\"公路/工程/试验/规程\"等满库背景词。GeneralFact 留空串。\n"
    "- dense_text: 给向量检索的简短自然语句，聚焦判别词。GeneralFact 留空串。\n"
    "示例：\n"
    "问：公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平\n"
    "答：{\"intent\":\"ListByCondition\",\"key_terms\":[\"天平\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"天平 仪具 材料\",\"dense_text\":\"使用天平的试验仪具与材料\"}\n"
    "问：哪些试验用到马歇尔\n"
    "答：{\"intent\":\"ListByCondition\",\"key_terms\":[\"马歇尔\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"马歇尔 仪具 材料\",\"dense_text\":\"使用马歇尔的试验仪具与材料\"}\n"
    "问：路基沉降怎么评定\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[],\"section_hints\":[],\"sparse_text\":\"\",\"dense_text\":\"\"}";

PlannerMode parse_planner_mode(const std::string& s) {
    if (s == "rule") return PlannerMode::Rule;
    if (s == "llm")  return PlannerMode::Llm;
    return PlannerMode::Auto;
}

const char* planner_mode_name(PlannerMode m) {
    switch (m) {
        case PlannerMode::Rule: return "rule";
        case PlannerMode::Llm:  return "llm";
        default:                return "auto";
    }
}

std::string normalize_question(const std::string& q) {
    std::string out;
    bool in_space = false, started = false;
    for (unsigned char c : q) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { in_space = true; continue; }
        if (started && in_space) out += ' ';
        in_space = false;
        started = true;
        out += static_cast<char>((c < 0x80) ? std::tolower(c) : c);
    }
    return out;
}

std::optional<LlmPlan> parse_llm_plan(const std::string& json_text) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    LlmPlan p;
    std::string it = j.value("intent", "");
    if      (it == "GeneralFact")     p.intent = QueryIntent::GeneralFact;
    else if (it == "ListByCondition") p.intent = QueryIntent::ListByCondition;
    else if (it == "ClauseLookup")    p.intent = QueryIntent::ClauseLookup;
    else if (it == "MethodLookup")    p.intent = QueryIntent::MethodLookup;
    else return std::nullopt;

    auto arr = [&](const char* k) {
        std::vector<std::string> v;
        if (j.contains(k) && j[k].is_array())
            for (const auto& e : j[k]) if (e.is_string()) v.push_back(e.get<std::string>());
        return v;
    };
    p.key_terms     = arr("key_terms");
    p.section_hints = arr("section_hints");
    p.sparse_text   = j.value("sparse_text", "");
    p.dense_text    = j.value("dense_text", "");

    if (p.intent == QueryIntent::ListByCondition && p.key_terms.empty()) return std::nullopt;
    return p;
}

namespace {
std::string cache_path_for(const std::string& cache_dir, const std::string& question,
                           const std::string& prompt_version) {
    std::string key = normalize_question(question) + "\x1f" + prompt_version;
    size_t h = std::hash<std::string>{}(key);
    char name[32];
    std::snprintf(name, sizeof(name), "%016zx.json", h);
    return cache_dir + "/" + name;
}

std::string read_cache(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void write_cache(const std::string& cache_dir, const std::string& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::ofstream f(path, std::ios::binary);
    if (f) f << content;   // 写失败不致命：下次重算
}
}  // namespace

QueryAnalysis QueryPlanner::plan(const std::string& question) const {
    QueryAnalysis a = analyze_query(question);   // 编号 + 编号驱动 intent

    // 编号问题 / Rule 模式 / 无 LLM 调用 → 规则保底路（含编号与白名单列举）。
    if (mode == PlannerMode::Rule || !a.clause_no.empty() || !a.method_no.empty() || !llm_call)
        return build_query_plan(question, terms);

    // LLM 路：缓存 → 调用 → 解析。
    std::string path = cache_path_for(cache_dir, question, prompt_version);
    std::string js = read_cache(path);
    if (js.empty()) {
        try {
            js = llm_call(kQueryPlannerSystemPrompt, question);
        } catch (const std::exception& e) {
            spdlog::warn("[queryplanner] LLM 调用失败，回退规则: {}", e.what());
            return build_query_plan(question, terms);
        }
        if (parse_llm_plan(js)) write_cache(cache_dir, path, js);   // 只缓存有效产物
    }

    auto parsed = parse_llm_plan(js);
    if (!parsed) {
        spdlog::warn("[queryplanner] LLM 输出非法，回退规则");
        return build_query_plan(question, terms);
    }

    // 无编号场景：采用 LLM 的 intent，但只接受 List/General（Clause/Method 归 General）。
    a.intent = (parsed->intent == QueryIntent::ListByCondition)
                   ? QueryIntent::ListByCondition : QueryIntent::GeneralFact;
    a.key_terms     = parsed->key_terms;
    a.section_hints = parsed->section_hints;
    a.sparse_text   = parsed->sparse_text;
    a.dense_text    = parsed->dense_text;
    spdlog::info("[queryplanner] LLM intent={} sparse=\"{}\"",
                 query_intent_name(a.intent), a.sparse_text);
    return a;
}
