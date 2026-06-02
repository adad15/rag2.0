#pragma once
#include <string>
#include <map>

namespace http {

struct Response {
    int status = 0;            // 0 表示连接失败
    std::string body;
    std::string error;         // 连接层错误描述
    bool ok() const { return status >= 200 && status < 300; }
};

// 最小封装：POST/GET 一个 JSON 字符串。base_url 形如 "https://host:port"。
// read_timeout_sec：读超时秒数（默认 120）；OCR 这类长任务可传大值。
Response post_json(const std::string& base_url, const std::string& path,
                   const std::string& json_body,
                   const std::map<std::string, std::string>& headers,
                   int read_timeout_sec = 120);

Response get_json(const std::string& base_url, const std::string& path,
                  const std::map<std::string, std::string>& headers);

}  // namespace http
