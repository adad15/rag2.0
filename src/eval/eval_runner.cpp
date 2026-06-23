#include "eval/eval_runner.h"
#include "eval/retrieval_metrics.h"
#include "retrieve/text_search.h"
#include <optional>
#include <spdlog/spdlog.h>

namespace {
// 去空格（gold_standard_no 可能写成 "JTG 3420"，find_standard_by_code 要裸代号）。
std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '\t') out += c;
    return out;
}
}  // namespace

EvalReport run_eval(const std::vector<EvalCase>& cases,
                    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                    const SynonymDict& syn, const std::string& collection, int k,
                    const QueryPlanner& planner) {
    EvalReport rep;
    for (const auto& c : cases) {
        auto cands = text_retrieve(c.question, mv, embed, pg, syn, collection,
                                   /*per_path_k=*/k * 4, /*top_k=*/k, planner);

        // 回查每个候选的 method_no 与 (standard_id|clause_no)
        std::vector<std::string> cand_methods;
        std::vector<std::string> cand_clause_keys;
        cand_methods.reserve(cands.size());
        cand_clause_keys.reserve(cands.size());
        for (const auto& cand : cands) {
            auto row = pg.get_chunk(cand.chunk_id);
            cand_methods.push_back(row ? row->method_no : "");
            cand_clause_keys.push_back(
                row ? (row->standard_id + "|" + row->clause_no) : "");
        }

        CaseResult cr;
        cr.question = c.question;

        LegacyRetrievalView gv = derive_legacy_view(c);
        if (gv.kind == LegacyKind::Coverage) {
            cr.is_coverage = true;
            cr.covered = covered_count(cand_methods, gv.gold_methods);
            cr.gold_total = static_cast<int>(gv.gold_methods.size());
            rep.coverage_cases++;
        } else {
            std::string gold_key;
            const std::vector<std::string>* keys = nullptr;
            if (gv.kind == LegacyKind::PointMethod) {
                gold_key = gv.gold_method_no;
                keys = &cand_methods;
            } else if (gv.kind == LegacyKind::PointClause) {
                std::string sid = pg.find_standard_by_code(strip_spaces(gv.gold_standard_no));
                if (sid.empty())
                    spdlog::warn("样本不计分（标准 {} 未找到）: {}", gv.gold_standard_no, c.question);
                else { gold_key = sid + "|" + gv.gold_clause_no; keys = &cand_clause_keys; }
            }
            if (keys) {
                cr.scored = true;
                cr.rank = first_hit_rank(*keys, gold_key);
                rep.point_cases++;
                if (hit_at_k(cr.rank, k)) rep.point_hits++;
                rep.mrr_sum += reciprocal_rank(cr.rank);
            }
        }
        rep.results.push_back(cr);
    }
    return rep;
}
