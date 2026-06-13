#include "query/query_analysis.h"
#include "parse/method_no.h"
#include <regex>

namespace {

std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '\t') out += c;
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

    return a;
}
