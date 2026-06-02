#include "embedding/cloud_embedding.h"
#include "http/http_client.h"
#include <stdexcept>
#include <thread>
#include <chrono>

using nlohmann::json;

std::string build_embedding_request_body(const std::string& model, const std::string& input) {
    json body;
    body["model"] = model;
    body["input"] = input;   // OpenAI 兼容：单条字符串
    return body.dump();
}

std::vector<float> parse_embedding_response(const std::string& json_body) {
    auto j = json::parse(json_body);
    std::vector<float> v;
    for (auto& x : j["data"][0]["embedding"]) v.push_back(x.get<float>());
    return v;
}

CloudEmbedding::CloudEmbedding(std::string base_url, std::string path, std::string model,
                               std::string api_key, int dim)
    : base_url_(std::move(base_url)), path_(std::move(path)), model_(std::move(model)),
      api_key_(std::move(api_key)), dim_(dim) {}

std::vector<float> CloudEmbedding::embed(const std::string& text) {
    auto body = build_embedding_request_body(model_, text);
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
    };
    // embedding 幂等：连接层偶发失败(status==0，如 SSL 断连)或 5xx 时退避重试，避免单次抖动中止整批入库。
    http::Response res;
    const int kMaxAttempts = 4;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        res = http::post_json(base_url_, path_, body, headers);
        if (res.ok()) break;
        bool transient = (res.status == 0 || res.status >= 500);
        if (attempt < kMaxAttempts && transient)
            std::this_thread::sleep_for(std::chrono::milliseconds(400 * attempt));
        else
            break;
    }
    if (!res.ok())
        throw std::runtime_error("embedding api failed: " + res.body + res.error);
    auto v = parse_embedding_response(res.body);
    if ((int)v.size() != dim_)
        throw std::runtime_error("embedding dim mismatch: got " + std::to_string(v.size()));
    return v;
}
