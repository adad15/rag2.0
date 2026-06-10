#include "retrieve/retrieval_chunk.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

namespace {

std::string string_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

int int_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number_integer()) return 0;
    return j[key].get<int>();
}

bool bool_value(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_boolean()) return false;
    return j[key].get<bool>();
}

std::vector<std::string> string_array_value(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j[key].is_array()) return out;
    for (const auto& v : j[key]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string trim_ascii_space(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) ++begin;
    size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) --end;
    return s.substr(begin, end - begin);
}

void append_line(std::string& out, const std::string& line) {
    std::string text = trim_ascii_space(line);
    if (text.empty()) return;
    if (!out.empty()) out += "\n";
    out += text;
}

void append_section(std::string& out, const std::string& heading, const std::vector<std::string>& lines,
                    size_t max_count, size_t max_chars) {
    std::vector<std::string> kept;
    size_t chars = 0;
    for (const auto& raw : lines) {
        std::string line = trim_ascii_space(raw);
        if (line.empty()) continue;
        if (std::find(kept.begin(), kept.end(), line) != kept.end()) continue;
        if (kept.size() >= max_count) break;
        if (chars + line.size() > max_chars) break;
        chars += line.size();
        kept.push_back(line);
    }
    if (kept.empty()) return;
    if (!out.empty()) out += "\n\n";
    out += heading;
    for (const auto& line : kept) {
        out += "\n";
        out += line;
    }
}

std::string node_label(const TreeNode& n) {
    std::string label;
    append_line(label, n.number + (n.title.empty() ? "" : " " + n.title));
    if (label.empty()) append_line(label, n.title);
    if (label.empty()) append_line(label, n.number);
    return label;
}

std::map<std::string, const TreeNode*> index_nodes(const ClauseTree& tree) {
    std::map<std::string, const TreeNode*> by_id;
    for (const auto& n : tree.nodes) by_id[n.node_id] = &n;
    return by_id;
}

std::vector<const TreeNode*> ancestor_chain(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::vector<const TreeNode*> reversed;
    std::string current = n.parent_id;
    while (!current.empty()) {
        auto it = by_id.find(current);
        if (it == by_id.end()) break;
        reversed.push_back(it->second);
        current = it->second->parent_id;
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::string path_text_for(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::vector<std::string> labels;
    for (const TreeNode* parent : ancestor_chain(n, by_id)) {
        std::string label = node_label(*parent);
        if (!label.empty()) labels.push_back(label);
    }
    std::string self = node_label(n);
    if (!self.empty()) labels.push_back(self);

    std::string out;
    for (const auto& label : labels) {
        if (!out.empty()) out += " > ";
        out += label;
    }
    return out;
}

std::string compose_atomic_text(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    append_section(out, "公式：", n.formulas, 5, 1200);
    return out;
}

std::string compose_embedding_text(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    append_section(out, "相关图表题：", n.captions, 3, 1200);
    append_section(out, "公式：", n.formulas, 5, 1200);
    return out;
}

std::string chunk_type_for(const TreeNode& n) {
    if (n.node_id.find(":appendix:") != std::string::npos) return "appendix";
    if (n.node_id.find(":explanation:") != std::string::npos) return "explanation";
    return "body";
}

RetrievalChunk chunk_from_leaf(const ClauseTree& tree, const TreeNode& n,
                               const std::map<std::string, const TreeNode*>& by_id) {
    RetrievalChunk c;
    c.chunk_id = n.node_id + "#main";
    c.node_id = n.node_id;
    c.standard_id = tree.standard_id;
    c.standard_no = tree.standard_no;
    c.chunk_type = chunk_type_for(n);
    c.clause_no = n.number;
    c.title = n.title;
    c.path_text = path_text_for(n, by_id);
    c.atomic_text = compose_atomic_text(n);
    c.embedding_text = compose_embedding_text(n);
    c.context_text = c.atomic_text;
    c.captions = n.captions;
    c.formulas = n.formulas;
    c.page_start = n.page_start;
    c.page_end = n.page_end;
    c.has_table = n.has_table;
    c.has_formula = n.has_formula;
    c.has_figure = n.has_figure;
    c.suspect = n.suspect;
    return c;
}

}  // namespace

RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree) {
    RetrievalChunkCache cache;
    cache.standard_id = tree.standard_id;
    cache.standard_no = tree.standard_no;

    auto by_id = index_nodes(tree);
    for (const auto& n : tree.nodes) {
        if (!n.is_leaf) continue;
        cache.chunks.push_back(chunk_from_leaf(tree, n, by_id));
    }

    return cache;
}

std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache) {
    json j;
    j["schema_version"] = cache.schema_version;
    j["standard_id"] = cache.standard_id;
    j["standard_no"] = cache.standard_no;
    j["chunks"] = json::array();
    for (const auto& c : cache.chunks) {
        j["chunks"].push_back({
            {"chunk_id", c.chunk_id},
            {"node_id", c.node_id},
            {"standard_id", c.standard_id},
            {"standard_no", c.standard_no},
            {"chunk_type", c.chunk_type},
            {"clause_no", c.clause_no},
            {"method_no", c.method_no},
            {"title", c.title},
            {"path_text", c.path_text},
            {"atomic_text", c.atomic_text},
            {"embedding_text", c.embedding_text},
            {"context_text", c.context_text},
            {"captions", c.captions},
            {"formulas", c.formulas},
            {"page_start", c.page_start},
            {"page_end", c.page_end},
            {"has_table", c.has_table},
            {"has_formula", c.has_formula},
            {"has_figure", c.has_figure},
            {"suspect", c.suspect},
        });
    }
    return j.dump(2);
}

RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, false);
    RetrievalChunkCache cache;
    if (!j.is_object()) return cache;

    cache.schema_version = j.value("schema_version", 1);
    cache.standard_id = string_value(j, "standard_id");
    cache.standard_no = string_value(j, "standard_no");

    if (j.contains("chunks") && j["chunks"].is_array()) {
        for (const auto& e : j["chunks"]) {
            if (!e.is_object()) continue;
            RetrievalChunk c;
            c.chunk_id = string_value(e, "chunk_id");
            c.node_id = string_value(e, "node_id");
            c.standard_id = string_value(e, "standard_id");
            c.standard_no = string_value(e, "standard_no");
            c.chunk_type = string_value(e, "chunk_type");
            c.clause_no = string_value(e, "clause_no");
            c.method_no = string_value(e, "method_no");
            c.title = string_value(e, "title");
            c.path_text = string_value(e, "path_text");
            c.atomic_text = string_value(e, "atomic_text");
            c.embedding_text = string_value(e, "embedding_text");
            c.context_text = string_value(e, "context_text");
            c.captions = string_array_value(e, "captions");
            c.formulas = string_array_value(e, "formulas");
            c.page_start = int_value(e, "page_start");
            c.page_end = int_value(e, "page_end");
            c.has_table = bool_value(e, "has_table");
            c.has_formula = bool_value(e, "has_formula");
            c.has_figure = bool_value(e, "has_figure");
            c.suspect = string_value(e, "suspect");
            cache.chunks.push_back(std::move(c));
        }
    }

    return cache;
}

void write_chunk_cache(const std::string& cache_path, const RetrievalChunkCache& cache) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary);
    if (!f) {
        spdlog::warn("chunk cache write failed: {}", cache_path);
        return;
    }
    f << retrieval_chunk_cache_to_json(cache);
}
