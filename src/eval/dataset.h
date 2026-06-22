#pragma once
#include <string>
#include <vector>

// 评估样本（M4 检索评估核心）。两种 gold：
//  - 点查：gold_clause_no（配 gold_standard_no）或 gold_method_no 任一非空。
//  - 覆盖查：gold_methods 非空——衡量召回覆盖到这组 method_no 的多少。
struct EvalCase {
    std::string question;
    std::string note;                       // 备注/题型（可空）
    std::string gold_standard_no;           // 标准号文本，如 "JTG 3420"（可空）
    std::string gold_clause_no;             // 条款号，如 "5.1.2"（可空）
    std::string gold_method_no;             // 方法号，如 "T0521-2005"（可空）
    std::vector<std::string> gold_methods;  // 覆盖查 gold（空=非覆盖查）
    std::vector<std::string> gold_values;   // 数值题期望出现的限值/单位（空=非数值题）
};

// 解析评估集 JSON（对象数组）。缺字段取默认（空）。非数组/解析失败抛 std::runtime_error。纯函数。
std::vector<EvalCase> parse_dataset(const std::string& json_text);
