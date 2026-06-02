#pragma once
#include <string>
#include "parse/parser.h"

// ParsedDoc <-> JSON（含 pages 与 elements）。纯函数，可单测。
std::string parsed_doc_to_json(const ParsedDoc& doc);
ParsedDoc   parsed_doc_from_json(const std::string& json_text);

// 磁盘缓存：写 data/parse_cache/<标准>.json（目录不存在则创建）。
void        write_parse_cache(const std::string& cache_path, const ParsedDoc& doc);
