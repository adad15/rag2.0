#include <doctest/doctest.h>
#include "milvus/milvus_rest.h"
#include <nlohmann/json.hpp>

TEST_CASE("build_search_body produces valid Milvus REST v2 search payload") {
    std::vector<float> vec = {0.1f, 0.2f, 0.3f};
    std::string body = milvus::build_search_body("clause_text", vec, 5, {"node_id", "standard_id"});
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["limit"] == 5);
    CHECK(j["data"][0].size() == 3);
    CHECK(j["data"][0][0] == doctest::Approx(0.1f));
    CHECK(j["outputFields"][0] == "node_id");
    CHECK(j["annsField"] == "dense");
}

TEST_CASE("build_insert_body wraps one row with chunk scalars and dense vector") {
    std::vector<float> vec = {1.0f, 2.0f};
    std::string body = milvus::build_insert_body("clause_text", "n1#main", "n1", "s1", vec);
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["data"][0]["chunk_id"] == "n1#main");
    CHECK(j["data"][0]["node_id"] == "n1");
    CHECK(j["data"][0]["standard_id"] == "s1");
    CHECK(j["data"][0]["dense"].size() == 2);
}

TEST_CASE("build_delete_body filters by standard id") {
    std::string body = milvus::build_delete_body("clause_text", "12215131224082667446");
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["filter"] == "standard_id == \"12215131224082667446\"");
}

TEST_CASE("build_search_body includes filter when provided") {
    std::vector<float> vec = {0.1f, 0.2f};
    std::string body = milvus::build_search_body(
        "clause_text", vec, 5, {"chunk_id", "node_id", "standard_id"},
        "standard_id == \"S1\"");
    auto j = nlohmann::json::parse(body);
    CHECK(j["filter"] == "standard_id == \"S1\"");
}

TEST_CASE("build_search_body omits filter key when expression is empty") {
    std::vector<float> vec = {0.1f, 0.2f};
    std::string body = milvus::build_search_body(
        "clause_text", vec, 5, {"chunk_id"}, "");
    auto j = nlohmann::json::parse(body);
    CHECK_FALSE(j.contains("filter"));
}

TEST_CASE("build_insert_full_body carries status and text, not sparse") {
    std::vector<float> vec = {1.0f, 2.0f};
    std::string body = milvus::build_insert_full_body(
        "clause_text", "c1#main", "c1", "s1", "现行", "正文文本", vec);
    auto j = nlohmann::json::parse(body);
    auto row = j["data"][0];
    CHECK(row["chunk_id"] == "c1#main");
    CHECK(row["node_id"] == "c1");
    CHECK(row["standard_id"] == "s1");
    CHECK(row["status"] == "现行");
    CHECK(row["text"] == "正文文本");
    CHECK(row["dense"].size() == 2);
    CHECK_FALSE(row.contains("sparse"));
}

TEST_CASE("build_bm25_body searches the sparse field with raw query text") {
    std::string body = milvus::build_bm25_body(
        "clause_text", "针入度 精度", 5, {"chunk_id", "node_id", "standard_id"},
        "status == \"现行\"");
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["annsField"] == "sparse");
    CHECK(j["data"][0] == "针入度 精度");
    CHECK(j["limit"] == 5);
    CHECK(j["filter"] == "status == \"现行\"");
    CHECK(j["outputFields"][0] == "chunk_id");
}

TEST_CASE("build_text_collection_body declares text analyzer, sparse, and BM25 function") {
    std::string body = milvus::build_text_collection_body("clause_text", 4096,
                                                          {"针入度", "压实度"});
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    auto fields = j["schema"]["fields"];
    bool has_status = false, has_text = false, has_sparse = false, has_dense = false;
    for (auto& fdef : fields) {
        std::string name = fdef["fieldName"];
        if (name == "status") has_status = true;
        if (name == "text") has_text = true;
        if (name == "sparse") has_sparse = true;
        if (name == "dense") has_dense = true;
    }
    CHECK(has_status);
    CHECK(has_text);
    CHECK(has_sparse);
    CHECK(has_dense);
    REQUIRE(j["schema"].contains("functions"));
    auto fn = j["schema"]["functions"][0];
    CHECK(fn["type"] == "BM25");
    CHECK(fn["inputFieldNames"][0] == "text");
    CHECK(fn["outputFieldNames"][0] == "sparse");
}
