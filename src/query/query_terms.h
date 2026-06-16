#pragma once
#include <string>
#include <vector>

// 查询词表（方案 B §4.3）。Phase 1 只消费 instruments + sections；
// background/stopwords 预留给 Phase 2 的 GeneralFact 清洗。
struct QueryTerms {
    std::vector<std::string> stopwords;   // 句式停用词（预留）
    std::vector<std::string> background;  // 领域背景词（预留）
    std::vector<std::string> sections;    // 章节提示词
    std::vector<std::string> instruments; // 核心条件词白名单
};

// 从分节文件加载（[stopword]/[background]/[section]/[instrument] 标头；
// '#' 开头与空行忽略）。文件不存在 → 返回空表（调用方回退原句）。
QueryTerms load_query_terms(const std::string& path);

// 纯函数：返回 dict 中作为 text 子串出现的词，按 dict 顺序、去重。
// 提取式（非删除式）：天然规避"试验筛"被按"试验"切成"筛"的问题。
std::vector<std::string> match_terms(const std::string& text,
                                     const std::vector<std::string>& dict);
