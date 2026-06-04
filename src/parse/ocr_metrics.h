#pragma once
#include <string>
#include "parse/parser.h"

struct OcrMetrics {
    int body_candidate = 0;   // region=body & Heading & 标题以数字起（应是条款的候选）
    int body_filled = 0;      // 其中成功抠到 clause_no 的
    int caption_leak = 0;     // 标题以 图/表 起 但 clause_no 非空 且 !is_caption
    int suspect = 0;          // body & suspect != ""
    double conf_min = 1.0, conf_mean = 1.0, conf_max = 1.0;
    bool all_conf_one = true;     // ppstructure 元素置信度是否全为 1.0（疑似写死）
    bool conf_available = false;  // 是否有真实逐块分数（max>0）；PP-Structure 不提供时全 0 → false
    int toc_count = 0;            // region=toc 元素数（信息项）
};

OcrMetrics compute_ocr_metrics(const ParsedDoc& doc);
std::string format_ocr_metrics(const OcrMetrics& m);   // 多行可读体检表
