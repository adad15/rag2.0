#pragma once
#include <string>
#include <vector>

// 契约②：文本 -> 稠密向量。M1 用云 API；M5 换 Qwen3-Embedding-8B 只换实现。
class EmbeddingClient {
public:
    virtual ~EmbeddingClient() = default;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual int dim() const = 0;
};
