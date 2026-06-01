#include "ingest/clause_splitter.h"
#include <regex>
#include <sstream>

// 把行内全角字符归一到半角，仅处理切分所需的三类：
//   全角空格 U+3000(E3 80 80) -> ' '
//   全角数字 U+FF10..FF19(EF BC 90..99) -> '0'..'9'
//   全角句点 U+FF0E(EF BC 8E) -> '.'
// 其余字节原样保留。归一化同时作用于条款号与正文（半角化对检索/展示更友好）。
static std::string normalize_fullwidth(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char b0 = static_cast<unsigned char>(in[i]);
        if (b0 >= 0xE0 && i + 2 < in.size()) {  // 需读 in[i+2]；严格 < 正确（允许三字节序列恰好结尾）
            unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(in[i + 2]);
            if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) { out += ' '; i += 3; continue; }
            if (b0 == 0xEF && b1 == 0xBC) {
                if (b2 >= 0x90 && b2 <= 0x99) { out += static_cast<char>('0' + (b2 - 0x90)); i += 3; continue; }
                if (b2 == 0x8E) { out += '.'; i += 3; continue; }
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no) {
    // 行首条款号：2~4 级（N.N / N.N.N / N.N.N.N），保留 -x 后缀（如 4.2.1-1 / 4.2.1-a）。
    // 单级编号（"4 桥涵设计"）不识别——M1 明确跳过，避免表格/列表误判。
    // M1 临时实现，M2 由层级树正规化替换。
    static const std::regex head(R"(^\s*(\d+(?:\.\d+){1,3}(?:-[0-9a-zA-Z]+)?)\s+(.*)$)");

    std::vector<SplitClause> out;
    std::istringstream iss(page_text);
    std::string raw;
    while (std::getline(iss, raw)) {
        std::string line = normalize_fullwidth(raw);
        // 跳过目录(TOC)条目：形如 "2.1 术语 ........ 2"，含长串点号引导。
        if (line.find("......") != std::string::npos) continue;
        std::smatch m;
        if (std::regex_match(line, m, head)) {
            SplitClause c;
            c.clause_no = m[1].str();
            c.text = m[2].str();
            c.page_start = page_no;
            out.push_back(std::move(c));
        } else if (!out.empty()) {
            // 续行：去掉首尾空白后并入当前条款
            size_t a = line.find_first_not_of(" \t\r");
            if (a != std::string::npos) {
                out.back().text += line.substr(a);
            }
        }
    }
    return out;
}
