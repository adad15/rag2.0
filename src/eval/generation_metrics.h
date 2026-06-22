#pragma once
#include <string>
#include <vector>

// 归一化用于匹配：去所有空白、全角数字(０-９)→半角、ASCII 大写→小写。纯函数。
std::string normalize_for_match(const std::string& s);

// 引用命中：归一化后答案同时包含 标准号 与 条款号/方法号 → true。
// gold_standard_code 为空串时视为"无标准号要求"，只校验 gold_ref。
// gold_ref 为空串时直接返回 false（无可校验的引用）。
bool citation_hit(const std::string& answer,
                  const std::string& gold_standard_code,
                  const std::string& gold_ref);

// 数值命中数：归一化后 gold_values 里有几个作为子串出现在答案中。
int count_value_hits(const std::string& answer,
                     const std::vector<std::string>& gold_values);
