#pragma once
#include <string>

namespace deepseek {
struct Message { std::string role; std::string content; };

class DeepSeekClient {
public:
    DeepSeekClient(std::string base_url, std::string path,
                   std::string model, std::string api_key);
    // 发送 system+user 两条消息，返回回答文本；失败抛异常
    std::string chat(const std::string& system, const std::string& user);
private:
    std::string base_url_, path_, model_, api_key_;
};
}
