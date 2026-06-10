#include "structure/clause_tree.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

std::string clause_tree_to_json(const ClauseTree& t) {
    json j;
    j["standard_id"] = t.standard_id;
    j["standard_no"] = t.standard_no;
    j["format_profile"] = t.format_profile;
    j["schema_version"] = t.schema_version;
    j["nodes"] = json::array();
    for (const auto& n : t.nodes) {
        j["nodes"].push_back({
            {"node_id", n.node_id},
            {"level", n.level},
            {"number", n.number},
            {"title", n.title},
            {"text", n.text},
            {"page_start", n.page_start},
            {"page_end", n.page_end},
            {"parent_id", n.parent_id},
            {"child_ids", n.child_ids},
            {"is_leaf", n.is_leaf},
            {"has_table", n.has_table},
            {"has_formula", n.has_formula},
            {"has_figure", n.has_figure},
            {"captions", n.captions},
            {"table_htmls", n.table_htmls},
            {"formulas", n.formulas},
            {"suspect", n.suspect},
        });
    }
    j["page_clause_map"] = json::object();
    for (const auto& kv : t.page_clause_map) {
        j["page_clause_map"][std::to_string(kv.first)] = kv.second;
    }
    return j.dump(2);
}

ClauseTree clause_tree_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, false);
    ClauseTree t;
    if (!j.is_object()) return t;

    t.standard_id = j.value("standard_id", "");
    t.standard_no = j.value("standard_no", "");
    t.format_profile = j.value("format_profile", "");
    t.schema_version = j.value("schema_version", 1);

    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const auto& e : j["nodes"]) {
            TreeNode n;
            n.node_id = e.value("node_id", "");
            n.level = e.value("level", 0);
            n.number = e.value("number", "");
            n.title = e.value("title", "");
            n.text = e.value("text", "");
            n.page_start = e.value("page_start", 0);
            n.page_end = e.value("page_end", 0);
            n.parent_id = e.value("parent_id", "");
            if (e.contains("child_ids") && e["child_ids"].is_array()) {
                for (const auto& id : e["child_ids"]) {
                    if (id.is_string()) n.child_ids.push_back(id.get<std::string>());
                }
            }
            n.is_leaf = e.value("is_leaf", false);
            n.has_table = e.value("has_table", false);
            n.has_formula = e.value("has_formula", false);
            n.has_figure = e.value("has_figure", false);
            if (e.contains("captions") && e["captions"].is_array()) {
                for (const auto& c : e["captions"]) {
                    if (c.is_string()) n.captions.push_back(c.get<std::string>());
                }
            }
            if (e.contains("table_htmls") && e["table_htmls"].is_array()) {
                for (const auto& h : e["table_htmls"]) {
                    if (h.is_string()) n.table_htmls.push_back(h.get<std::string>());
                }
            }
            if (e.contains("formulas") && e["formulas"].is_array()) {
                for (const auto& f : e["formulas"]) {
                    if (f.is_string()) n.formulas.push_back(f.get<std::string>());
                }
            }
            n.suspect = e.value("suspect", "");
            t.nodes.push_back(std::move(n));
        }
    }

    if (j.contains("page_clause_map") && j["page_clause_map"].is_object()) {
        for (auto it = j["page_clause_map"].begin(); it != j["page_clause_map"].end(); ++it) {
            int page = 0;
            try {
                page = std::stoi(it.key());
            } catch (...) {
                continue;
            }
            if (!it.value().is_array()) continue;
            for (const auto& id : it.value()) {
                if (id.is_string()) t.page_clause_map[page].push_back(id.get<std::string>());
            }
        }
    }

    return t;
}

void write_tree_cache(const std::string& cache_path, const ClauseTree& t) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary);
    if (!f) {
        spdlog::warn("tree cache write failed: {}", cache_path);
        return;
    }
    f << clause_tree_to_json(t);
}
