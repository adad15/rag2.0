#include "structure/tree_builder.h"

#include "structure/format_profile.h"
#include "structure/region_segmenter.h"
#include "structure/toc_parser.h"

#include <cctype>
#include <algorithm>
#include <regex>
#include <vector>

namespace {

std::string normalize_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 2 < s.size() &&
            static_cast<unsigned char>(s[i]) == 0xE2 &&
            static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0x94 ||
             static_cast<unsigned char>(s[i + 2]) == 0x93)) {
            out += '-';
            i += 3;
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : normalize_dashes(s)) {
        if (c != ' ' && c != '\t') out += c;
    }
    return out;
}

std::string display_text(const ParseElement* e) {
    if (!e->text.empty()) return e->text;
    if (!e->title.empty()) return e->title;
    return e->caption;
}

bool starts_with_bytes(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

size_t first_non_space(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}

std::string trim_ascii_space(const std::string& s) {
    size_t begin = first_non_space(s);
    size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

char ascii_upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

bool ascii_letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool looks_like_caption(const ParseElement* e) {
    const std::string text = display_text(e);
    if (starts_with_bytes(text, "\xE5\x9B\xBE\xE4\xB8\xAD") ||
        starts_with_bytes(text, "\xE8\xA1\xA8\xE4\xB8\xAD")) {
        return false;
    }
    if (e->is_caption) return true;
    if (e->raw_label == "figure_title" || e->raw_label == "chart_title" ||
        e->raw_label == "table_title") {
        return true;
    }
    return starts_with_bytes(text, "\xE5\x9B\xBE") ||
           starts_with_bytes(text, "\xE8\xA1\xA8") ||
           starts_with_bytes(text, "\xE7\xBB\xAD\xE8\xA1\xA8") ||
           starts_with_bytes(text, "\xE9\x99\x84\xE5\x9B\xBE") ||
           starts_with_bytes(text, "\xE9\x99\x84\xE8\xA1\xA8") ||
           starts_with_bytes(text, "figure ") ||
           starts_with_bytes(text, "table ");
}

bool text_looks_like_table_caption(const std::string& text) {
    return starts_with_bytes(text, "\xE8\xA1\xA8") ||
           starts_with_bytes(text, "\xE7\xBB\xAD\xE8\xA1\xA8") ||
           starts_with_bytes(text, "\xE9\x99\x84\xE8\xA1\xA8") ||
           starts_with_bytes(text, "table ");
}

bool text_looks_like_figure_caption(const std::string& text) {
    return starts_with_bytes(text, "\xE5\x9B\xBE") ||
           starts_with_bytes(text, "\xE9\x99\x84\xE5\x9B\xBE") ||
           starts_with_bytes(text, "figure ");
}

bool looks_like_figure_caption(const ParseElement* e) {
    const std::string text = display_text(e);
    if (text_looks_like_table_caption(text)) return false;
    if (text_looks_like_figure_caption(text)) return true;
    if (e->raw_label == "table_title") return false;
    return e->raw_label == "figure_title" || e->raw_label == "chart_title";
}

void append_text(TreeNode& n, const std::string& text, int page) {
    if (!text.empty()) {
        if (!n.text.empty()) n.text += "\n";
        n.text += text;
    }
    if (page > n.page_end) n.page_end = page;
}

void add_formula_to_node(TreeNode& n, const std::string& formula, int page) {
    if (formula.empty()) return;
    n.has_formula = true;
    if (std::find(n.formulas.begin(), n.formulas.end(), formula) == n.formulas.end()) {
        n.formulas.push_back(formula);
    }

    if (n.text.find(formula) == std::string::npos) {
        size_t pos = n.text.find("\n\xE5\xBC\x8F\xE4\xB8\xAD");
        if (pos == std::string::npos && starts_with_bytes(n.text, "\xE5\xBC\x8F\xE4\xB8\xAD")) {
            n.text = formula + "\n" + n.text;
        } else if (pos != std::string::npos) {
            n.text.insert(pos, "\n" + formula);
        } else {
            append_text(n, formula, page);
        }
    }
    if (page > n.page_end) n.page_end = page;
}

void attach_caption(ClauseTree& t, int current_node, const ParseElement* e) {
    if (current_node < 0) return;
    const std::string text = display_text(e);
    if (!text.empty()) t.nodes[current_node].captions.push_back(text);
    if (looks_like_figure_caption(e)) {
        t.nodes[current_node].has_figure = true;
    } else {
        t.nodes[current_node].has_table = true;
    }
    if (e->page_no > t.nodes[current_node].page_end) t.nodes[current_node].page_end = e->page_no;
}

void attach_table_or_formula(ClauseTree& t, int current_node, const ParseElement* e) {
    if (current_node < 0) return;
    if (e->type == ElementType::Table) {
        t.nodes[current_node].has_table = true;
        if (!e->table_html.empty()) t.nodes[current_node].table_htmls.push_back(e->table_html);
    } else if (e->type == ElementType::Formula) {
        std::string text = display_text(e);
        add_formula_to_node(t.nodes[current_node], text, e->page_no);
    }
    if (e->page_no > t.nodes[current_node].page_end) t.nodes[current_node].page_end = e->page_no;
}

size_t consume_loose_number_prefix(const std::string& text, const std::string& number) {
    if (number.empty()) return std::string::npos;
    size_t i = first_non_space(text);
    size_t j = 0;
    while (j < number.size()) {
        if (i >= text.size()) return std::string::npos;
        if (number[j] == '.') {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (i >= text.size() || text[i] != '.') return std::string::npos;
            ++i;
            ++j;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            continue;
        }
        char actual = text[i];
        char expected = number[j];
        if (ascii_letter(actual) && ascii_letter(expected)) {
            if (ascii_upper(actual) != ascii_upper(expected)) return std::string::npos;
        } else if (actual != expected) {
            return std::string::npos;
        }
        ++i;
        ++j;
    }
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
    return i;
}

std::string strip_leading_number(const std::string& text, const std::string& number) {
    if (number.empty()) return text;
    if (text.rfind(number, 0) != 0) return text;
    size_t i = number.size();
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
        ++i;
    }
    return text.substr(i);
}

std::string strip_number_for_title(const std::string& text, const std::string& number) {
    size_t loose = consume_loose_number_prefix(text, number);
    if (loose != std::string::npos) return text.substr(loose);
    return strip_leading_number(text, number);
}

std::string extract_appendix_heading_number(const std::string& text) {
    static const std::string appendix = "\xE9\x99\x84\xE5\xBD\x95";
    size_t i = first_non_space(text);
    if (text.size() - i < appendix.size() || text.compare(i, appendix.size(), appendix) != 0) return "";
    i += appendix.size();
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) return "";
    if (!ascii_letter(text[i]) && !ascii_digit(text[i])) return "";

    std::string number;
    while (i < text.size() && (ascii_letter(text[i]) || ascii_digit(text[i]))) {
        number += ascii_letter(text[i]) ? ascii_upper(text[i]) : text[i];
        ++i;
    }
    return number;
}

