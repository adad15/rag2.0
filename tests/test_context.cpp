#include <doctest/doctest.h>
#include "generate/context.h"
#include "generate/answer_pipeline.h"
#include <nlohmann/json.hpp>

TEST_CASE("ContextFragment serializes to the §11.3 schema") {
    ContextFragment f;
    f.source_id = "S1";
    f.standard_no = "JTG D60-2015";
    f.standard_name = "公路桥涵设计通用规范";
    f.status = "现行";
    f.clause_no = "4.2.1";
    f.path = "第4章 / 4.2 xxxx / 4.2.1";
    f.is_mandatory = true;
    f.text = "条款原文……";
    auto j = to_json(f);
    CHECK(j["source_id"] == "S1");
    CHECK(j["standard_no"] == "JTG D60-2015");
    CHECK(j["status"] == "现行");
    CHECK(j["clause_no"] == "4.2.1");
    CHECK(j["is_mandatory"] == true);
    CHECK(j.contains("tables"));
    CHECK(j.contains("formulas"));
}

TEST_CASE("fragment_from_chunk maps chunk row to llm context fragment") {
    RetrievalChunkRow chunk;
    chunk.chunk_id = "sid:5/5.1/5.1.2#main";
    chunk.standard_id = "sid";
    chunk.clause_no = "5.1.2";
    chunk.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    chunk.atomic_text = "5.1.2 路基沉降\n正文";
    chunk.context_text = "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n\n5.1.2 路基沉降\n正文";

    StandardRow s;
    s.standard_no = "JTC 5210-2018";
    s.standard_name = "公路技术状况评定标准";
    s.status = "现行";

    ContextFragment f = fragment_from_chunk(3, chunk, s);
    CHECK(f.source_id == "S3");
    CHECK(f.standard_no == "JTC 5210-2018");
    CHECK(f.standard_name == "公路技术状况评定标准");
    CHECK(f.status == "现行");
    CHECK(f.clause_no == "5.1.2");
    CHECK(f.path == chunk.path_text);
    CHECK(f.text == chunk.context_text);   // LLM 上下文必须是 small-to-big 的 context_text
    CHECK_FALSE(f.is_mandatory);
}

TEST_CASE("fragment_from_chunk tolerates missing standard row") {
    RetrievalChunkRow chunk;
    chunk.clause_no = "2";
    chunk.context_text = "正文";
    ContextFragment f = fragment_from_chunk(1, chunk, std::nullopt);
    CHECK(f.source_id == "S1");
    CHECK(f.standard_no == "");
    CHECK(f.standard_name == "");
    CHECK(f.status == "");
    CHECK(f.text == "正文");
}
