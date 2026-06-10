#pragma once

#include "parse/parser.h"
#include "structure/clause_tree.h"

ClauseTree build_clause_tree(const ParsedDoc& doc, const std::string& standard_id);
