#pragma once
#include <string>

// 条款号抠取结果。matched=false 表示该文本不以条款号起。
struct ClauseNoResult {
    bool matched = false;
    std::string clause_no;   // 如 "5.2.1" / "1" / "4.2.1-1"
    std::string rest;        // 抠号后剩余文本（已去首部空白）
};

// 共享条款号文法（poppler 前端与 OCR 前端共用）。纯函数，可单测。
// is_heading=true 时才允许"单级章号"（如 "1总则"），否则只认多级号（>=2 级）。
ClauseNoResult parse_clause_no(const std::string& text, bool is_heading);
