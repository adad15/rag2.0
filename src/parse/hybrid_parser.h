#pragma once
#include <vector>
#include <string>
#include "parse/parser.h"
#include "parse/ocr_backend.h"

enum class ParseMode { Auto, Poppler, Ocr };

// 纯函数：给定每页字节数，按模式与阈值算出需 OCR 的页号（1-based）。
std::vector<int> pick_ocr_pages(const std::vector<int>& bytes_per_page,
                                ParseMode mode, int threshold);

// 纯函数：合并 poppler 基础文档与 OCR 页元素。
// - OCR 页：text = 该页 OCR 元素文本拼接；elements = 该页 OCR 元素。
// - 非 OCR 页：保留 poppler text；并为该页生成一个 Text 元素(source=poppler)。
// - elements 按页号升序排列。
ParsedDoc merge_doc(const ParsedDoc& poppler_doc,
                    const std::vector<int>& ocr_pages,
                    const std::vector<ParseElement>& ocr_elements);

// 契约① 实现：逐页路由（poppler 文字页 + OcrBackend 扫描页），合并为 ParsedDoc。
class HybridParser : public Parser {
public:
    HybridParser(Parser& poppler, OcrBackend& ocr, ParseMode mode, int threshold);
    ParsedDoc parse(const std::string& file_path) override;
private:
    Parser& poppler_;
    OcrBackend& ocr_;
    ParseMode mode_;
    int threshold_;
};