std::string extract_appendix_clause_number(const std::string& text) {
    size_t i = first_non_space(text);
    if (i >= text.size() || !ascii_letter(text[i])) return "";

    std::string number(1, ascii_upper(text[i++]));
    int segments = 0;
    while (true) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || text[i] != '.') break;
        ++i;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || !ascii_digit(text[i])) return "";
        number += '.';
        while (i < text.size() && ascii_digit(text[i])) number += text[i++];
        ++segments;
    }
    return segments > 0 ? number : "";
}

bool is_appendix_number(const std::string& number) {
    if (number.empty() || !ascii_letter(number[0])) return false;
    for (size_t i = 1; i < number.size(); ++i) {
        if (!ascii_digit(number[i]) && number[i] != '.') return false;
    }
    return true;
}

int appendix_level_for(const std::string& number) {
    if (!is_appendix_number(number)) return 0;
    int dots = 0;
    for (char c : number) if (c == '.') ++dots;
    if (dots == 0) return 1;
    if (number.find(".0.") != std::string::npos) return dots;
    return dots + 1;
}

std::string extract_test_number(const std::string& s) {
    static const std::regex re(R"(T\s*\d{4}\s*-\s*\d{4})");
    std::smatch m;
    std::string norm = normalize_dashes(s);
    if (std::regex_search(norm, m, re)) return m.str(0);
    return "";
}

