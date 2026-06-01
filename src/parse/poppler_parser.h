#pragma once
#include "parse/parser.h"

class PopplerParser : public Parser {
public:
    ParsedDoc parse(const std::string& file_path) override;
};
