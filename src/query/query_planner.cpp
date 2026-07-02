#include "query/query_planner.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

const char* kQueryPlannerPromptVersion = "qp-v2";

const char* kQueryPlannerSystemPrompt =
    "你是公路工程标准问答系统的查询分析器。给定一个中文问题，只输出一个严格的 JSON 对象，"
    "描述检索计划，不要任何解释或代码块围栏。字段：\n"
    "- intent: 四选一 \"GeneralFact\"|\"ListByCondition\"|\"ClauseLookup\"|\"MethodLookup\"。"
    "问\"哪些/有哪些…用到/需要某仪器或材料\"这类要列举多个试验的→ListByCondition；其余普通问答→GeneralFact。\n"
    "- key_terms: 所有题型都抽 1-4 个最能定位答案的判别词（试验方法名/仪器/材料/指标名），用问题原文词形；"
    "绝不放\"试验/规程/方法/公路/工程/水泥/混凝土/沥青/集料\"这类满库通用词；确无判别词才给空数组。\n"
    "- section_hints: 判别词通常所在章节词（仪器→[\"仪具\",\"材料\"]）；无则空数组。\n"
    "- sparse_text: 给关键词检索的查询=判别词+章节词，空格分隔，去掉\"哪些/的/了\"等虚词与满库背景词；拿不准就留空串。\n"
    "- dense_text: 给向量检索的聚焦重述，一句话，必须保留问题里全部关键实体与限定条件；拿不准就留空串。\n"
    "拿不准或确无判别词时：key_terms 给空数组、sparse_text 和 dense_text 留空串——留空系统会回退用原句检索，不会更差。\n"
    "示例：\n"
    "问：公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平\n"
    "答：{\"intent\":\"ListByCondition\",\"key_terms\":[\"天平\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"天平 仪具 材料\",\"dense_text\":\"使用天平的试验仪具与材料\"}\n"
    "问：环球法测沥青软化点时，试样制备、加热速度和终点判定怎样控制？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"环球法\",\"软化点\"],\"section_hints\":[],\"sparse_text\":\"环球法 软化点 加热速度 终点判定\",\"dense_text\":\"环球法测定沥青软化点的试样制备、加热速度与终点判定\"}\n"
    "问：微型狄法尔法和洛杉矶法评价集料磨耗性能时，试验作用方式与结果指标有什么不同？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"微型狄法尔\",\"洛杉矶\",\"磨耗\"],\"section_hints\":[],\"sparse_text\":\"微型狄法尔 洛杉矶 磨耗\",\"dense_text\":\"微型狄法尔法与洛杉矶法评价集料磨耗性能的作用方式与结果指标区别\"}\n"
    "问：粗集料筛分试验需要准备哪些主要仪具？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"筛分\",\"仪具\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"筛分 仪具 材料\",\"dense_text\":\"粗集料筛分试验的仪具与材料\"}";

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
    // std::hash 不保证跨编译器/构建稳定；旧构建的缓存文件命中不上会被静默忽略并重算（只是优化，无正确性影响）。
    size_t h = std::hash<std::string>{}(key);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(h));
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
    const bool from_cache = !js.empty();
    if (!from_cache) {
        try {
            js = llm_call(kQueryPlannerSystemPrompt, question);
        } catch (const std::exception& e) {
            spdlog::warn("[queryplanner] LLM 调用失败，回退规则: {}", e.what());
            return build_query_plan(question, terms);
        }
    }

    auto parsed = parse_llm_plan(js);            // 只解析一次
    if (parsed && !from_cache) write_cache(cache_dir, path, js);   // 只缓存新获得的有效产物
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
