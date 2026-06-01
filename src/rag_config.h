#pragma once
#include <string>
#include <map>
#include <vector>
#include <algorithm>

struct Config {
    std::string pg_conninfo;
    std::string milvus_base_url;
    std::string milvus_token;
    std::string milvus_collection;
    std::string embed_base_url;
    std::string embed_path;
    std::string embed_model;
    int         embed_dim = 1024;
    std::string embed_key;
    std::string deepseek_base_url;
    std::string deepseek_path;
    std::string deepseek_model;
    std::string deepseek_key;
    std::string doc_path;

    // 从任意 key->value map 构建（测试友好）
    static Config from_map(const std::map<std::string, std::string>& env);
    // 从真实进程环境变量构建
    static Config from_env();
    // 返回缺失的必填 key 名列表
    std::vector<std::string> missing_required() const;
};
