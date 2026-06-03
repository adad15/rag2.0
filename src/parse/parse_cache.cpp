#include "parse/parse_cache.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <spdlog/spdlog.h>

using nlohmann::json;

std::string parsed_doc_to_json(const ParsedDoc& doc) {
    json j;
    j["source_path"] = doc.source_path;
    j["title"] = doc.title;
    j["standard_no"] = doc.standard_no;
    j["schema_version"] = doc.schema_version;
    j["pages"] = json::array();
    for (const auto& p : doc.pages)
        j["pages"].push_back({ {"page_no", p.page_no}, {"text", p.text} });
    j["elements"] = json::array();
    for (const auto& e : doc.elements) {
        j["elements"].push_back({
            {"type", element_type_to_string(e.type)},
            {"page_no", e.page_no}, {"level", e.level},
            {"clause_no", e.clause_no}, {"title", e.title}, {"text", e.text},
            {"table_html", e.table_html}, {"caption", e.caption},
            {"source", e.source}, {"ocr_confidence", e.ocr_confidence},
            {"raw_label", e.raw_label}, {"region", region_to_string(e.region)},
            {"is_caption", e.is_caption}, {"suspect", e.suspect}
        });
    }
    return j.dump(2);
}

ParsedDoc parsed_doc_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    ParsedDoc d;
    if (!j.is_object()) return d;
    d.source_path = j.value("source_path", "");
    d.title = j.value("title", "");
    d.standard_no = j.value("standard_no", "");
    d.schema_version = j.value("schema_version", 1);
    if (j.contains("pages") && j["pages"].is_array())
        for (auto& p : j["pages"]) {
            ParsedPage pp;
            pp.page_no = p.value("page_no", 0);
            pp.text = p.value("text", "");
            d.pages.push_back(std::move(pp));
        }
    if (j.contains("elements") && j["elements"].is_array())
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
            pe.source = e.value("source", "");
            pe.ocr_confidence = e.value("ocr_confidence", 1.0f);
            pe.raw_label = e.value("raw_label", "");
            pe.region = region_from_string(e.value("region", "body"));
            pe.is_caption = e.value("is_caption", false);
            pe.suspect = e.value("suspect", "");
            d.elements.push_back(std::move(pe));
        }
    return d;
}

void write_parse_cache(const std::string& cache_path, const ParsedDoc& doc) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary);  // binary：避免 Windows CRLF 改写 UTF-8 JSON
    if (!f) { spdlog::warn("parse cache 写入失败，无法打开: {}", cache_path); return; }
    f << parsed_doc_to_json(doc);
}
