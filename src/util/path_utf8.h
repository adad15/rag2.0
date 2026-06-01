#pragma once
#include <string>

// UTF-8 安全的路径处理：仅按 ASCII 分隔符 '/' '\\' '.' 切字节，不经过
// std::filesystem 的 ANSI 代码页(ACP)转换。后者在 Windows 上遇到中文路径会抛
// "No mapping for the Unicode character exists in the target multi-byte code page"。
// 分隔符均为 ASCII，且 UTF-8 续字节恒为 0x80~0xBF，不会与之冲突，故按字节切安全。
namespace path_utf8 {

// 取文件名（含扩展名）
inline std::string filename(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// 取主名（去掉最后一个扩展名）；首字符即点（如 ".env"）不视为扩展名
inline std::string stem(const std::string& p) {
    std::string fn = filename(p);
    size_t dot = fn.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? fn : fn.substr(0, dot);
}

}  // namespace path_utf8
