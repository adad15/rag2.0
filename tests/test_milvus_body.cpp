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
