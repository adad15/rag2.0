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

    // M2a 解析层
    std::string parse_mode;          // auto | poppler | ocr
    std::string ocr_engine;          // ppstructure（预留 mineru/vlapi/tesseract）
    std::string ppstruct_base_url;
    int         scan_chars_threshold = 100;  // 每页字节数低于此判为扫描页

    // 从任意 key->value map 构建（测试友好）
    static Config from_map(const std::map<std::string, std::string>& env);
    // 从 JSON 文本构建（测试友好）
    static Config from_json_string(const std::string& json_text);
    // 从 JSON 配置文件构建（如 config.json）；文件缺失/解析失败时返回空配置
    static Config from_json_file(const std::string& path);
    // 返回缺失的必填 key 名列表
    std::vector<std::string> missing_required() const;
};
