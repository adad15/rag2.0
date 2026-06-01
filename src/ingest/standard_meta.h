#pragma once
#include <string>

// 从首页文本里正则抽中文国标/行标标准号（JTG D60-2015 / GB 50010-2010 /
// GB/T 50081-2019 / JGJ 3-2010 等）。抽不到则返回 fallback（通常文件名去扩展名）。
// 纯函数，可单测。M1 临时实现，M2 由正规元数据抽取替换。
std::string extract_standard_no(const std::string& page_text, const std::string& fallback);
