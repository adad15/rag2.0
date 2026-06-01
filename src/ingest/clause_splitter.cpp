#include "ingest/clause_splitter.h"
#include <regex>
#include <sstream>

std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no) {
    // 行首条款号：数字段 + 至少两段 .数字（覆盖 1.0.1 / 4.2.1 / 4.2.1-1 / 4.2.1-a）
    static const std::regex head(R"(^\s*(\d+\.\d+(?:\.\d+)?(?:-[0-9a-zA-Z]+)?)\s+(.*)$)");

    std::vector<SplitClause> out;
    std::istringstream iss(page_text);
    std::string line;
    while (std::getline(iss, line)) {
        std::smatch m;
        if (std::regex_match(line, m, head)) {
            SplitClause c;
            c.clause_no = m[1].str();
            c.text = m[2].str();
            c.page_start = page_no;
            out.push_back(std::move(c));
        } else if (!out.empty()) {
            // 续行：去掉首尾空白后并入当前条款
            std::string trimmed = line;
            size_t a = trimmed.find_first_not_of(" \t\r");
            if (a != std::string::npos) {
                out.back().text += trimmed.substr(a);
            }
        }
    }
    return out;
}
