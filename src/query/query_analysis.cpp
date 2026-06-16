#include "query/query_analysis.h"
#include "parse/method_no.h"
#include <regex>

namespace {

std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '\t') out += c;
    return out;
}

bool contains_any(const std::string& text, const std::vector<std::string>& words) {
    for (const auto& w : words)
        if (!w.empty() && text.find(w) != std::string::npos) return true;
    return false;
}

// 明确列举信号（硬编码，不依赖词表）。
const std::vector<std::string>& list_markers() {
    static const std::vector<std::string> m = {"哪些", "有哪些", "哪几项", "哪几种"};
    return m;
}

QueryIntent classify_intent(const QueryAnalysis& a, const std::string& question) {
    if (!a.clause_no.empty())  return QueryIntent::ClauseLookup;
    if (!a.method_no.empty())  return QueryIntent::MethodLookup;
    if (contains_any(question, list_markers())) return QueryIntent::ListByCondition;
    return QueryIntent::GeneralFact;
}

std::string join_terms(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}

}  // namespace

QueryAnalysis analyze_query(const std::string& question) {
    QueryAnalysis a;
    a.clean_text = question;

    // 标准号：比入库 extract_standard_no 更宽松——年份段可选，
    // 因为用户常省年份（"JTG 3420"）。需 >=2 个大写字母前缀，
    // 故单字母方法号 "T0302" 不会被误判为标准号。
    {
        static const std::regex pat(
            R"([A-Z]{2,}(?:/[A-Z]+)?[ \t]*[A-Z]{0,2}\d+(?:\.\d+)?(?:-(?:19|20)\d{2})?)");
        std::smatch m;
        if (std::regex_search(question, m, pat)) a.standard_code = strip_spaces(m[0].str());
    }

    // 条款号：可选"第"前缀 + 多级号（>=2 级，必含小数点）+ 可选"条"。
    // 误命中（如散文里的 "0.98"）无害：置顶时查不到该条款即空操作。
    {
        static const std::regex pat(R"((?:第\s*)?(\d+(?:\.\d+)+)\s*条?)");
        std::smatch m;
        if (std::regex_search(question, m, pat)) a.clause_no = m[1].str();
    }

    // 方法号：与入库共用同一抽取函数，年份可选。
    a.method_no = extract_method_no(question);

    a.intent = classify_intent(a, question);
    return a;
}

QueryAnalysis build_query_plan(const std::string& question, const QueryTerms& terms) {
    QueryAnalysis a = analyze_query(question);
    a.key_terms     = match_terms(question, terms.instruments);
    a.section_hints = match_terms(question, terms.sections);

    // 仅改写"列举 + 命中仪器"的查询；其它一律留空 → 调用方回退 clean_text。
    if (a.intent == QueryIntent::ListByCondition && !a.key_terms.empty()) {
        if (a.section_hints.empty())
            a.section_hints = {"仪具", "材料"};   // 仪器类列举的默认章节

        std::vector<std::string> bag = a.key_terms;
        bag.insert(bag.end(), a.section_hints.begin(), a.section_hints.end());
        a.sparse_text = join_terms(bag, " ");
        a.dense_text  = "查找试验方法中" + join_terms(a.section_hints, "和")
                      + "包含" + join_terms(a.key_terms, "和") + "的段落";
    }
    return a;
}

const char* query_intent_name(QueryIntent intent) {
    switch (intent) {
        case QueryIntent::ClauseLookup:    return "ClauseLookup";
        case QueryIntent::MethodLookup:    return "MethodLookup";
        case QueryIntent::ListByCondition: return "ListByCondition";
        default:                           return "GeneralFact";
    }
}
