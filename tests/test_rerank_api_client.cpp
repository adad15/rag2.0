#include <doctest/doctest.h>
#include "retrieve/rerank_api_client.h"
#include <nlohmann/json.hpp>

TEST_CASE("build_rerank_request_body: fields + omit instruction when empty") {
    auto body = build_rerank_request_body("M", "查询", "指令", {"d0", "d1"}, false);
    auto j = nlohmann::json::parse(body);
    CHECK(j["model"] == "M");
    CHECK(j["query"] == "查询");
    CHECK(j["instruction"] == "指令");
    CHECK(j["documents"].size() == 2);
    CHECK(j["documents"][0] == "d0");
    CHECK(j["return_documents"] == false);
    CHECK(j.contains("top_n") == false);            // 不发 top_n

    auto body2 = build_rerank_request_body("M", "q", "", {"d0"}, false);
    auto j2 = nlohmann::json::parse(body2);
    CHECK(j2.contains("instruction") == false);     // instruction 空则不写
}

TEST_CASE("parse_rerank_response: valid results -> index+score") {
    std::string r = R"({"id":"x","results":[
        {"index":2,"relevance_score":0.9},
        {"index":0,"relevance_score":0.5}]})";
    auto s = parse_rerank_response(r);
    REQUIRE(s.size() == 2);
    CHECK(s[0].index == 2);
    CHECK(s[0].score == doctest::Approx(0.9));
    CHECK(s[1].index == 0);
}

TEST_CASE("parse_rerank_response: invalid / empty rejected (throws)") {
    CHECK_THROWS(parse_rerank_response("not json"));
    CHECK_THROWS(parse_rerank_response(R"({"id":"x"})"));            // 无 results
    CHECK_THROWS(parse_rerank_response(R"({"results":[]})"));        // 空 results
}
