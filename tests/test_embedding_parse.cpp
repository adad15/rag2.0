#include <doctest/doctest.h>
#include "embedding/cloud_embedding.h"

TEST_CASE("parse_embedding_response extracts the first vector") {
    std::string resp = R"({
      "data": [ { "embedding": [0.1, 0.2, 0.3], "index": 0 } ],
      "model": "text-embedding-v3"
    })";
    std::vector<float> v = parse_embedding_response(resp);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == doctest::Approx(0.1f));
    CHECK(v[2] == doctest::Approx(0.3f));
}

TEST_CASE("build_embedding_request_body wraps input and model") {
    std::string body = build_embedding_request_body("text-embedding-v3", "压实度限值");
    auto j = nlohmann::json::parse(body);
    CHECK(j["model"] == "text-embedding-v3");
    CHECK(j["input"] == "压实度限值");
}
