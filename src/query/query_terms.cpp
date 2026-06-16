#include "query/query_terms.h"
#include <fstream>

std::vector<std::string> match_terms(const std::string& text,
                                     const std::vector<std::string>& dict) {
    std::vector<std::string> out;
    for (const auto& w : dict) {
        if (w.empty() || text.find(w) == std::string::npos) continue;
        bool dup = false;
        for (const auto& o : out) if (o == w) { dup = true; break; }
        if (!dup) out.push_back(w);
    }
    return out;
}

namespace {
std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

QueryTerms load_query_terms(const std::string& path) {
    QueryTerms t;
    std::ifstream f(path, std::ios::binary);
    if (!f) return t;
    std::vector<std::string>* cur = nullptr;
    std::string line;
    while (std::getline(f, line)) {
        std::string s = strip(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            std::string name = s.substr(1, s.size() - 2);
            if (name == "stopword")        cur = &t.stopwords;
            else if (name == "background") cur = &t.background;
            else if (name == "section")    cur = &t.sections;
            else if (name == "instrument") cur = &t.instruments;
            else cur = nullptr;
            continue;
        }
        if (cur) cur->push_back(s);
    }
    return t;
}
