#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>

using nlohmann::json;

// 每批送多少页给 OCR 服务（批越小、单次请求越短、越不易撞读超时；失败只丢一批，便于看进度）。
static constexpr size_t kOcrBatchPages = 8;
// OCR 单批读超时（秒）：8 页 × 高 DPI 慢卡每页数秒-数十秒，给足余量。
static constexpr int kOcrReadTimeoutSec = 1200;

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
        pe.raw_label = e.value("raw_label", "");
        // region / is_caption 不从服务 JSON 读：由 normalize_parsed_doc 在下游统一打标。
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
    const size_t nbatch = (pages.size() + kOcrBatchPages - 1) / kOcrBatchPages;
    for (size_t i = 0, b = 1; i < pages.size(); i += kOcrBatchPages, ++b) {
        std::vector<int> batch(pages.begin() + i,
                               pages.begin() + std::min(pages.size(), i + kOcrBatchPages));
        spdlog::info("OCR 批次 {}/{}：发送第 {}~{} 页（{} 页），等待服务返回…（逐页进度见 OCR 服务窗口）",
                     b, nbatch, batch.front(), batch.back(), batch.size());
        json body;
        body["file_path"] = file_path;
        body["pages"] = batch;
        auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {},
                                   kOcrReadTimeoutSec);
        if (!res.ok())
            throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
        auto els = parse_ppstructure_json(res.body);
        spdlog::info("OCR 批次 {}/{}：返回 {} 个元素", b, nbatch, els.size());
        all.insert(all.end(), els.begin(), els.end());
    }
    return all;
}
