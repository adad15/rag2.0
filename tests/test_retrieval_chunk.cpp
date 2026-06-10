#include <doctest/doctest.h>
#include "retrieve/retrieval_chunk.h"

TEST_CASE("retrieval_chunk JSON round trip preserves cache and chunk fields") {
    RetrievalChunkCache cache;
    cache.standard_id = "sid";
    cache.standard_no = "JTC 5210-2018";

    RetrievalChunk c;
    c.chunk_id = "sid:5/5.1/5.1.2#main";
    c.node_id = "sid:5/5.1/5.1.2";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5.1.2";
    c.method_no = "";
    c.title = "路基沉降";
    c.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    c.atomic_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.embedding_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。\n相关图表题：\n图5.1.2 路基沉降示意图";
    c.context_text = "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.captions = {"图5.1.2 路基沉降示意图"};
    c.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    c.page_start = 12;
    c.page_end = 13;
    c.has_table = true;
    c.has_formula = true;
    c.has_figure = true;
    c.suspect = "seq";
    cache.chunks.push_back(c);

    RetrievalChunkCache round_trip =
        retrieval_chunk_cache_from_json(retrieval_chunk_cache_to_json(cache));

    CHECK(round_trip.schema_version == 1);
    CHECK(round_trip.standard_id == "sid");
    CHECK(round_trip.standard_no == "JTC 5210-2018");
    REQUIRE(round_trip.chunks.size() == 1);

    const RetrievalChunk& r = round_trip.chunks[0];
    CHECK(r.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(r.node_id == "sid:5/5.1/5.1.2");
    CHECK(r.standard_id == "sid");
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.chunk_type == "body");
    CHECK(r.clause_no == "5.1.2");
    CHECK(r.method_no == "");
    CHECK(r.title == "路基沉降");
    CHECK(r.path_text.find("5.1 路基") != std::string::npos);
    CHECK(r.atomic_text.find("路基沉降") != std::string::npos);
    CHECK(r.embedding_text.find("图5.1.2") != std::string::npos);
    CHECK(r.context_text.find("路径：") != std::string::npos);
    REQUIRE(r.captions.size() == 1);
    CHECK(r.captions[0] == "图5.1.2 路基沉降示意图");
    REQUIRE(r.formulas.size() == 1);
    CHECK(r.formulas[0] == "MQI = SCI + PQI + BCI + TCI");
    CHECK(r.page_start == 12);
    CHECK(r.page_end == 13);
    CHECK(r.has_table);
    CHECK(r.has_formula);
    CHECK(r.has_figure);
    CHECK(r.suspect == "seq");
}
