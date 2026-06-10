#pragma once

#include "parse/parser.h"
#include "structure/format_profile.h"

struct TocResult {
    bool detected = false;
    FormatProfile profile = FormatProfile::A_decimal;
};

TocResult detect_format_from_toc(const ParsedDoc& doc);
FormatProfile detect_format_from_body(const ParsedDoc& doc);
