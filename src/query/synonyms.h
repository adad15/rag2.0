#pragma once
#include <string>
#include <vector>
#include <map>

// 领域同义词词典。每行一组互为同义的词（逗号分隔，# 开头为注释）。
// 仅用于查询侧 BM25 路扩展（纯逻辑，可单测）。
class SynonymDict {
public:
    void load_from_lines(const std::vector<std::string>& lines);
    void load_from_file(const std::string& path);   // 文件缺失则保持空词典
    // 查询中命中某词则追加其同义词（空格分隔），原词与已含词不重复追加。
    std::string expand(const std::string& query) const;
private:
    std::map<std::string, std::vector<std::string>> alias_;  // 词 -> 同组其他词
};
