#pragma once

#include <map>
#include <string>
#include <vector>

struct TreeNode {
    std::string node_id;
    int level = 0;
    std::string number;
    std::string title;
    std::string text;
    int page_start = 0;
    int page_end = 0;
    std::string parent_id;
    std::vector<std::string> child_ids;
    bool is_leaf = false;
    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::vector<std::string> captions;
    std::vector<std::string> table_htmls;
    std::vector<std::string> formulas;
    std::string suspect;
};

struct ClauseTree {
    std::string standard_id;
    std::string standard_no;
    std::vector<TreeNode> nodes;
    std::map<int, std::vector<std::string>> page_clause_map;
    std::string format_profile;
    int schema_version = 2;
};

std::string clause_tree_to_json(const ClauseTree& t);
ClauseTree clause_tree_from_json(const std::string& json_text);
void write_tree_cache(const std::string& cache_path, const ClauseTree& t);
