#include "parse/parser_factory.h"
#include "parse/ppstructure_backend.h"
#include <stdexcept>

ParseMode parse_mode_from_string(const std::string& s) {
    if (s == "poppler") return ParseMode::Poppler;
    if (s == "ocr")     return ParseMode::Ocr;
    return ParseMode::Auto;
}

std::unique_ptr<OcrBackend> make_ocr_backend(const std::string& engine,
                                             const std::string& base_url) {
    if (engine == "ppstructure")
        return std::make_unique<PpStructureBackend>(base_url);
    throw std::runtime_error("OCR 引擎 '" + engine +
        "' 尚未实现（本轮仅支持 ppstructure；mineru/vlapi/tesseract 为预留）");
}
