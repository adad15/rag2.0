#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>

using nlohmann::json;

// 每批送多少页给 OCR 服务（批越小、单次请求越短、越不易撞读超时；失败只丢一批，便于看进度）。
static constexpr size_t kOcrBatchPages = 4;
// OCR 单批读超时（秒）：4 页 × 高 DPI 慢卡，表格页可达 300s+/页，给足余量(4×~300s 留头)。
static constexpr int kOcrReadTimeoutSec = 2400;

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

// 不再自建磁盘缓存：唯一的 OCR 缓存是 Python 服务侧的“原始料缓存”(data/ocr_raw_cache)。
// 命中缓存的页由服务秒回(不渲染、不跑 GPU)，未命中才走模型；断点续传也由服务侧按页落盘保证。
// 代价：重跑 ingest 必须开着 OCR 服务（即便只是从缓存秒拿）。
std::vector<ParseElement> PpStructureBackend::ocr_pages(const std::string& file_path,
                                                        const std::vector<int>& pages) {
    std::vector<ParseElement> all;
    // 分批仅为请求体大小/超时安全；服务按页缓存，命中页秒回。
    const size_t nbatch = (pages.size() + kOcrBatchPages - 1) / kOcrBatchPages;
    for (size_t i = 0, b = 1; i < pages.size(); i += kOcrBatchPages, ++b) {
        std::vector<int> batch(pages.begin() + i,
                               pages.begin() + std::min(pages.size(), i + kOcrBatchPages));
        spdlog::info("OCR 批次 {}/{}：请求第 {}~{} 页（{} 页），超时 {}s…"
                     "（命中原始料缓存的页秒回，未命中走 GPU；逐页进度见服务窗口）",
                     b, nbatch, batch.front(), batch.back(), batch.size(), kOcrReadTimeoutSec);
        json body;
        body["file_path"] = file_path;
        body["pages"] = batch;
        auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {},
                                   kOcrReadTimeoutSec);
        if (!res.ok())
            throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
        auto els = parse_ppstructure_json(res.body);
        all.insert(all.end(), els.begin(), els.end());
        spdlog::info("OCR 批次 {}/{}：返回 {} 个元素", b, nbatch, els.size());
    }
    return all;
}
