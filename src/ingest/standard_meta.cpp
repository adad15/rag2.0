#include "ingest/standard_meta.h"
#include <regex>

std::string extract_standard_no(const std::string& page_text, const std::string& fallback) {
    // 前缀大写字母(>=2) + 可选 /字母 + 空白 + 可选字母段 + 数字(可带小数) + - + 四位年份(19xx/20xx)
    // 覆盖 JTG D60-2015 / GB 50010-2010 / GB/T 50081-2019 / JGJ 3-2010 / TB 10002-2017。
    static const std::regex pat(
        R"([A-Z]{2,}(?:/[A-Z]+)?\s*[A-Z]{0,2}\d+(?:\.\d+)?-(?:19|20)\d{2})");
    std::smatch m;
    if (std::regex_search(page_text, m, pat)) return m[0].str();
    return fallback;
}