std::string parent_number_for_gap(const std::string& number) {
    size_t pos = number.find_last_of('.');
    if (pos == std::string::npos) return "";
    return number.substr(0, pos);
}

std::string remove_ascii_spaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\t') out += c;
    }
    return out;
}

std::string extract_line_number(const std::string& line) {
    std::string text = trim_ascii_space(line);
    std::smatch m;
    static const std::regex appendix_re(R"(^([A-Za-z]\s*\.\s*\d+(?:\s*\.\s*\d+)*))");
    if (std::regex_search(text, m, appendix_re)) return remove_ascii_spaces(m[1].str());

    static const std::regex decimal_re(R"(^([1-9]\d*(?:\.\d+){1,4}))");
    if (std::regex_search(text, m, decimal_re)) return m[1].str();

    return "";
}

bool contains_cjk(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0xE4 && c <= 0xE9) return true;
    }
    return false;
}

bool looks_like_standalone_formula_line(const std::string& line) {
    std::string text = trim_ascii_space(line);
    if (text.empty()) return false;
    if (text[0] == '(') return false;          // formula number, e.g. (7.2.1)
    if (text.find('=') == std::string::npos) return false;
    if (contains_cjk(text)) return false;      // prose lines such as "式中：..."

    return text.find('\\') != std::string::npos ||
           text.find('_') != std::string::npos ||
           text.find('^') != std::string::npos ||
           text.find('{') != std::string::npos ||
           text.find('}') != std::string::npos;
}

int find_node_index_for_number_page(const ClauseTree& t, const std::string& number, int page) {
    std::string current = number;
    while (!current.empty()) {
        int fallback = -1;
        for (int i = 0; i < static_cast<int>(t.nodes.size()); ++i) {
            const TreeNode& n = t.nodes[i];
            if (n.number != current) continue;
            if (n.page_start <= page && page <= n.page_end) return i;
            if (fallback < 0) fallback = i;
        }
        if (fallback >= 0) return fallback;
        current = parent_number_for_gap(current);
    }
    return -1;
}

