#pragma once
#include <string>
#include <vector>
#include "generate/context.h"

// 答案缓存键的一部分；改 build_system_prompt 必须 bump 它使旧缓存失效。
extern const char* kAnswerPromptVersion;

// §11.2 约束 -> system prompt
std::string build_system_prompt();
// 问题 + §11.3 上下文片段 -> user prompt
std::string build_user_prompt(const std::string& question,
                              const std::vector<ContextFragment>& fragments);
