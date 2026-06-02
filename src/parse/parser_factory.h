#pragma once
#include <memory>
#include <string>
#include "parse/hybrid_parser.h"
#include "parse/ocr_backend.h"

ParseMode parse_mode_from_string(const std::string& s);

// 按引擎名造 OCR 后端：本轮仅 "ppstructure"；其余抛 std::runtime_error。
std::unique_ptr<OcrBackend> make_ocr_backend(const std::string& engine,
                                             const std::string& base_url);
