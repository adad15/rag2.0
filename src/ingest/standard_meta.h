#pragma once
#include <string>

// 从首页文本里正则抽中文国标/行标标准号（JTG D60-2015 / GB 50010-2010 /
// GB/T 50081-2019 / JGJ 3-2010 等）。抽不到则返回 fallback（通常文件名去扩展名）。
// 纯函数，可单测。M1 临时实现，M2 由正规元数据抽取替换。
std::string extract_standard_no(const std::string& page_text, const std::string& fallback);

// 把标准号归一化为匹配键，吸收 OCR/文件名/排版变体：去空格/下划线/斜杠、
// 各类破折号(— ― – － ‐ − )→'-'、全角数字→半角、ASCII 转小写。其余(中文/括号)原样保留。
// 用于 find_standard_by_code 的鲁棒匹配（如 "JTG/T 3650-2020" ≡ "JTGT 3650—2020"）。纯函数。
std::string normalize_standard_code(const std::string& s);
