#pragma once
#include <string>

// UTF-8 安全的文本截断：按可见字符数（一个汉字算 1）截断，绝不从多字节
// 字符中间切断；发生截断时追加省略号 "\xE2\x80\xA6"(U+2026)。纯函数，可单测。
// 仿 path_utf8.h：UTF-8 续字节恒为 0x80~0xBF，据此识别字符边界。
namespace text_utf8 {

inline std::string truncate(const std::string& s, size_t max_chars) {
    size_t i = 0;        // 字节游标
    size_t chars = 0;    // 已计字符数
    while (i < s.size() && chars < max_chars) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = (b < 0x80) ? 1 : ((b >> 5) == 0x6) ? 2 : ((b >> 4) == 0xE) ? 3 : 4;
        i += step;
        ++chars;
    }
    if (i >= s.size()) return s;          // 未截断
    return s.substr(0, i) + "\xE2\x80\xA6";  // U+2026 省略号
}

// 可见字符数（一个汉字算 1）。纯函数。
inline size_t char_count(const std::string& s) {
    size_t i = 0, chars = 0;
    while (i < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = (b < 0x80) ? 1 : ((b >> 5) == 0x6) ? 2 : ((b >> 4) == 0xE) ? 3 : 4;
        i += step;
        ++chars;
    }
    return chars;
}

}  // namespace text_utf8
