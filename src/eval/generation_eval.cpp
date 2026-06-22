#include "eval/generation_eval.h"
#include "eval/generation_metrics.h"
#include "generate/answer_pipeline.h"
#include "generate/prompt_builder.h"   // kAnswerPromptVersion
#include "query/query_planner.h"       // normalize_question
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <cstdio>

namespace {
std::string cache_path_for(const std::string& dir, const std::string& q) {
    std::string key = normalize_question(q) + "\x1f" + kAnswerPromptVersion;
    size_t h = std::hash<std::string>{}(key);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.txt", static_cast<unsigned long long>(h));
    return dir + "/" + name;
}
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
void write_file(const std::string& dir, const std::string& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(path, std::ios::binary);
    if (f) f << content;   // 写失败非致命：下次重算
}
// "T0521-2005" -> "T0521"；无 '-' 原样返回。答案多半只写不带年份的方法号。
std::string method_stem(const std::string& m) { return m.substr(0, m.find('-')); }
}  // namespace

GenerationReport run_generation_eval(
    const std::vector<EvalCase>& cases,
    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
    const SynonymDict& syn, deepseek::DeepSeekClient& ds,
    const std::string& collection, int top_k,
    const QueryPlanner& planner, const std::string& answer_cache_dir) {

    GenerationReport rep;
    for (const auto& c : cases) {
        // 选引用 gold：方法号优先（取 stem）；否则条款号+标准号。
        std::string gold_ref, gold_std;
        if (!c.gold_method_no.empty()) {
            gold_ref = method_stem(c.gold_method_no);     // gold_std 留空：方法题无 gold_standard_no
        } else if (!c.gold_clause_no.empty() && !c.gold_standard_no.empty()) {
            gold_ref = c.gold_clause_no;
            gold_std = c.gold_standard_no;
        } else if (!c.gold_clause_no.empty()) {
            // spec §7：有条款号但缺 gold_standard_no → 引用不计分并告警。
            spdlog::warn("[gen-eval] 条款题缺 gold_standard_no，引用不计分: {}", c.question);
        }
        const bool want_cite = !gold_ref.empty();
        const bool want_num  = !c.gold_values.empty();
        if (!want_cite && !want_num) continue;            // 无生成 gold（如纯覆盖题）→ 跳过，不调 LLM

        // 取缓存答案；未命中则调 answer_query 并缓存。
        std::string path = cache_path_for(answer_cache_dir, c.question);
        std::string answer = read_file(path);
        if (answer.empty()) {
            try {
                answer = answer_query(c.question, mv, embed, pg, syn, ds, collection, top_k, planner);
            } catch (const std::exception& e) {
                spdlog::warn("[gen-eval] answer_query 失败，跳过该条: {} ({})", c.question, e.what());
                continue;                                  // 该条不计分
            }
            // 不缓存拒答串（如 Milvus 短暂不可用时的兜底答复），避免污染后续 eval。
            if (!answer.empty() && answer.find("无法作答") == std::string::npos)
                write_file(answer_cache_dir, path, answer);
        }

        GenCaseResult cr;
        cr.question = c.question;
        if (want_cite) {
            cr.cite_scored = true;
            cr.cite_hit = citation_hit(answer, gold_std, gold_ref);
            rep.cite_scored++;
            if (cr.cite_hit) rep.cite_hits++;
        }
        if (want_num) {
            cr.value_gold = static_cast<int>(c.gold_values.size());
            cr.value_hits = count_value_hits(answer, c.gold_values);
            rep.value_gold_total += cr.value_gold;
            rep.value_hit_total  += cr.value_hits;
        }
        rep.results.push_back(cr);
    }
    return rep;
}
