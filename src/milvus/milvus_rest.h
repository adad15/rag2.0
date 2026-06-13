#pragma once
#include <string>
#include <vector>

namespace milvus {

struct Hit {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    float score = 0.0f;
};

// 纯函数：构造 REST v2 请求体（可单测）
std::string build_insert_body(const std::string& collection,
                              const std::string& chunk_id,
                              const std::string& node_id,
                              const std::string& standard_id,
                              const std::vector<float>& dense);
std::string build_delete_body(const std::string& collection,
                              const std::string& standard_id);
std::string build_search_body(const std::string& collection,
                              const std::vector<float>& query,
                              int top_k,
                              const std::vector<std::string>& output_fields,
                              const std::string& filter_expr = "");

class MilvusRest {
public:
    MilvusRest(std::string base_url, std::string token);
    bool ping();                                  // POST /v2/vectordb/collections/list
    // 若集合不存在则创建（dense 维度 = dim）
    void ensure_collection(const std::string& collection, int dim);
    void insert(const std::string& collection, const std::string& chunk_id,
                const std::string& node_id, const std::string& standard_id,
                const std::vector<float>& dense);
    // 按 standard_id 删除该标准的全部向量（先删后插幂等的 Milvus 侧）
    void delete_by_standard(const std::string& collection, const std::string& standard_id);
    // 一次性升级/测试清理用
    void drop_collection(const std::string& collection);
    std::vector<Hit> search(const std::string& collection,
                            const std::vector<float>& query, int top_k,
                            const std::string& filter_expr = "");
private:
    std::string base_url_;
    std::string token_;
};

}  // namespace milvus
