#include "parse/hybrid_parser.h"
#include "parse/ocr_normalize.h"
#include <set>

std::vector<int> pick_ocr_pages(const std::vector<int>& bytes_per_page,
                                ParseMode mode, int threshold) {
    std::vector<int> out;
    for (size_t i = 0; i < bytes_per_page.size(); ++i) {
        int page = (int)i + 1;
        if (mode == ParseMode::Poppler) continue;
        if (mode == ParseMode::Ocr) { out.push_back(page); continue; }
        if (bytes_per_page[i] < threshold) out.push_back(page);   // Auto
    }
    return out;
}

ParsedDoc merge_doc(const ParsedDoc& base,
                    const std::vector<int>& ocr_pages,
                    const std::vector<ParseElement>& ocr_elements) {
    std::set<int> ocr_set(ocr_pages.begin(), ocr_pages.end());
    ParsedDoc out;
    out.source_path = base.source_path;
    out.title = base.title;
    out.standard_no = base.standard_no;

    for (const auto& p : base.pages) {
        ParsedPage np;
        np.page_no = p.page_no;
        if (ocr_set.count(p.page_no)) {
            std::string merged;
            for (const auto& e : ocr_elements)
                if (e.page_no == p.page_no && !e.text.empty()) {
                    if (!merged.empty()) merged += "\n";
                    merged += e.text;
                }
            np.text = merged;
        } else {
            np.text = p.text;
        }
        out.pages.push_back(std::move(np));
    }

    for (const auto& p : base.pages) {
        if (ocr_set.count(p.page_no)) {
            for (const auto& e : ocr_elements)
                if (e.page_no == p.page_no) out.elements.push_back(e);
        } else if (!p.text.empty()) {
            ParseElement e;
            e.type = ElementType::Text;
            e.page_no = p.page_no;
            e.text = p.text;
            e.source = "poppler";
            out.elements.push_back(std::move(e));
        }
    }
    return out;
}

HybridParser::HybridParser(Parser& poppler, OcrBackend& ocr, ParseMode mode, int threshold)
    : poppler_(poppler), ocr_(ocr), mode_(mode), threshold_(threshold) {}

ParsedDoc HybridParser::parse(const std::string& file_path) {
    ParsedDoc base = poppler_.parse(file_path);
    std::vector<int> bytes;
    bytes.reserve(base.pages.size());
    for (const auto& p : base.pages) bytes.push_back((int)p.text.size());

    auto ocr_pages = pick_ocr_pages(bytes, mode_, threshold_);
    std::vector<ParseElement> ocr_els;
    if (!ocr_pages.empty()) ocr_els = ocr_.ocr_pages(file_path, ocr_pages);

    ParsedDoc doc = merge_doc(base, ocr_pages, ocr_els);
    normalize_parsed_doc(doc);
    return doc;
}
