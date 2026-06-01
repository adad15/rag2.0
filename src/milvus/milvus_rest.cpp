#include "milvus/milvus_rest.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

namespace milvus {

std::string build_insert_body(const std::string& collection, const std::string& node_id,
                              const std::string& standard_id, const std::vector<float>& dense) {
    json row;
    row["node_id"] = node_id;
    row["standard_id"] = standard_id;
    row["dense"] = dense;
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({row});
    return body.dump();
}

std::string build_search_body(const std::string& collection, const std::vector<float>& query,
                              int top_k, const std::vector<std::string>& output_fields) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query});
    body["annsField"] = "dense";
    body["limit"] = top_k;
    body["outputFields"] = output_fields;
    return body.dump();
}

MilvusRest::MilvusRest(std::string base_url, std::string token)
    : base_url_(std::move(base_url)), token_(std::move(token)) {}

static std::map<std::string, std::string> auth_headers(const std::string& token) {
    return { {"Authorization", "Bearer " + token} };
}

bool MilvusRest::ping() {
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/list", "{}",
                               auth_headers(token_));
    return res.ok();
}

void MilvusRest::ensure_collection(const std::string& collection, int dim) {
    // 已存在则直接返回
    {
        json q; q["collectionName"] = collection;
        auto has = http::post_json(base_url_, "/v2/vectordb/collections/has", q.dump(),
                                   auth_headers(token_));
        if (has.ok()) {
            auto j = json::parse(has.body, nullptr, false);
            if (!j.is_discarded() && j.contains("data") &&
                j["data"].contains("has") && j["data"]["has"].get<bool>())
                return;
        }
    }
    // 快速建集合 + 自定义 schema（主键 node_id varchar，dense 向量）
    json schema;
    schema["autoID"] = false;
    schema["fields"] = json::array({
        { {"fieldName","node_id"}, {"dataType","VarChar"}, {"isPrimary",true},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","standard_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 128} }} },
        { {"fieldName","dense"}, {"dataType","FloatVector"},
          {"elementTypeParams", { {"dim", dim} }} }
    });
    json index = json::array({
        { {"fieldName","dense"}, {"indexName","dense_idx"}, {"metricType","COSINE"} }
    });
    json body;
    body["collectionName"] = collection;
    body["schema"] = schema;
    body["indexParams"] = index;
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/create", body.dump(),
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus create collection failed: " + res.body + res.error);
}

void MilvusRest::insert(const std::string& collection, const std::string& node_id,
                        const std::string& standard_id, const std::vector<float>& dense) {
    auto body = build_insert_body(collection, node_id, standard_id, dense);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/insert", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus insert failed: " + res.body + res.error);
}

std::vector<Hit> MilvusRest::search(const std::string& collection,
                                    const std::vector<float>& query, int top_k) {
    auto body = build_search_body(collection, query, top_k, {"node_id", "standard_id"});
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus search failed: " + res.body + res.error);
    auto j = json::parse(res.body);
    std::vector<Hit> hits;
    for (auto& item : j["data"]) {
        Hit h;
        h.node_id = item.value("node_id", "");
        h.standard_id = item.value("standard_id", "");
        h.score = item.value("distance", 0.0f);
        hits.push_back(h);
    }
    return hits;
}

}  // namespace milvus
