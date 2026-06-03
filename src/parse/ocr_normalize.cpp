#include "parse/ocr_normalize.h"
#include "parse/clause_no.h"

// UTF-8 字节前缀匹配。
static bool starts_with(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

bool is_caption_label(const std::string& raw_label, const std::string& title) {
    if (raw_label == "table_title" || raw_label == "figure_title" || raw_label == "chart_title")
        return true;
    // 前缀兜底：图 / 表 / 续表 / 附图 / 附表（允许前导空白）
    size_t a = title.find_first_not_of(" \t\r");
    std::string t = (a == std::string::npos) ? std::string() : title.substr(a);
    static const char* prefixes[] = {"图", "表", "续表", "附图", "附表"};
    for (auto p : prefixes) if (starts_with(t, p)) return true;
    return false;
}

bool is_english_garble(const std::string& text) {
    if (text.empty()) return false;
    size_t ascii = 0, total = 0;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        ++total;
        if (c < 0x80) ++ascii;
    }
    return total > 0 && (double)ascii / total > 0.80;
}

void apply_clause_extraction(std::vector<ParseElement>& els) {
    for (auto& e : els) {
        std::string& src = e.title.empty() ? e.text : e.title;
        if (is_caption_label(e.raw_label, src)) {
            e.is_caption = true;
            continue;                 // caption 不抠号
        }
        bool is_heading = (e.type == ElementType::Heading);
        auto r = parse_clause_no(src, is_heading);
        if (r.matched) {
            e.clause_no = r.clause_no;
            if (e.text.empty() || e.type == ElementType::Heading) e.text = r.rest;
        }
    }
}

// 以下三个本任务留桩，后续任务实现。
void tag_regions(std::vector<ParseElement>&) {}
void flag_anomalies(std::vector<ParseElement>&) {}
void normalize_parsed_doc(ParsedDoc& doc) { apply_clause_extraction(doc.elements); }
