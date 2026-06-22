#include "eval/dataset.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

std::vector<EvalCase> parse_dataset(const std::string& json_text) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        throw std::runtime_error("eval dataset must be a JSON array");

    std::vector<EvalCase> out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        EvalCase c;
        c.question        = item.value("question", "");
        c.note            = item.value("note", "");
        c.gold_standard_no = item.value("gold_standard_no", "");
        c.gold_clause_no  = item.value("gold_clause_no", "");
        c.gold_method_no  = item.value("gold_method_no", "");
        if (item.contains("gold_methods") && item["gold_methods"].is_array())
            for (const auto& m : item["gold_methods"])
                if (m.is_string()) c.gold_methods.push_back(m.get<std::string>());
        if (item.contains("gold_values") && item["gold_values"].is_array())
            for (const auto& v : item["gold_values"])
                if (v.is_string()) c.gold_values.push_back(v.get<std::string>());
        out.push_back(std::move(c));
    }
    return out;
}
