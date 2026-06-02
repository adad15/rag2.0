#pragma once
#include <string>
#include <vector>
#include "parse/parser.h"

// 可插拔 OCR 后端：对指定页做 OCR，返回这些页的归一化元素。
// 本轮实现 PpStructureBackend；MinerU/VL-API/Tesseract 实现此接口即可接入，HybridParser 不改。
class OcrBackend {
public:
    virtual ~OcrBackend() = default;
    virtual std::vector<ParseElement> ocr_pages(const std::string& file_path,
                                                const std::vector<int>& pages) = 0;
};
