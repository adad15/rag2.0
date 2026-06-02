#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

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
    json body;
    body["file_path"] = file_path;
    body["pages"] = pages;
    auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {});
    if (!res.ok())
        throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
    return parse_ppstructure_json(res.body);
}
