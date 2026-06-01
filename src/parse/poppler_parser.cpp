#include "parse/poppler_parser.h"
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <memory>
#include <stdexcept>
#include <filesystem>

ParsedDoc PopplerParser::parse(const std::string& file_path) {
    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_file(file_path));
    if (!doc)
        throw std::runtime_error("poppler: cannot open " + file_path);

    ParsedDoc out;
    out.source_path = file_path;
    out.title = std::filesystem::path(file_path).filename().string();

    int n = doc->pages();
    for (int i = 0; i < n; ++i) {
        std::unique_ptr<poppler::page> pg(doc->create_page(i));
        ParsedPage p;
        p.page_no = i + 1;
        if (pg) {
            poppler::byte_array ba = pg->text().to_utf8();
            p.text.assign(ba.begin(), ba.end());
        }
        out.pages.push_back(std::move(p));
    }
    return out;
}
