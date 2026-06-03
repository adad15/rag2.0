#pragma once
#include <string>
#include <vector>
#include "parse/parser.h"

// 是否图/表题：raw_label 命中 table_title/figure_title/chart_title，或标题以 图/表/续表/附图/附表 起。
bool is_caption_label(const std::string& raw_label, const std::string& title);

// 整篇几乎全英文且不成词（OCR 糊块）。
bool is_english_garble(const std::string& text);

// 对元素流做：caption 标记 + clause_no 抠取（caption 不抠）。就地修改。
void apply_clause_extraction(std::vector<ParseElement>& els);

// 顺序扫元素流，按 目次/附录/条文说明 标题与首章检测打 region 标签。就地修改。
void tag_regions(std::vector<ParseElement>& els);

// 抠号后：连续性(seq) 与正文过短(short) 检查，写 .suspect。就地修改。
void flag_anomalies(std::vector<ParseElement>& els);

// M2b 总入口：对 doc.elements 依次跑 上面四步 + 丢弃独立英文糊块。就地修改 doc。
void normalize_parsed_doc(ParsedDoc& doc);
