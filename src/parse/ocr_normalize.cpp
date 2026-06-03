#include "parse/ocr_normalize.h"
#include "parse/clause_no.h"

// 去前导空白后做 UTF-8 字节前缀匹配（OCR 文本常带前导空格）。
static bool starts_with_trimmed(const std::string& s, const std::string& pre) {
    size_t a = s.find_first_not_of(" \t\r");
    std::string t = (a == std::string::npos) ? std::string() : s.substr(a);
    return t.size() >= pre.size() && t.compare(0, pre.size(), pre) == 0;
}

bool is_caption_label(const std::string& raw_label, const std::string& title) {
    if (raw_label == "table_title" || raw_label == "figure_title" || raw_label == "chart_title")
        return true;
    // 前缀兜底：图 / 表 / 续表 / 附图 / 附表（允许前导空白）
    static const char* prefixes[] = {"图", "表", "续表", "附图", "附表"};
    for (auto p : prefixes) if (starts_with_trimmed(title, p)) return true;
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
            // Heading（或 text 为空）：把号从标题剥到 text，供下游统一读 text。
            // 正文 Text 元素：仅打 clause_no，保留原文不剥（避免破坏正文）。
            if (e.text.empty() || e.type == ElementType::Heading) e.text = r.rest;
        }
    }
}

void tag_regions(std::vector<ParseElement>& els) {
    Region cur = Region::FrontMatter;
    bool body_started = false;
    auto title_of = [](const ParseElement& e){ return e.title.empty() ? e.text : e.title; };
    for (auto& e : els) {
        std::string t = title_of(e);
        // 区域标题切换（优先级：条文说明/附录/目次 标题）
        if (starts_with_trimmed(t, "条文说明")) cur = Region::Explanation;
        else if (starts_with_trimmed(t, "附录")) cur = Region::Appendix;
        else if (!body_started && starts_with_trimmed(t, "目次")) cur = Region::Toc;

        // 首个合法单级章号（如 "1总则"）→ 正文开始。
        // 假设：正文首章总是单级编号（"1 xxx"）。若文档直接以多级号(如 1.1)起，将停留在前序区域。
        if (!body_started && e.type == ElementType::Heading && !e.is_caption
            && !e.clause_no.empty() && e.clause_no.find('.') == std::string::npos) {
            body_started = true;
            cur = Region::Body;
        }
        e.region = cur;
    }
}
void flag_anomalies(std::vector<ParseElement>&) {}
// 本轮仅抠号；英文糊过滤 + region + 异常标记在后续 Task 接入。
void normalize_parsed_doc(ParsedDoc& doc) { apply_clause_extraction(doc.elements); }
