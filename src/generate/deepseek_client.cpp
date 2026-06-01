#include "generate/deepseek_client.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace deepseek {

DeepSeekClient::DeepSeekClient(std::string base_url, std::string path,
                               std::string model, std::string api_key)
    : base_url_(std::move(base_url)), path_(std::move(path)),
      model_(std::move(model)), api_key_(std::move(api_key)) {}

std::string DeepSeekClient::chat(const std::string& system, const std::string& user) {
    json body;
    body["model"] = model_;
    body["messages"] = json::array({
        { {"role","system"}, {"content", system} },
        { {"role","user"},   {"content", user} }
    });
    body["temperature"] = 0.0;
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
    };
    auto res = http::post_json(base_url_, path_, body.dump(), headers);
    if (!res.ok())
        throw std::runtime_error("deepseek chat failed: " + res.body + res.error);
    auto j = json::parse(res.body);
    return j["choices"][0]["message"]["content"].get<std::string>();
}

}  // namespace deepseek
