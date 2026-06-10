#include "parse/ocr_normalize.h"
#include "parse/clause_no.h"
#include <algorithm>

// 去前导空白后做 UTF-8 字节前缀匹配（OCR 文本常带前导空格）。
static bool starts_with_trimmed(const std::string& s, const std::string& pre) {
    size_t a = s.find_first_not_of(" \t\r");
    std::string t = (a == std::string::npos) ? std::string() : s.substr(a);
    return t.size() >= pre.size() && t.compare(0, pre.size(), pre) == 0;
}

static bool ascii_alnum(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static bool caption_prefix_match(const std::string& s, const std::string& pre) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos || s.size() - a < pre.size()) return false;
    if (s.compare(a, pre.size(), pre) != 0) return false;

    size_t i = a + pre.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i < s.size() && ascii_alnum(s[i]);
}

bool is_caption_label(const std::string& raw_label, const std::string& title) {
    (void)raw_label;
    if (starts_with_trimmed(title, "图中") || starts_with_trimmed(title, "表中"))
        return false;
    // PP-Structure 的 figure_title/table_title 会误标普通标题；最终以文本形态定性。
    static const char* prefixes[] = {"图", "表", "续表", "附图", "附表"};
    for (auto p : prefixes) if (caption_prefix_match(title, p)) return true;
    return false;
}

// 页面 furniture：页眉/页脚/页码。PP-Structure 已标好 raw_label，按标签直接滤掉最精准。
static bool is_page_furniture(const std::string& raw_label) {
    return raw_label == "header" || raw_label == "footer" || raw_label == "number";
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
    auto single_numeric_chapter = [](const std::string& no) {
        if (no.empty()) return false;
        return std::all_of(no.begin(), no.end(), [](unsigned char c){ return c >= '0' && c <= '9'; });
    };
    auto test_method_no = [](const std::string& no) {
        if (no.empty()) return false;
        char first = no[0];
        return (first == 'T' || first == 't') && no.find('-') != std::string::npos;
    };
    auto looks_like_toc_entry = [](const std::string& t) {
        return t.find("...") != std::string::npos ||
               t.find("\xE2\x80\xA6") != std::string::npos;
    };
    for (auto& e : els) {
        std::string t = title_of(e);
        // 区域标题切换：必须是 Heading（独立章级标题）才切——避免正文里含"条文说明"的
        // text 块误触发(JTG 3432 实例:一个 text 块="条文说明" 曾把 99% 正文锁进 explanation)。
        const bool heading = (e.type == ElementType::Heading);
        if (body_started && heading && test_method_no(e.clause_no)) cur = Region::Body;
        else if ((heading || cur == Region::Appendix) && starts_with_trimmed(t, "条文说明")) cur = Region::Explanation;
        else if (heading && starts_with_trimmed(t, "附录")) cur = Region::Appendix;
        else if (!body_started && heading && starts_with_trimmed(t, "目次")) cur = Region::Toc;

        // 首个合法单级章号（如 "1总则"）→ 正文开始。
        // 假设：正文首章总是单级编号（"1 xxx"）。若文档直接以多级号(如 1.1)起，将停留在前序区域。
        if (!body_started && e.type == ElementType::Heading && !e.is_caption
            && single_numeric_chapter(e.clause_no) && !looks_like_toc_entry(t)) {
            body_started = true;
            cur = Region::Body;
        }
        e.region = cur;
    }
}
// 把 "7.2"/"7.33"/"5.2.1" 拆成整数段，便于比较顺序。
// 任一数字段超过 9 位（OCR 噪声）或出现前导点等畸形 → 返回空，调用方按"无顺序信息"处理（不抛）。
static std::vector<int> split_no(const std::string& no) {
    std::vector<int> v; std::string cur;
    for (char c : no) {
        if (c >= '0' && c <= '9') { cur += c; if (cur.size() > 9) return {}; }  // 超长数字段=噪声，放弃
        else if (c == '.') { if (cur.empty()) return {}; v.push_back(std::stoi(cur)); cur.clear(); }
        else break;   // 遇 '-' 后缀停止
    }
    if (!cur.empty()) v.push_back(std::stoi(cur));
    return v;
}

void flag_anomalies(std::vector<ParseElement>& els) {
    std::vector<int> prev;
    for (auto& e : els) {
        if (e.region != Region::Body || e.clause_no.empty() || e.is_caption) continue;
        // 正文过短（抠号后 text < 9 字节 ≈ 不足 3 个汉字，如 "算："=6 字节）
        if (e.text.size() < 9) { e.suspect = "short"; }
        // 连续性：同级（段数相同）下末段应递增 1，跳变 >1 标 seq。
        // 注意：prev 按"深度变化"而非"父级变化"复位——若某节只有子条款而缺节标题
        // （如缺 5.3 标题、只有 5.3.x），可能把 5.2→5.4 误标 seq。仅作提示，可接受。
        auto cur = split_no(e.clause_no);
        if (!prev.empty() && cur.size() == prev.size()) {
            bool same_parent = std::equal(cur.begin(), cur.end()-1, prev.begin());
            if (same_parent && cur.back() - prev.back() > 1 && e.suspect.empty())
                e.suspect = "seq";
        }
        prev = cur;
    }
}

void normalize_parsed_doc(ParsedDoc& doc) {
    auto& els = doc.elements;
    // 1) 丢弃：页眉/页脚/页码（raw_label）+ 独立英文糊块。仅对 ppstructure 源、非表格。
    els.erase(std::remove_if(els.begin(), els.end(), [](const ParseElement& e){
        if (e.source != "ppstructure" || !e.table_html.empty()) return false;
        if (is_page_furniture(e.raw_label)) return true;
        return is_english_garble(e.title.empty()? e.text : e.title);
    }), els.end());
    // 2) caption + clause_no
    apply_clause_extraction(els);
    // 3) region
    tag_regions(els);
    // 4) 异常标记
    flag_anomalies(els);
    doc.schema_version = 2;
}
