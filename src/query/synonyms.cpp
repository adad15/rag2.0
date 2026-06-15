#include "query/synonyms.h"
#include <fstream>
#include <sstream>

namespace {

std::vector<std::string> split_commas(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t\r");
        size_t b = item.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

}  // namespace

void SynonymDict::load_from_lines(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        auto group = split_commas(line);
        for (size_t i = 0; i < group.size(); ++i)
            for (size_t j = 0; j < group.size(); ++j)
                if (i != j) alias_[group[i]].push_back(group[j]);
    }
}

void SynonymDict::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    load_from_lines(lines);
}

std::string SynonymDict::expand(const std::string& query) const {
    std::string extra;
    for (const auto& kv : alias_) {
        if (query.find(kv.first) != std::string::npos) {
            for (const auto& syn : kv.second) {
                if (query.find(syn) == std::string::npos &&
                    extra.find(syn) == std::string::npos)
                    extra += " " + syn;
            }
        }
    }
    return extra.empty() ? query : query + extra;
}
