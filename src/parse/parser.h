#pragma once
#include <string>
#include <vector>

// 统一中间格式（IR）——poppler（M1）与 MinerU（M2）都产出它。
// M1 仅需文本 + 页码；表格/公式/block 字段为 M2 预留，先留空。
struct ParsedPage {
    int page_no = 0;       // 从 1 开始
    std::string text;      // 该页全文
};

struct ParsedDoc {
    std::string source_path;
    std::string title;            // M1 可为文件名
    std::vector<ParsedPage> pages;
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual ParsedDoc parse(const std::string& file_path) = 0;
};
