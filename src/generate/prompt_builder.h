#pragma once
#include <string>
#include <vector>
#include "generate/context.h"

// §11.2 约束 -> system prompt
std::string build_system_prompt();
// 问题 + §11.3 上下文片段 -> user prompt
std::string build_user_prompt(const std::string& question,
                              const std::vector<ContextFragment>& fragments);
