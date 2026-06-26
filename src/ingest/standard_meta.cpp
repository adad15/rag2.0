#include "ingest/standard_meta.h"
#include <regex>

namespace {
// 软化封面 OCR 标点，使抽取正则能命中：下划线/全角空格→空格、各破折号→'-'、
// 全角斜杠→'/'、全角数字→半角。保留 ASCII 空格/斜杠/大小写，产出干净代号。
std::string soften_code_punct(const std::string& s) {
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += (c == '_') ? ' ' : static_cast<char>(c);
            ++i;
            continue;
        }
        if (i + 2 < n) {
            unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if (c == 0xE3 && b1 == 0x80 && b2 == 0x80) { out += ' '; i += 3; continue; }   // 　全角空格
            if (c == 0xE2 && b1 == 0x80 && (b2 == 0x90 || b2 == 0x93 || b2 == 0x94 || b2 == 0x95)) { out += '-'; i += 3; continue; }  // ‐ – — ―
            if (c == 0xE2 && b1 == 0x88 && b2 == 0x92) { out += '-'; i += 3; continue; }   // − 减号
            if (c == 0xEF && b1 == 0xBC) {
                if (b2 == 0x8F) { out += '/'; i += 3; continue; }                          // ／全角斜杠
                if (b2 == 0x8D) { out += '-'; i += 3; continue; }                          // －全角连字符
                if (b2 >= 0x90 && b2 <= 0x99) { out += static_cast<char>('0' + (b2 - 0x90)); i += 3; continue; }  // 全角数字
            }
        }
        size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        for (size_t k = 0; k < len && i < n; ++k) out += s[i++];
    }
    return out;
}
}  // namespace

std::string extract_standard_no(const std::string& page_text, const std::string& fallback) {
    // 前缀大写字母(>=2) + 可选 /字母 + 空白 + 可选字母段 + 数字(可带小数) + - + 四位年份(19xx/20xx)
    // 覆盖 JTG D60-2015 / GB 50010-2010 / GB/T 50081-2019 / JGJ 3-2010 / TB 10002-2017。
    // 已知不覆盖：双连字符编号如 JTG/T B05-01-2013（年份前还有连字符段），留给 M2 修复。
    static const std::regex pat(
        R"([A-Z]{2,}(?:/[A-Z]+)?[ \t]*[A-Z]{0,2}\d+(?:\.\d+)?-(?:19|20)\d{2})");
    std::string text = soften_code_punct(page_text);   // 先软化 OCR 标点(下划线/全角破折号等)
    std::smatch m;
    if (std::regex_search(text, m, pat)) return m[0].str();
    return fallback;
}

std::string normalize_standard_code(const std::string& s) {
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {                                   // ASCII
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '_' || c == '/') { ++i; continue; }
            out += static_cast<char>(std::tolower(c));
            ++i;
            continue;
        }
        if (i + 2 < n) {                                  // 三字节特殊符号
            unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if (c == 0xE3 && b1 == 0x80 && b2 == 0x80) { i += 3; continue; }                 // 　全角空格→删
            if (c == 0xE2 && b1 == 0x80 && (b2 == 0x90 || b2 == 0x93 || b2 == 0x94 || b2 == 0x95)) { out += '-'; i += 3; continue; }  // ‐ – — ―
            if (c == 0xE2 && b1 == 0x88 && b2 == 0x92) { out += '-'; i += 3; continue; }     // − 减号
            if (c == 0xEF && b1 == 0xBC) {
                if (b2 == 0x8F) { i += 3; continue; }                                        // ／全角斜杠→删
                if (b2 == 0x8D) { out += '-'; i += 3; continue; }                            // －全角连字符
                if (b2 >= 0x90 && b2 <= 0x99) { out += static_cast<char>('0' + (b2 - 0x90)); i += 3; continue; }  // 全角数字
            }
        }
        size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;   // 其余 UTF-8 字符原样保留
        for (size_t k = 0; k < len && i < n; ++k) out += s[i++];
    }
    return out;
}