void backfill_formulas_from_pages(ClauseTree& t, const ParsedDoc& doc) {
    for (const auto& page : doc.pages) {
        std::string current_number;
        size_t start = 0;
        while (start <= page.text.size()) {
            size_t end = page.text.find('\n', start);
            std::string line = end == std::string::npos
                ? page.text.substr(start)
                : page.text.substr(start, end - start);

            std::string line_number = extract_line_number(line);
            if (!line_number.empty()) current_number = line_number;

            if (!current_number.empty() && looks_like_standalone_formula_line(line)) {
                int node_idx = find_node_index_for_number_page(t, current_number, page.page_no);
                if (node_idx >= 0) add_formula_to_node(t.nodes[node_idx], trim_ascii_space(line), page.page_no);
            }

            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
}

std::string sid_for_region(const std::string& standard_id, Region region) {
    switch (region) {
        case Region::Explanation:
            return standard_id + ":explanation";
        case Region::Appendix:
            return standard_id + ":appendix";
        default:
            return standard_id;
    }
}

struct Builder {
    ClauseTree& t;
    std::string sid;

    int add(int parent_idx, int level, const std::string& number,
            const std::string& text, int page, const std::string& suspect) {
        TreeNode n;
        n.level = level;
        n.number = number;
        n.title = strip_number_for_title(text, number);
        n.text = strip_number_for_title(text, number);
        n.suspect = suspect;
        n.page_start = page;
        n.page_end = page;

        if (parent_idx >= 0) {
            n.parent_id = t.nodes[parent_idx].node_id;
            n.node_id = n.parent_id + "/" + sanitize(number);
        } else {
            n.node_id = sid + ":" + sanitize(number);
        }

        int idx = static_cast<int>(t.nodes.size());
        t.nodes.push_back(std::move(n));
        if (parent_idx >= 0) t.nodes[parent_idx].child_ids.push_back(t.nodes[idx].node_id);
        return idx;
    }
};

void build_group(ClauseTree& t, const RegionGroup& group, const std::string& sid, FormatProfile profile) {
    Builder b{t, sid};
    std::vector<std::pair<int, int>> stack;
    int current_node = -1;
    bool in_test_scope = false;

    for (const ParseElement* e : group.elements) {
        if (looks_like_caption(e)) {
            attach_caption(t, current_node, e);
            continue;
        }

        if (e->type == ElementType::Table || e->type == ElementType::Formula) {
            attach_table_or_formula(t, current_node, e);
            continue;
        }

        std::string number = e->clause_no;
        bool this_is_test = false;
        if (group.region == Region::Appendix) {
            const std::string text = display_text(e);
            std::string appendix_number = extract_appendix_heading_number(text);
            if (appendix_number.empty()) appendix_number = extract_appendix_clause_number(text);
            if (!appendix_number.empty()) number = appendix_number;
        }
        if (profile == FormatProfile::B_testno) {
            std::string test_no = extract_test_number(number + "\n" + display_text(e));
            if (!test_no.empty()) {
                number = test_no;
                this_is_test = true;
            }
        }

        if (number.empty()) {
            if (current_node >= 0) append_text(t.nodes[current_node], display_text(e), e->page_no);
            continue;
        }

        int lvl = group.region == Region::Appendix && is_appendix_number(number)
            ? appendix_level_for(number)
            : level_for(profile, number, in_test_scope);
        if (lvl == 0) {
            if (current_node >= 0) append_text(t.nodes[current_node], display_text(e), e->page_no);
            continue;
        }

        if (lvl > retrieval_depth(profile)) {
            if (current_node >= 0) append_text(t.nodes[current_node], display_text(e), e->page_no);
            continue;
        }

        while (!stack.empty() && stack.back().second >= lvl) stack.pop_back();
        int parent_idx = stack.empty() ? -1 : stack.back().first;
        int parent_level = stack.empty() ? 0 : stack.back().second;

        if (lvl - parent_level >= 2 && decimal_depth(number) >= 1) {
            std::string mid = parent_number_for_gap(number);
            if (!mid.empty()) {
                int virtual_level = lvl - 1;
                int virtual_idx = b.add(parent_idx, virtual_level, mid, "", e->page_no, "gap");
                stack.push_back({virtual_idx, virtual_level});
                parent_idx = virtual_idx;
            }
        }

        int idx = b.add(parent_idx, lvl, number, display_text(e), e->page_no, e->suspect);
        stack.push_back({idx, lvl});
        current_node = idx;

        if (this_is_test) {
            in_test_scope = true;
        } else if (profile == FormatProfile::B_testno && lvl <= 1) {
            in_test_scope = false;
        }
    }
}

}  // namespace

ClauseTree build_clause_tree(const ParsedDoc& doc, const std::string& standard_id) {
    ClauseTree t;
    t.standard_id = standard_id;
    t.standard_no = doc.standard_no;

    TocResult toc = detect_format_from_toc(doc);
    FormatProfile profile = toc.detected ? toc.profile : detect_format_from_body(doc);
    t.format_profile = profile_to_string(profile);

    for (const auto& group : segment_regions(doc)) {
        build_group(t, group, sid_for_region(standard_id, group.region), profile);
    }

    backfill_formulas_from_pages(t, doc);

    for (auto& n : t.nodes) n.is_leaf = n.child_ids.empty();
    for (const auto& n : t.nodes) {
        if (!n.is_leaf) continue;
        for (int p = n.page_start; p <= n.page_end && p > 0; ++p) {
            t.page_clause_map[p].push_back(n.node_id);
        }
    }

    return t;
}
