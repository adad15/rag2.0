#pragma once
#include <string>
#include <vector>

struct SplitClause {
    std::string clause_no;
    std::string text;
    int page_start = 0;
};

// 朴素切分：按行扫描，行首匹配 形如 N.N.N / N.N / N.N.N-x 的条款号即起新条款，
// 否则把该行追加到当前条款正文。M1 临时实现，M2 由层级树正规化替换。
std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no);
