#pragma once
#include <string>

struct QueryAnalysis {
    std::string clean_text;    // 原始问题（dense 路输入，不抠编号）
    std::string standard_code; // 归一化裸代号，如 "JTC5210-2018" / "JTG3420"（空=未指定）
    std::string clause_no;     // 归一化条款号，如 "5.1.2"（空=未指定）
    std::string method_no;     // 归一化方法号，如 "T0302"（空=未指定）
};

// 查询理解（纯函数）：抽取标准号/条款号/方法号并归一化。
QueryAnalysis analyze_query(const std::string& question);
