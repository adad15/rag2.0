#include <doctest/doctest.h>
#include "ingest/chunk_loader.h"

#include <nlohmann/json.hpp>

TEST_CASE("chunk_to_row maps all fields and serializes captions and formulas to json") {
    RetrievalChunk c;
    c.chunk_id = "sid:5/5.1/5.1.2#main";
    c.node_id = "sid:5/5.1/5.1.2";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5.1.2";
    c.method_no = "T0302-2024";
    c.title = "路基沉降";
    c.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    c.atomic_text = "5.1.2 路基沉降\n正文";
    c.embedding_text = "5.1.2 路基沉降\n正文\n图题";
    c.context_text = "路径：…\n正文";
    c.captions = {"图5.1.2 路基沉降示意图"};
    c.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    c.page_start = 12;
    c.page_end = 13;
    c.has_table = true;
    c.has_formula = true;
    c.has_figure = true;
    c.suspect = "seq";

    RetrievalChunkRow row = chunk_to_row(c);

    CHECK(row.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(row.node_id == "sid:5/5.1/5.1.2");
    CHECK(row.standard_id == "sid");
    CHECK(row.chunk_type == "body");
    CHECK(row.clause_no == "5.1.2");
    CHECK(row.method_no == "T0302-2024");
    CHECK(row.title == "路基沉降");
    CHECK(row.path_text == c.path_text);
    CHECK(row.atomic_text == c.atomic_text);
    CHECK(row.embedding_text == c.embedding_text);
    CHECK(row.context_text == c.context_text);
    CHECK(row.page_start == 12);
    CHECK(row.page_end == 13);
    CHECK(row.has_table);
    CHECK(row.has_formula);
    CHECK(row.has_figure);
    CHECK(row.suspect == "seq");

    auto caps = nlohmann::json::parse(row.captions_json);
    REQUIRE(caps.is_array());
    REQUIRE(caps.size() == 1);
    CHECK(caps[0] == "图5.1.2 路基沉降示意图");

    auto forms = nlohmann::json::parse(row.formulas_json);
    REQUIRE(forms.is_array());
    REQUIRE(forms.size() == 1);
    CHECK(forms[0] == "MQI = SCI + PQI + BCI + TCI");
}

TEST_CASE("chunk_to_row serializes empty captions and formulas as empty json arrays") {
    RetrievalChunk c;
    c.chunk_id = "sid:1#main";
    RetrievalChunkRow row = chunk_to_row(c);
    CHECK(row.chunk_id == "sid:1#main");
    CHECK(row.captions_json == "[]");
    CHECK(row.formulas_json == "[]");
}
