#pragma once
#include "parse/ocr_backend.h"
#include <string>

// 纯函数：把 PP-Structure 服务返回 JSON 解析为元素流（含 error 检测）。可单测。
std::vector<ParseElement> parse_ppstructure_json(const std::string& json_body);

class PpStructureBackend : public OcrBackend {
public:
    explicit PpStructureBackend(std::string base_url);
    std::vector<ParseElement> ocr_pages(const std::string& file_path,
                                        const std::vector<int>& pages) override;
private:
    std::string base_url_;
};
