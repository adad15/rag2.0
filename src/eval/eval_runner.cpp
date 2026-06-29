#include "eval/eval_runner.h"
#include "eval/retrieval_metrics.h"
#include "retrieve/text_search.h"
#include <optional>
#include <set>
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

RichReport run_rich_eval(const std::vector<EvalCase>& cases,
                         milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                         const SynonymDict& syn, const std::string& collection,
                         const QueryPlanner& planner) {
    RichReport rep;
    rep.ks = {1, 3, 5, 10, 20};
    for (const auto& c : cases) {
        if (c.must_have_groups.empty()) continue;   // 纯数值题/无检索 gold → 不计入

        // top_k=20（k 集上限），per_path_k=80（=20*4，与 run_eval 的 k*4 同比例）
        auto cands = text_retrieve(c.question, mv, embed, pg, syn, collection,
                                   /*per_path_k=*/80, /*top_k=*/20, planner);

        // 候选标识集（按排名）：chunk_id + method_no + standard_id|clause_no
        std::vector<std::set<std::string>> cand_keys;
        cand_keys.reserve(cands.size());
        for (const auto& cand : cands) {
            std::set<std::string> keys;
            keys.insert("cid:" + cand.chunk_id);
            auto row = pg.get_chunk(cand.chunk_id);
            if (row) {
                if (!row->method_no.empty()) keys.insert("m:" + row->method_no);
                if (!row->clause_no.empty())
                    keys.insert("c:" + row->standard_id + "|" + row->clause_no);
            }
            cand_keys.push_back(std::move(keys));
        }

        // 每组的可接受标识集：chunk_ids + stable_refs(method / 解析后的 sid|clause)
        std::vector<std::set<std::string>> group_keys;
        for (const auto& g : c.must_have_groups) {
            std::set<std::string> gk;
            for (const auto& id : g.chunk_ids) gk.insert("cid:" + id);
            for (const auto& r : g.stable_refs) {
                if (!r.method_no.empty()) gk.insert("m:" + r.method_no);
                if (!r.clause_no.empty()) {
                    std::string sid = pg.find_standard_by_code(strip_spaces(r.standard_no));
                    if (!sid.empty()) gk.insert("c:" + sid + "|" + r.clause_no);
                    else spdlog::warn("[rich-eval] 标准 {} 未找到，组 {} 的 clause key 跳过: {}",
                                      r.standard_no, g.group_id, c.question);
                }
            }
            group_keys.push_back(std::move(gk));
        }

        // —— 富排序指标所需的每候选数据 ——
        std::set<std::string> distractor_set(c.distractor_chunks.begin(), c.distractor_chunks.end());
        std::set<std::string> acceptable_set(c.acceptable_chunks.begin(), c.acceptable_chunks.end());
        std::vector<std::string> cand_ids;
        cand_ids.reserve(cands.size());
        for (const auto& cand : cands) cand_ids.push_back(cand.chunk_id);

        std::vector<double> gains;                       // 去重后增益
        std::vector<std::set<std::string>> sig_by_rank;  // Redundancy 签名
        std::set<int> covered_groups_seen;
        std::set<std::string> acc_seen;
        int first_gold_rank = 0;
        for (size_t r = 0; r < cands.size(); ++r) {
            std::set<int> g_here;                        // 该候选覆盖的组
            for (size_t gi = 0; gi < group_keys.size(); ++gi)
                for (const auto& key : cand_keys[r])
                    if (group_keys[gi].count(key)) { g_here.insert(static_cast<int>(gi)); break; }
            if (first_gold_rank == 0 && !g_here.empty()) first_gold_rank = static_cast<int>(r) + 1;

            bool new_group = false;
            for (int gi : g_here) if (!covered_groups_seen.count(gi)) { new_group = true; break; }
            double gain = 0.0;
            if (new_group) gain = 2.0;
            else if (acceptable_set.count(cand_ids[r]) && !acc_seen.count(cand_ids[r])) {
                gain = 1.0; acc_seen.insert(cand_ids[r]);
            }
            gains.push_back(gain);
            for (int gi : g_here) covered_groups_seen.insert(gi);

            std::set<std::string> sg;
            for (int gi : g_here) sg.insert("g:" + std::to_string(gi));
            for (const auto& key : cand_keys[r]) if (key.rfind("m:", 0) == 0) sg.insert(key);
            sig_by_rank.push_back(std::move(sg));
        }
        std::vector<double> achievable;
        for (size_t gi = 0; gi < group_keys.size(); ++gi) achievable.push_back(2.0);
        for (size_t ai = 0; ai < acceptable_set.size(); ++ai) achievable.push_back(1.0);
        int first_distractor_rank = first_rank_in_set(cand_ids, distractor_set);

        RichCaseResult cr;
        cr.question = c.question;
        cr.group_total = static_cast<int>(group_keys.size());
        cr.distractor_total = static_cast<int>(distractor_set.size());
        cr.first_distractor_rank = first_distractor_rank;
        cr.first_gold_rank = first_gold_rank;
        for (int kk : rep.ks) {
            cr.covered.push_back(covered_groups_at_k(group_keys, cand_keys, kk));
            cr.distractor_in_k.push_back(count_in_set_at_k(cand_ids, distractor_set, kk));
            cr.ndcg.push_back(ndcg_at_k(gains, achievable, kk));
            cr.redundancy.push_back(redundancy_at_k(sig_by_rank, kk));
        }
        rep.results.push_back(std::move(cr));
    }
    return rep;
}
