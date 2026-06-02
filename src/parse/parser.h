#pragma once
#include <string>
#include <vector>

// 归一化元素类型：poppler（降级，仅 Text）与 OCR 后端（完整）都产出。
enum class ElementType { Heading, Text, Table, Formula, Figure };

inline std::string element_type_to_string(ElementType t) {
    switch (t) {
        case ElementType::Heading: return "Heading";
        case ElementType::Table:   return "Table";
        case ElementType::Formula: return "Formula";
        case ElementType::Figure:  return "Figure";
        default:                   return "Text";
    }
}
inline ElementType element_type_from_string(const std::string& s) {
    if (s == "Heading") return ElementType::Heading;
    if (s == "Table")   return ElementType::Table;
    if (s == "Formula") return ElementType::Formula;
    if (s == "Figure")  return ElementType::Figure;
    return ElementType::Text;
}

// 归一化元素：按阅读顺序排列。poppler 页产出 Text；OCR 页产出完整结构。
struct ParseElement {
    ElementType type = ElementType::Text;
    int page_no = 0;             // 从 1 开始
    int level = 0;               // Heading 层级，非标题为 0
    std::string clause_no;       // 元素自带条款号（可空）
    std::string title;           // 标题文本（Heading）
    std::string text;            // 正文 / OCR 文本
    std::string table_html;      // 表格 HTML（Table 类型）
    std::string caption;         // 表/图题
    std::string source;          // "poppler" | "ppstructure" | ...
    float ocr_confidence = 1.0f;
};

// 统一中间格式（IR）。M1 仅用 pages；M2a 加 elements（富 IR）与 standard_no。
struct ParsedPage {
    int page_no = 0;       // 从 1 开始
    std::string text;      // 该页全文
};

struct ParsedDoc {
    std::string source_path;
    std::string title;                       // 可为文件名
    std::string standard_no;                 // 可空，由 extract_standard_no 回填
    std::vector<ParsedPage> pages;           // 每页全文
    std::vector<ParseElement> elements;      // 归一化元素流（富 IR，本轮仅缓存）
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual ParsedDoc parse(const std::string& file_path) = 0;
};
