#include "parse/ocr_metrics.h"
#include <sstream>

static bool starts_digit(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    return a != std::string::npos && s[a] >= '0' && s[a] <= '9';
}
static bool starts_fig_table(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    std::string t = (a==std::string::npos)? std::string(): s.substr(a);
    return t.compare(0, std::string("图").size(), "图") == 0
        || t.compare(0, std::string("表").size(), "表") == 0;
}

OcrMetrics compute_ocr_metrics(const ParsedDoc& doc) {
    OcrMetrics m;
    double cmin = 2.0, cmax = -1.0, csum = 0.0; int cn = 0;
    for (const auto& e : doc.elements) {
        const std::string& t = e.title.empty() ? e.text : e.title;
        if (e.region == Region::Toc) ++m.toc_count;
        if (e.source == "ppstructure") {
            ++cn; csum += e.ocr_confidence;
            if (e.ocr_confidence < cmin) cmin = e.ocr_confidence;
            if (e.ocr_confidence > cmax) cmax = e.ocr_confidence;
            if (e.ocr_confidence != 1.0f) m.all_conf_one = false;
        }
        if (e.region == Region::Body && e.type == ElementType::Heading && starts_digit(t)) {
            ++m.body_candidate;
            if (!e.clause_no.empty()) ++m.body_filled;
        }
        if (!e.is_caption && !e.clause_no.empty() && starts_fig_table(t)) ++m.caption_leak;
        if (e.region == Region::Body && !e.suspect.empty()) ++m.suspect;
    }
    if (cn > 0) { m.conf_min = cmin; m.conf_max = cmax; m.conf_mean = csum / cn; }
    return m;
}

std::string format_ocr_metrics(const OcrMetrics& m) {
    std::ostringstream os;
    double fill = m.body_candidate ? 100.0 * m.body_filled / m.body_candidate : 0.0;
    os << "==== OCR 体检表 ====\n";
    os << "正文条款候选        : " << m.body_candidate << "\n";
    os << "  其中抠到号        : " << m.body_filled << "  (填充率 " << fill << "%)\n";
    os << "图表题泄漏          : " << m.caption_leak << "  (目标 0)\n";
    os << "可疑条款(seq/short) : " << m.suspect << "\n";
    os << "TOC 元素            : " << m.toc_count << "\n";
    os << "置信度 min/mean/max : " << m.conf_min << " / " << m.conf_mean << " / " << m.conf_max << "\n";
    os << "置信度全为1.0(疑写死): " << (m.all_conf_one ? "是" : "否") << "\n";
    return os.str();
}
