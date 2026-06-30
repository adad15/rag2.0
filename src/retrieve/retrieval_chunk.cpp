#include "retrieve/retrieval_chunk.h"
#include "parse/method_no.h"

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

std::string method_no_for(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::string self = extract_method_no(n.number + " " + n.title + " " + n.node_id);
    if (!self.empty()) return self;
    std::vector<const TreeNode*> ancestors = ancestor_chain(n, by_id);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        std::string found = extract_method_no((*it)->number + " " + (*it)->title + " " + (*it)->node_id);
        if (!found.empty()) return found;
    }
    return "";
}

std::string chunk_type_for(const TreeNode& n) {
    if (n.node_id.find(":appendix:") != std::string::npos) return "appendix";
    if (n.node_id.find(":explanation:") != std::string::npos) return "explanation";
    return "body";
}

std::string leaf_text_for_context(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    return out;
}

void collect_leaf_descendants(const TreeNode& parent,
                              const std::map<std::string, const TreeNode*>& by_id,
                              std::vector<const TreeNode*>& out) {
    for (const auto& child_id : parent.child_ids) {
        auto it = by_id.find(child_id);
        if (it == by_id.end()) continue;
        const TreeNode* child = it->second;
        if (child->is_leaf) {
            out.push_back(child);
        } else {
            collect_leaf_descendants(*child, by_id, out);
        }
    }
}

std::vector<const TreeNode*> context_leaves_for(const TreeNode& n,
                                                const std::map<std::string, const TreeNode*>& by_id) {
    auto parent_it = by_id.find(n.parent_id);
    if (parent_it == by_id.end()) return {&n};

    std::vector<const TreeNode*> leaves;
    collect_leaf_descendants(*parent_it->second, by_id, leaves);
    if (leaves.empty()) return {&n};
    return leaves;
}

std::string compose_context_text(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    constexpr size_t max_chars = 6000;
    std::string out = "路径：" + path_text_for(n, by_id);

    // 不能用 out.find(n.text) 判断当前叶子是否已拼入：拼入的是裁剪后的文本，
    // 原文首尾带空白时查找会失配，导致当前叶子重复出现。
    bool current_included = false;
    std::vector<const TreeNode*> leaves = context_leaves_for(n, by_id);
    for (const TreeNode* leaf : leaves) {
        std::string part = leaf_text_for_context(*leaf);
        if (part.empty()) continue;
        if (out.size() + part.size() + 2 > max_chars && leaf->node_id != n.node_id) continue;
        if (leaf->node_id == n.node_id) current_included = true;
        out += "\n\n";
        out += part;
    }

    if (!current_included) {
        std::string current = leaf_text_for_context(n);
        if (!current.empty()) {
            out += "\n\n";
            out += current;
        }
    }

    return out;
}

std::string compact_code(const std::string& s) {
    std::string out;
    for (char ch : s) if (ch != ' ') out += ch;   // "GB 175-2023" -> "GB175-2023"
    return out;
}

std::vector<std::string> compose_bm25_terms(const ClauseTree& tree, const TreeNode& n,
                                            const std::string& method_no) {
    std::vector<std::string> terms;
    auto add = [&](const std::string& t) {
        if (t.empty()) return;
        if (std::find(terms.begin(), terms.end(), t) == terms.end()) terms.push_back(t);
    };
    if (!tree.standard_no.empty()) add(compact_code(tree.standard_no));
    if (!method_no.empty()) { add(method_no); add(compact_code(method_no)); }

    const std::string& body = n.text;
    auto has = [&](const char* kw) { return body.find(kw) != std::string::npos; };
    bool has_qualify = has("合格") || has("不合格");
    bool has_require = has("应") || has("不得") || has("不应") || has("应符合");
    bool has_method  = has("法") && has("合格");
    if (has_qualify) { add("判定"); add("要求"); add("合格"); }
    if (has_require) { add("要求"); add("规定"); }
    if (has_method)  { add("方法"); add("试验方法"); add("判定"); }
    return terms;
}

std::string compose_bm25_text(const ClauseTree& tree, const TreeNode& n,
                              const std::map<std::string, const TreeNode*>& by_id,
                              const std::string& method_no) {
    std::string out;
    if (!tree.standard_no.empty()) {
        append_line(out, "标准：" + tree.standard_no);
        append_line(out, "标准代号：" + tree.standard_no + " " + compact_code(tree.standard_no));
    }
    std::string path = path_text_for(n, by_id);
    if (!path.empty()) append_line(out, "路径：" + path);
    std::string clause = node_label(n);
    if (!clause.empty()) append_line(out, "条款：" + clause);

    std::vector<std::string> terms = compose_bm25_terms(tree, n, method_no);
    if (!terms.empty()) {
        std::string joined;
        for (const auto& t : terms) { if (!joined.empty()) joined += " "; joined += t; }
        append_line(out, "检索词：" + joined);
    }
    std::string body = compose_embedding_text(n);
    if (!body.empty()) {
        if (!out.empty()) out += "\n";
        out += "正文：\n" + body;
    }
    constexpr size_t kMaxChars = 3500;   // < Milvus text.max_length=8192
    if (out.size() > kMaxChars) out = out.substr(0, kMaxChars);
    return out;
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
    c.method_no = method_no_for(n, by_id);
    c.title = n.title;
    c.path_text = path_text_for(n, by_id);
    c.atomic_text = compose_atomic_text(n);
    c.embedding_text = compose_embedding_text(n);
    c.context_text = compose_context_text(n, by_id);
    c.bm25_text = compose_bm25_text(tree, n, by_id, c.method_no);
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
            {"bm25_text", c.bm25_text},
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
            c.bm25_text = string_value(e, "bm25_text");
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
