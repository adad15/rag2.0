#pragma once
#include "embedding/embedding_client.h"
#include <nlohmann/json.hpp>

// 纯函数（可单测）
std::string build_embedding_request_body(const std::string& model, const std::string& input);
std::vector<float> parse_embedding_response(const std::string& json_body);

class CloudEmbedding : public EmbeddingClient {
public:
    CloudEmbedding(std::string base_url, std::string path, std::string model,
                   std::string api_key, int dim);
    std::vector<float> embed(const std::string& text) override;
    int dim() const override { return dim_; }
private:
    std::string base_url_, path_, model_, api_key_;
    int dim_;
};
