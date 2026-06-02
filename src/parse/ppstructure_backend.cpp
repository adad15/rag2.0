#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>

using nlohmann::json;

// 每批送多少页给 OCR 服务（避免单次请求几十分钟、便于看进度、失败只丢一批）。
static constexpr size_t kOcrBatchPages = 16;
// OCR 单批读超时（秒）：一批 16 页 × 慢卡数秒/页，给足余量。
static constexpr int kOcrReadTimeoutSec = 600;

std::vector<ParseElement> parse_ppstructure_json(const std::string& json_body) {
    auto j = json::parse(json_body);   // 解析失败抛异常（交由上层 try/catch）
    if (j.contains("error"))
        throw std::runtime_error("ppstructure service error: " +
                                 j["error"].get<std::string>());
    std::vector<ParseElement> out;
    if (!j.contains("elements") || !j["elements"].is_array()) return out;
    for (auto& e : j["elements"]) {
        ParseElement pe;
        pe.type = element_type_from_string(e.value("type", "Text"));
        pe.page_no = e.value("page_no", 0);
        pe.level = e.value("level", 0);
        pe.clause_no = e.value("clause_no", "");
        pe.title = e.value("title", "");
        pe.text = e.value("text", "");
        pe.table_html = e.value("table_html", "");
        pe.caption = e.value("caption", "");
        pe.ocr_confidence = e.value("ocr_confidence", 1.0f);
        pe.source = "ppstructure";
        out.push_back(std::move(pe));
    }
    return out;
}

PpStructureBackend::PpStructureBackend(std::string base_url)
    : base_url_(std::move(base_url)) {}

std::vector<ParseElement> PpStructureBackend::ocr_pages(const std::string& file_path,
                                                        const std::vector<int>& pages) {
    std::vector<ParseElement> all;
    for (size_t i = 0; i < pages.size(); i += kOcrBatchPages) {
        std::vector<int> batch(pages.begin() + i,
                               pages.begin() + std::min(pages.size(), i + kOcrBatchPages));
        json body;
        body["file_path"] = file_path;
        body["pages"] = batch;
        auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {},
                                   kOcrReadTimeoutSec);
        if (!res.ok())
            throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
        auto els = parse_ppstructure_json(res.body);
        all.insert(all.end(), els.begin(), els.end());
    }
    return all;
}
