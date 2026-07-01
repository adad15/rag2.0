#pragma once
#include <string>
#include <vector>

// 单个候选的模型打分。index = 请求 documents 数组下标。
struct RerankScore {
    int index = 0;
    double score = 0.0;
};

// 纯函数：构造 /v1/rerank 请求体。instruction 为空则不写该字段；不发 top_n。
std::string build_rerank_request_body(const std::string& model, const std::string& query,
                                      const std::string& instruction,
                                      const std::vector<std::string>& documents,
                                      bool return_documents);

// 纯函数：解析响应。要求 results 为非空数组、每项含 index 与 relevance_score。
// 结构不可用（非 JSON / 无 results / results 空）时抛 std::runtime_error。
std::vector<RerankScore> parse_rerank_response(const std::string& json_body);

// 网络客户端：复用 http::post_json。score() 失败（HTTP 非 2xx / 解析失败）抛异常，由调用方兜底。
class RerankApiClient {
public:
    RerankApiClient(std::string base_url, std::string path, std::string model,
                    std::string api_key, int timeout_sec, std::string instruction)
        : base_url_(std::move(base_url)), path_(std::move(path)), model_(std::move(model)),
          api_key_(std::move(api_key)), timeout_sec_(timeout_sec),
          instruction_(std::move(instruction)) {}
    std::vector<RerankScore> score(const std::string& query,
                                   const std::vector<std::string>& docs) const;
private:
    std::string base_url_, path_, model_, api_key_;
    int timeout_sec_;
    std::string instruction_;
};
