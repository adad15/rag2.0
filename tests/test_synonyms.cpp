#include <doctest/doctest.h>
#include "query/synonyms.h"

TEST_CASE("SynonymDict expand appends synonyms of matched terms, keeping the original") {
    SynonymDict d;
    d.load_from_lines({"针入度,贯入度", "精度,精密度,重复性"});
    std::string out = d.expand("沥青针入度试验精度");
    CHECK(out.find("针入度") != std::string::npos);
    CHECK(out.find("贯入度") != std::string::npos);
    CHECK(out.find("精密度") != std::string::npos);
    CHECK(out.find("重复性") != std::string::npos);
}

TEST_CASE("SynonymDict expand is a no-op when nothing matches") {
    SynonymDict d;
    d.load_from_lines({"针入度,贯入度"});
    CHECK(d.expand("水泥取样方法") == "水泥取样方法");
}

TEST_CASE("SynonymDict expand does not duplicate a synonym already present") {
    SynonymDict d;
    d.load_from_lines({"精度,精密度"});
    std::string out = d.expand("精度和精密度");
    size_t first = out.find("精密度");
    REQUIRE(first != std::string::npos);
    CHECK(out.find("精密度", first + 1) == std::string::npos);
}

TEST_CASE("SynonymDict skips blank and comment lines") {
    SynonymDict d;
    d.load_from_lines({"# 注释", "", "针入度,贯入度"});
    CHECK(d.expand("针入度").find("贯入度") != std::string::npos);
}
