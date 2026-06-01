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
Response post_json(const std::string& base_url, const std::string& path,
                   const std::string& json_body,
                   const std::map<std::string, std::string>& headers);

Response get_json(const std::string& base_url, const std::string& path,
                  const std::map<std::string, std::string>& headers);

}  // namespace http
