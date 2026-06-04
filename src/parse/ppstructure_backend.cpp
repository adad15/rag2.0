#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <map>

using nlohmann::json;

// 每批送多少页给 OCR 服务（批越小、单次请求越短、越不易撞读超时；失败只丢一批，便于看进度）。
static constexpr size_t kOcrBatchPages = 4;
// OCR 单批读超时（秒）：4 页 × 高 DPI 慢卡每页数十秒-上百秒，给足余量。
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

// 把 ParseElement 序列化回"服务元素"形状（字段与 parse_ppstructure_json 读取的一致）：
// 逐页 OCR 缓存写盘用此；读盘直接复用 parse_ppstructure_json，往返字段一致。
static json element_to_service_json(const ParseElement& e) {
    return json{
        {"type", element_type_to_string(e.type)}, {"page_no", e.page_no},
        {"level", e.level}, {"clause_no", e.clause_no}, {"title", e.title},
        {"text", e.text}, {"table_html", e.table_html}, {"caption", e.caption},
        {"ocr_confidence", e.ocr_confidence}, {"raw_label", e.raw_label}
    };
}

// 逐页 OCR 缓存目录（按文件路径 hash，与 standard_id 同源）。data/ 已 gitignore。
// ⚠️ 仅按文件路径键控：改了 OCR 设置（DPI/去水印/app.py 逻辑）后，必须手动删
//    data/ocr_cache/<id>/（或整个 data/ocr_cache/）才会重新 OCR，否则会复用旧结果。
static std::string ocr_cache_dir(const std::string& file_path) {
    return "data/ocr_cache/" + std::to_string(std::hash<std::string>{}(file_path));
}
static std::string page_cache_path(const std::string& dir, int page) {
    return dir + "/p" + std::to_string(page) + ".json";
}

PpStructureBackend::PpStructureBackend(std::string base_url)
    : base_url_(std::move(base_url)) {}

std::vector<ParseElement> PpStructureBackend::ocr_pages(const std::string& file_path,
                                                        const std::vector<int>& pages) {
    namespace fs = std::filesystem;
    const std::string dir = ocr_cache_dir(file_path);
    std::error_code ec;
    fs::create_directories(dir, ec);

    // 1) 断点续传：跳过磁盘已缓存的页，只 OCR 缺的页
    std::vector<int> missing;
    for (int p : pages)
        if (!fs::exists(page_cache_path(dir, p))) missing.push_back(p);
    if (missing.size() < pages.size())
        spdlog::info("OCR 续传：{} 页已缓存，仅需 OCR {} 页（共 {} 页）",
                     pages.size() - missing.size(), missing.size(), pages.size());

    // 2) 仅对缺页分批 OCR；每批返回后按页(含空页)立即落盘(临时文件+改名，防半截文件)
    const size_t nbatch = (missing.size() + kOcrBatchPages - 1) / kOcrBatchPages;
    for (size_t i = 0, b = 1; i < missing.size(); i += kOcrBatchPages, ++b) {
        std::vector<int> batch(missing.begin() + i,
                               missing.begin() + std::min(missing.size(), i + kOcrBatchPages));
        spdlog::info("OCR 批次 {}/{}：发送第 {}~{} 页（{} 页），超时 {}s…（逐页进度见服务窗口）",
                     b, nbatch, batch.front(), batch.back(), batch.size(), kOcrReadTimeoutSec);
        json body;
        body["file_path"] = file_path;
        body["pages"] = batch;
        auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {},
                                   kOcrReadTimeoutSec);
        if (!res.ok())
            throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
        auto els = parse_ppstructure_json(res.body);

        std::map<int, json> by_page;
        for (int p : batch) by_page[p] = json::array();
        for (const auto& e : els)
            if (by_page.count(e.page_no)) by_page[e.page_no].push_back(element_to_service_json(e));
        for (int p : batch) {
            json wrap;
            wrap["elements"] = by_page[p];
            const std::string path = page_cache_path(dir, p);
            const std::string tmp = path + ".tmp";
            { std::ofstream f(tmp, std::ios::binary); if (f) f << wrap.dump(); }
            fs::rename(tmp, path, ec);
        }
        spdlog::info("OCR 批次 {}/{}：返回 {} 个元素，已落盘第 {}~{} 页",
                     b, nbatch, els.size(), batch.front(), batch.back());
    }

    // 3) 按请求页顺序从缓存装配（新 OCR 的也回读，路径统一并确认已持久化）
    std::vector<ParseElement> all;
    for (int p : pages) {
        std::ifstream f(page_cache_path(dir, p), std::ios::binary);
        if (!f) continue;
        std::stringstream ss; ss << f.rdbuf();
        auto els = parse_ppstructure_json(ss.str());
        all.insert(all.end(), els.begin(), els.end());
    }
    return all;
}
