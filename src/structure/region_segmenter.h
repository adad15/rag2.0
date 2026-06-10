#pragma once

#include <vector>
#include "parse/parser.h"

struct RegionGroup {
    Region region = Region::Body;
    std::vector<const ParseElement*> elements;
};

std::vector<RegionGroup> segment_regions(const ParsedDoc& doc);
