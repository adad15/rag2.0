#include "generate/prompt_builder.h"
#include <nlohmann/json.hpp>

std::string build_system_prompt() {
    return
        "你是规范条文检索助手。严格遵守：\n"
        "1. 只能依据提供的检索上下文回答，不得编造；\n"
        "2. 回答必须标明标准号、标准名称、条款号；\n"
        "3. 涉及强制性条文必须提示；\n"
        "4. 涉及废止/非现行标准必须提示；\n"
        "5. 若上下文为空或不足以支撑，必须明确拒答，提示未检索到依据；\n"
        "6. 不得编造条款号、限值、单位。\n"
        "引用格式示例：依据《标准名称》（标准号）第 X.X.X 条……";
}

std::string build_user_prompt(const std::string& question,
                              const std::vector<ContextFragment>& fragments) {
    nlohmann::json ctx = nlohmann::json::array();
    for (auto& f : fragments) ctx.push_back(to_json(f));
    std::string s = "【用户问题】\n" + question + "\n\n【检索上下文（JSON 数组）】\n";
    s += ctx.dump(2);
    s += "\n\n请依据上述上下文作答，并按要求标注来源；若不足以作答请拒答。";
    return s;
}
