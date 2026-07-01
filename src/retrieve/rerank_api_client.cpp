#include "retrieve/rerank_api_client.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>

using nlohmann::json;

std::string build_rerank_request_body(const std::string& model, const std::string& query,
                                      const std::string& instruction,
                                      const std::vector<std::string>& documents,
                                      bool return_documents) {
    json body;
    body["model"] = model;
    body["query"] = query;
    if (!instruction.empty()) body["instruction"] = instruction;
    body["documents"] = documents;
    body["return_documents"] = return_documents;
    // 有意不设 top_n：让模型给整池打分并全返回，漏排交本地补尾。
    return body.dump();
}

std::vector<RerankScore> parse_rerank_response(const std::string& json_body) {
    json j = json::parse(json_body, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("results") || !j["results"].is_array() || j["results"].empty())
        throw std::runtime_error("rerank response invalid: missing/empty results");
    std::vector<RerankScore> out;
    for (const auto& r : j["results"]) {
        if (!r.contains("index") || !r.contains("relevance_score")) continue;
        RerankScore s;
        s.index = r["index"].get<int>();
        s.score = r["relevance_score"].get<double>();
        out.push_back(s);
    }
    if (out.empty()) throw std::runtime_error("rerank response invalid: no scored result");
    return out;
}

std::vector<RerankScore> RerankApiClient::score(const std::string& query,
                                                const std::vector<std::string>& docs) const {
    std::string body = build_rerank_request_body(model_, query, instruction_, docs, false);
    std::map<std::string, std::string> headers = {{"Authorization", "Bearer " + api_key_}};
    auto res = http::post_json(base_url_, path_, body, headers, timeout_sec_);
    if (!res.ok())
        throw std::runtime_error("rerank api failed: status=" + std::to_string(res.status) + " " + res.error);
    return parse_rerank_response(res.body);   // 解析失败抛，调用方兜底
}
