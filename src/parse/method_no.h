#pragma once
#include <string>

// 从文本中抽取试验方法号（如 "T 0302-2024" / "T0302" / 全角破折号变体），
// 归一化为无空格、半角破折号形式（"T0302-2024" / "T0302"）。年份可选。
// 无匹配返回 ""。入库侧与查询侧共用，保证两侧字符串逐字可比。纯函数。
std::string extract_method_no(const std::string& text);
