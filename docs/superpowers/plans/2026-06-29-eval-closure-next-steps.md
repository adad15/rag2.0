# 评估闭环下一步（full review + 富排序指标）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把已就绪的 100 题标注接进评估闭环——先修 `mine_annotations.py` 让 review 覆盖全部题，再给 `eval --rich` 加 Distractor/Redundancy/nDCG 排序指标消费 `distractor_chunks`/`acceptable_chunks`，最后同步 README 并跑 baseline。

**Architecture:** Python 侧加 `--review-only`/`--allow-llm` 与 done-case review 重建（复用候选逻辑、优先读缓存、缺失标 `cache_missing`）。C++ 侧在 `src/eval/retrieval_metrics.{h,cpp}` 加纯指标函数（doctest 覆盖），在 `run_rich_eval` 复用现有 top-20 召回算出每题数据，在 `main.cpp` 的 `--rich` 段追加排序指标报告。不改 `text_retrieve`、不改 base `run_eval`、不动 Milvus schema。

**Tech Stack:** C++20 / MSBuild / doctest / nlohmann/json / spdlog；Python 3.13 / psycopg2 / requests / pytest。沿用 [[rag2-build-setup]]。

**Spec:** [`docs/superpowers/specs/2026-06-29-eval-closure-next-steps-design.md`](../specs/2026-06-29-eval-closure-next-steps-design.md)

**构建/测试（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
python -m pytest scripts/test_mine_annotations.py -v
```

**关键事实（源码勘察）：**
- `run_rich_eval`（`src/eval/eval_runner.cpp:72`）每题已做 top-20 召回，建好 `cand_keys`（每候选标识集 `cid:/m:/c:`）与 `group_keys`（每必要组），调 `covered_groups_at_k`。**新指标在同一循环里加。**
- 候选 `Candidate` 有 `chunk_id`；`EvalCase` 已有 `distractor_chunks`/`acceptable_chunks`（chunk_id 列表）。spec §6.1：首版 distractor/acceptable **只按 chunk_id 精确匹配**。
- 报告打印在 `main.cpp:519-543` 的 `if (rich) {…}` 块。
- 纯指标现住 `src/eval/retrieval_metrics.{h,cpp}`；doctest 在 `tests/test_retrieval_metrics.cpp`（搜 `covered_groups_at_k`）。
- Python：`mine_annotations.py` 已有 `build_user_message`/`resolve_gold_chunks`/`fetch_chunk_context`/`embed_text`/`milvus_search`/`cache_key`/`read_cache`/`build_gold_brief`/`render_review`/`process_case`/`main`；`GENERATOR_VERSION="annot-v1"`。

**提交纪律（重要）：** 工作区有用户改的 `docs/.../specs/*.md` 与未跟踪垃圾。**每次提交必用 pathspec**（`git add <files>` + `git commit -m "…" -- <files>`），**绝不** `git add -A`/裸 `git commit`。

**已知限制（实现者须知）：** review 重建依赖 `cache_key`（含候选集）。Milvus ANN 有跨次微漂移→候选集偶尔变→`cache_key` 变→读不到原缓存，会标 `cache_missing`（spec §5-step1-5 允许，不得编造理由）。这是预期行为，不是 bug。

---

## Task 1: Python full review / review-only

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加纯函数测试到 `scripts/test_mine_annotations.py`**

```python
def test_label_of_classifies_by_membership():
    d = {"d1", "d2"}; a = {"a1"}
    assert m.label_of("d1", d, a) == "distractor"
    assert m.label_of("a1", d, a) == "acceptable"
    assert m.label_of("x9", d, a) == "irrelevant"


def test_review_entry_from_annotated_labels_and_status():
    case = {"question": "q", "gold_method_no": "T0301-2024",
            "distractor_chunks": ["d1"], "acceptable_chunks": ["a1"]}
    candidates = [
        {"chunk_id": "d1", "method_no": "T0302-2024", "clause_no": "", "title": "粗集料取样", "snippet": "x"},
        {"chunk_id": "a1", "method_no": "", "clause_no": "2", "title": "概述", "snippet": "y"},
        {"chunk_id": "z9", "method_no": "", "clause_no": "", "title": "无关", "snippet": "z"},
    ]
    e = m.review_entry_from_annotated(case, candidates, reasons={"d1": "对象不同"}, cache_status="hit")
    assert e["question"] == "q"
    assert e["gold_brief"] == "T0301-2024"
    assert e["labels"] == {"distractor": ["d1"], "acceptable": ["a1"]}
    assert e["reasons"]["d1"] == "对象不同"
    assert e["cache_status"] == "hit"


def test_render_review_shows_cache_missing():
    reviews = [{"question": "q", "gold_brief": "T0301-2024", "cache_status": "cache_missing",
                "candidates": [{"chunk_id": "d1", "method_no": "T0302-2024", "clause_no": "", "title": "粗集料取样", "snippet": "x"}],
                "labels": {"distractor": ["d1"], "acceptable": []}, "reasons": {}}]
    md = m.render_review(reviews)
    assert "cache_missing" in md
    assert "[distractor] d1" in md
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k "label_of or review_entry_from_annotated or cache_missing" -v`
Expected: FAIL（无 `label_of` / `review_entry_from_annotated`；`render_review` 不含 cache_status）。

- [ ] **Step 3: 实现纯函数（追加/改 `scripts/mine_annotations.py`）**

追加：
```python
def label_of(chunk_id, distractor_ids, acceptable_ids):
    """按隶属判定候选标签（drift 无关：标签来自标注集成员关系，不依赖候选池重建）。"""
    if chunk_id in distractor_ids:
        return "distractor"
    if chunk_id in acceptable_ids:
        return "acceptable"
    return "irrelevant"


def review_entry_from_annotated(case, candidates, reasons, cache_status):
    """从已标注 case + 重建候选池 + 缓存理由，构造一条 review entry。纯函数。"""
    d = set(case.get("distractor_chunks") or [])
    a = set(case.get("acceptable_chunks") or [])
    return {
        "question": case["question"],
        "gold_brief": case.get("gold_method_no") or case.get("gold_clause_no") or "",
        "candidates": candidates,
        "labels": {"distractor": list(case.get("distractor_chunks") or []),
                   "acceptable": list(case.get("acceptable_chunks") or [])},
        "reasons": reasons or {},
        "cache_status": cache_status,
    }
```

把现有 `render_review` 改为带 `cache_status` 标注（替换原函数体的标题两行）：
```python
def render_review(reviews):
    out = ["# 标注人审导出\n"]
    for r in reviews:
        cs = r.get("cache_status", "")
        tag = f" [{cs}]" if cs else ""
        out.append(f"## {r['question']}{tag}")
        out.append(f"- gold: {r['gold_brief']}")
        d = set(r["labels"]["distractor"])
        a = set(r["labels"]["acceptable"])
        reasons = r.get("reasons", {})
        for c in r["candidates"]:
            cid = c["chunk_id"]
            lab = "distractor" if cid in d else ("acceptable" if cid in a else "irrelevant")
            tagm = c.get("method_no") or c.get("clause_no") or "-"
            title = c.get("title", "")
            reason = " ".join(reasons.get(cid, "").split())
            snip = " ".join(c.get("snippet", "").split())[:80]
            out.append(f"  - [{lab}] {cid} [{tagm}] {title} — {reason} — {snip}")
        out.append("")
    return "\n".join(out)
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed（含新 3 条；现有用例不回归——`render_review` 旧测试不传 `cache_status` 时 `tag` 为空，标题仍含 question）。

- [ ] **Step 5: 实现 IO 重建 + CLI（追加/改 `scripts/mine_annotations.py`）**

追加重建函数（IO，不单测，靠集成验收）：
```python
def rebuild_review_for_case(case, conn, cfg, cache_dir, top_n, allow_llm):
    """为已标注 case 重建 review：复用候选逻辑(embed+Milvus)→候选池；
    优先按 cache_key 读 data/annotation_cache 取理由；缓存缺失标 cache_missing，
    不编造理由(spec §5-step1-5)；allow_llm 时才补调 DeepSeek。"""
    question = case["question"]
    gold_ids = resolve_gold_chunks(conn, case)
    vec = embed_text(cfg, question)
    hits = milvus_search(cfg, vec, top_n + len(gold_ids) + 10)
    pool = build_candidate_pool(hits, gold_ids, top_n)
    ctx = fetch_chunk_context(conn, pool)
    candidates = [ctx[c] for c in pool if c in ctx]
    cand_ids = [c["chunk_id"] for c in candidates]

    key = cache_key(question, cand_ids, GENERATOR_VERSION)
    resp = read_cache(cache_dir, key)
    cache_status = "hit"
    if resp is None:
        if allow_llm:
            gold_ctx = fetch_chunk_context(conn, gold_ids)
            user = build_user_message(question, build_gold_brief(case, gold_ctx), candidates)
            resp = llm_call_with_retry(cfg, SYSTEM_PROMPT, user, cand_ids)
            if resp is not None:
                write_cache(cache_dir, key, resp)
                cache_status = "llm_backfill"
            else:
                cache_status = "cache_missing"
        else:
            cache_status = "cache_missing"
    reasons = reasons_from_response(resp, cand_ids) if resp is not None else {}
    return review_entry_from_annotated(case, candidates, reasons, cache_status)
```

在 `main` 的 `argparse` 区加两个开关（在 `--config` 那行后）：
```python
    ap.add_argument("--review-only", action="store_true",
                    help="只从 --in 的标注文件重建 review，不改标注 json")
    ap.add_argument("--allow-llm", action="store_true",
                    help="review 重建时缓存缺失允许补调 DeepSeek（默认只读缓存）")
```

在 `main` 里、`cfg = load_config(...)` 之后、读 cases 之前，加 review-only 分支：
```python
    if args.review_only:
        with open(args.inp, encoding="utf-8") as f:
            cases = load_cases(f.read())
        if args.limit:
            cases = cases[:args.limit]
        conn = pg_connect(cfg["RAG_PG_CONNINFO"])
        cache_dir = "data/annotation_cache"
        reviews = []
        for case in cases:
            reviews.append(rebuild_review_for_case(case, conn, cfg, cache_dir, args.top_n, args.allow_llm))
            sys.stderr.write(f"[review] {case.get('case_id') or case['question'][:20]}: {reviews[-1]['cache_status']}\n")
        out_review = args.review or (args.inp.rsplit('.', 1)[0] + ".review.md")
        with open(out_review, "w", encoding="utf-8") as f:
            f.write(render_review(reviews))
        sys.stderr.write(f"[review] 完成 {len(reviews)} 题 → {out_review}\n")
        return
```

- [ ] **Step 6: 集成验收（真实服务，先 5 题）**

Run（确认 Milvus+PG 在线；见 [[local-services]]）：
```powershell
python scripts/mine_annotations.py --review-only --in eval/retrieval_questions_100.annotated.json --review eval/_rv5.review.md --limit 5
```
Expected：stderr 每题打 `hit`/`cache_missing`；`eval/_rv5.review.md` 有 5 个 `##` 标题，每条列出候选 + 标签；不改 `annotated.json`。
核对：`git status --short eval/retrieval_questions_100.annotated.json` 无改动。

- [ ] **Step 7: 生成完整 100 题 review + 抽检**

Run:
```powershell
python scripts/mine_annotations.py --review-only --in eval/retrieval_questions_100.annotated.json --review eval/retrieval_questions_100.review.md
```
Expected：`eval/retrieval_questions_100.review.md` 有 **100 个** `##` 标题。抽检 10 条（spec §5-step1 验收）：gold 不在候选、distractor 像但错、acceptable 仅相关补充。

- [ ] **Step 8: Commit**

```powershell
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m @'
feat(annot): full review + --review-only/--allow-llm (修 resume 漏审)

review_entry_from_annotated/label_of 纯函数 + rebuild_review_for_case(复用候选、优先读缓存、
缺失标 cache_missing 不编造、--allow-llm 才补调)。修 §4.1 resume 复用题不出 review。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```
（`eval/_rv5.review.md` 是临时产物，`eval/*.review.md` 已 gitignore，不入库。）

---

## Task 2: C++ 富排序指标纯函数 + doctest

**Files:**
- Modify: `src/eval/retrieval_metrics.h`, `src/eval/retrieval_metrics.cpp`
- Modify: `tests/test_retrieval_metrics.cpp`

- [ ] **Step 1: 在 `src/eval/retrieval_metrics.h` 声明新函数**

在 `covered_groups_at_k` 声明后加：
```cpp
#include <cmath>

// 集合命中：前 k 个候选里属于 id_set 的去重个数。纯函数。
int count_in_set_at_k(const std::vector<std::string>& cand_ids_by_rank,
                      const std::set<std::string>& id_set, int k);

// 首个落在 id_set 的 1-based 排名；无命中返回 0。纯函数。
int first_rank_in_set(const std::vector<std::string>& cand_ids_by_rank,
                      const std::set<std::string>& id_set);

// 干扰项是否排在 gold 之前：first_distractor_rank>0 且
//（first_gold_rank==0[gold 未命中] 或 first_distractor_rank<first_gold_rank）。纯函数。
bool distractor_before_gold(int first_distractor_rank, int first_gold_rank);

// nDCG@k：gains_by_rank[r]=排名 r 候选增益(去重后:组首命中2/acceptable 1/重复0)；
// achievable_gains=可达增益多重集(G 个 2 + |A| 个 1)。折扣 1/log2(rank+1)。IDCG=0 返回 0。纯函数。
double ndcg_at_k(const std::vector<double>& gains_by_rank,
                 const std::vector<double>& achievable_gains, int k);

// Redundancy@k：前 k 中"冗余证据"占"有效证据"之比。
// sig_by_rank[r]=该候选证据签名集(覆盖的必要组 "g:<i>" + 方法 "m:<no>";空=非证据)。
// 有效证据=签名非空;冗余=签名非空但未引入任何新签名元素(全被更靠前候选见过)。纯函数。
double redundancy_at_k(const std::vector<std::set<std::string>>& sig_by_rank, int k);
```

- [ ] **Step 2: 追加 doctest 到 `tests/test_retrieval_metrics.cpp`**

```cpp
TEST_CASE("count_in_set_at_k counts unique hits within k") {
    std::vector<std::string> cand = {"a", "d1", "b", "d2", "d1"};
    std::set<std::string> d = {"d1", "d2"};
    CHECK(count_in_set_at_k(cand, d, 3) == 1);   // 前3:a,d1,b → d1
    CHECK(count_in_set_at_k(cand, d, 5) == 2);   // d1,d2(d1重复不再计)
    CHECK(count_in_set_at_k(cand, d, 1) == 0);
}

TEST_CASE("first_rank_in_set returns 1-based rank or 0") {
    std::vector<std::string> cand = {"a", "b", "g1"};
    CHECK(first_rank_in_set(cand, {"g1"}) == 3);
    CHECK(first_rank_in_set(cand, {"zz"}) == 0);
}

TEST_CASE("distractor_before_gold logic") {
    CHECK(distractor_before_gold(2, 5) == true);   // 干扰更靠前
    CHECK(distractor_before_gold(5, 2) == false);  // gold 更靠前
    CHECK(distractor_before_gold(3, 0) == true);   // gold 未命中、干扰命中
    CHECK(distractor_before_gold(0, 4) == false);  // 干扰未命中
    CHECK(distractor_before_gold(0, 0) == false);
}

TEST_CASE("ndcg_at_k with dedup gains") {
    // 理想:2,1 在前;实际:组首命中(2)在1,acceptable(1)在2 → 完美
    std::vector<double> achievable = {2.0, 1.0};
    std::vector<double> perfect = {2.0, 1.0, 0.0};
    CHECK(ndcg_at_k(perfect, achievable, 3) == doctest::Approx(1.0));
    // 实际把 0 排前、2 排后 → nDCG 下降
    std::vector<double> bad = {0.0, 0.0, 2.0, 1.0};
    CHECK(ndcg_at_k(bad, achievable, 4) < 1.0);
    // 无可达增益 → 0
    CHECK(ndcg_at_k({0.0, 0.0}, {}, 2) == doctest::Approx(0.0));
}

TEST_CASE("redundancy_at_k flags evidence bringing nothing new") {
    // r0: g:0(新) r1: g:0(旧→冗余) r2: g:1(新) r3: 空(非证据,不计)
    std::vector<std::set<std::string>> sig = {{"g:0"}, {"g:0"}, {"g:1"}, {}};
    CHECK(redundancy_at_k(sig, 4) == doctest::Approx(1.0 / 3.0));  // 3有效,1冗余
    // 同方法重复且无新组 → 冗余
    std::vector<std::set<std::string>> sig2 = {{"m:T0302"}, {"m:T0302"}};
    CHECK(redundancy_at_k(sig2, 2) == doctest::Approx(0.5));
    // 无有效证据 → 0
    CHECK(redundancy_at_k({{}, {}}, 2) == doctest::Approx(0.0));
}
```

- [ ] **Step 3: 运行确认失败（编译错=未定义）**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected：链接/编译报 `count_in_set_at_k` 等未定义。

- [ ] **Step 4: 在 `src/eval/retrieval_metrics.cpp` 实现**

文件顶部确保 `#include <cmath>`、`#include <algorithm>`。追加：
```cpp
int count_in_set_at_k(const std::vector<std::string>& cand, const std::set<std::string>& s, int k) {
    std::set<std::string> seen;
    int c = 0, n = std::min<int>(k, static_cast<int>(cand.size()));
    for (int i = 0; i < n; ++i)
        if (s.count(cand[i]) && !seen.count(cand[i])) { seen.insert(cand[i]); ++c; }
    return c;
}

int first_rank_in_set(const std::vector<std::string>& cand, const std::set<std::string>& s) {
    for (size_t i = 0; i < cand.size(); ++i)
        if (s.count(cand[i])) return static_cast<int>(i) + 1;
    return 0;
}

bool distractor_before_gold(int d, int g) {
    if (d <= 0) return false;
    return g <= 0 || d < g;
}

double ndcg_at_k(const std::vector<double>& gains,
                 const std::vector<double>& achievable, int k) {
    auto dcg = [](const std::vector<double>& g, int kk) {
        double s = 0.0; int n = std::min<int>(kk, static_cast<int>(g.size()));
        for (int i = 0; i < n; ++i) s += g[i] / std::log2(static_cast<double>(i) + 2.0);
        return s;
    };
    std::vector<double> ideal = achievable;
    std::sort(ideal.begin(), ideal.end(), std::greater<double>());
    double idcg = dcg(ideal, k);
    return idcg > 0.0 ? dcg(gains, k) / idcg : 0.0;
}

double redundancy_at_k(const std::vector<std::set<std::string>>& sig, int k) {
    std::set<std::string> seen;
    int eff = 0, red = 0, n = std::min<int>(k, static_cast<int>(sig.size()));
    for (int r = 0; r < n; ++r) {
        if (sig[r].empty()) continue;
        ++eff;
        bool brings_new = false;
        for (const auto& e : sig[r]) if (!seen.count(e)) { brings_new = true; break; }
        if (!brings_new) ++red;
        for (const auto& e : sig[r]) seen.insert(e);
    }
    return eff > 0 ? static_cast<double>(red) / eff : 0.0;
}
```

- [ ] **Step 5: 构建 + 运行新 doctest + 全量**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*count_in_set*","*first_rank*","*distractor_before_gold*","*ndcg*","*redundancy*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：新用例全 PASS；全量绿（0 failed）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/retrieval_metrics.h src/eval/retrieval_metrics.cpp tests/test_retrieval_metrics.cpp
git commit -m @'
feat(eval): 富排序指标纯函数 (nDCG/Distractor/Redundancy/before-gold)

count_in_set_at_k/first_rank_in_set/distractor_before_gold/ndcg_at_k/redundancy_at_k +
doctest。离散增益(组首2/acceptable 1/重复0)、Redundancy 签名去重首版。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/eval/retrieval_metrics.h src/eval/retrieval_metrics.cpp tests/test_retrieval_metrics.cpp
```

---

## Task 3: 接入 run_rich_eval + 报告段

**Files:**
- Modify: `src/eval/eval_runner.h`, `src/eval/eval_runner.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: 扩 `RichCaseResult`（`src/eval/eval_runner.h`）**

把 `RichCaseResult` 改为：
```cpp
struct RichCaseResult {
    std::string question;
    int group_total = 0;                 // G
    std::vector<int> covered;            // 与 ks 对齐：Group Recall 覆盖组数
    int distractor_total = 0;            // |distractor_chunks|
    std::vector<int> distractor_in_k;    // 与 ks 对齐：top-k 命中干扰项数
    std::vector<double> ndcg;            // 与 ks 对齐
    std::vector<double> redundancy;      // 与 ks 对齐
    int first_distractor_rank = 0;       // 0=未命中
    int first_gold_rank = 0;             // 0=未命中（top-20 内首个覆盖任一组的排名）
};
```

- [ ] **Step 2: 在 `run_rich_eval` 计算新数据（`src/eval/eval_runner.cpp`）**

在现有 `group_keys` 构建之后、`RichCaseResult cr;` 之前，插入：
```cpp
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
```

把现有 `cr` 填充段改为（替换原 `cr.question`…`rep.results.push_back` 块）：
```cpp
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
```
确保 `eval_runner.cpp` 顶部 `#include "eval/retrieval_metrics.h"` 已存在（现已 `covered_groups_at_k` 在用，应已包含）。

- [ ] **Step 3: 扩报告段（`src/main.cpp`，`if (rich) {…}` 内）**

在现有 Group Recall/Complete 汇总 `for` 循环（`main.cpp:531-543`）之后、该 `if (rich)` 块的收尾 `}` 之前，追加这段：
```cpp
            std::cout << "\n--- 富排序指标 (--rich) ---\n";
            for (const auto& r : rr.results) {
                if (distractor_before_gold(r.first_distractor_rank, r.first_gold_rank))
                    std::cout << "[risk] distractor-before-gold  " << r.question << "\n";
                size_t i10 = 0; for (; i10 < rr.ks.size(); ++i10) if (rr.ks[i10] == 10) break;
                if (i10 < rr.ks.size() && i10 < r.redundancy.size() && r.redundancy[i10] >= 0.4)
                    std::cout << "[risk] redundancy@10=" << r.redundancy[i10] << "  " << r.question << "\n";
            }
            for (size_t i = 0; i < rr.ks.size(); ++i) {
                double ndcg_sum = 0.0, red_sum = 0.0; int n = 0;
                int dist_hit = 0, dist_den = 0; double dist_recall_sum = 0.0;
                for (const auto& r : rr.results) {
                    ndcg_sum += r.ndcg[i]; red_sum += r.redundancy[i]; ++n;
                    if (r.distractor_total > 0) {
                        ++dist_den;
                        if (r.distractor_in_k[i] > 0) ++dist_hit;
                        dist_recall_sum += static_cast<double>(r.distractor_in_k[i]) / r.distractor_total;
                    }
                }
                if (n > 0) {
                    std::cout << "nDCG@" << rr.ks[i] << ": " << (ndcg_sum / n)
                              << "   Redundancy@" << rr.ks[i] << ": " << (red_sum / n);
                    if (dist_den > 0)
                        std::cout << "   Distractor Hit@" << rr.ks[i] << ": "
                                  << (static_cast<double>(dist_hit) / dist_den)
                                  << "   Distractor@" << rr.ks[i] << ": " << (dist_recall_sum / dist_den);
                    std::cout << "\n";
                }
            }
            {   // Distractor-before-gold 汇总（分母=有 distractor 的样本）
                int den = 0, before = 0;
                for (const auto& r : rr.results) if (r.distractor_total > 0) {
                    ++den;
                    if (distractor_before_gold(r.first_distractor_rank, r.first_gold_rank)) ++before;
                }
                if (den > 0)
                    std::cout << "Distractor-before-gold: " << (static_cast<double>(before) / den)
                              << "  (有效样本 " << den << ")\n";
            }
```
> 实现者注意：上面**第一段带 `placeholder removed below` 的 3 行不要写入**，只写其后的真实代码。`main.cpp` 顶部需能见到 `distractor_before_gold` 声明——加 `#include "eval/retrieval_metrics.h"`（若未包含）。

- [ ] **Step 4: 构建 + 全量测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误；全量 doctest 绿。

- [ ] **Step 5: 集成验收（真实服务）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich 2>&1 | Select-String -Pattern "Group Recall|Complete@|nDCG@|Distractor|Redundancy|risk" | Out-String
```
Expected：
- 原 `Group Recall@k`/`Complete@k` 汇总**仍在**（base 段不回归）；
- 新增 `--- 富排序指标 (--rich) ---` 段含 `nDCG@k`/`Distractor Hit@k`/`Distractor@k`/`Redundancy@k`/`Distractor-before-gold`；
- `[risk] …` 行只在异常题出现。
记录这组数字作为 baseline。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/eval_runner.h src/eval/eval_runner.cpp src/main.cpp
git commit -m @'
feat(eval): --rich 接入富排序指标 + 报告段

run_rich_eval 复用 top-20 召回算 nDCG/Distractor/Redundancy/before-gold;
main 报告追加汇总 + 异常逐题。base Group Recall/Complete 不变。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/eval/eval_runner.h src/eval/eval_runner.cpp src/main.cpp
```

---

## Task 4: README 同步 + baseline 记录

**Files:**
- Modify: `README.md`
- Create: `docs/superpowers/specs/` 无新增（baseline 数字写进本 plan 的执行记录/或 README）

- [ ] **Step 1: 定位 README 滞后处**

```powershell
Select-String -Path README.md -Pattern "M1|进行中|walking|计划|M3|M4|roadmap|路线图|ingest|query" | Select-Object LineNumber, Line
```
读出路线图/状态段的确切行，确认把 `ingest`/`query`、M3/M4 写成"建设中/计划"的位置。

- [ ] **Step 2: 改路线图状态为真实状态**

把"`ingest`/`query` 建设中、M3/M4 计划"等改为真实状态：文本路 MVP 已成型（三路召回 dense/BM25/PG-exact + RRF + 列举直查补召回 + 方法号/条款号置顶）、M3a/M3b/M4/M4.1/M4.2/富指标已落地；后续改进由富指标 + 失败样本驱动。（按 Step 1 实际行内容做最小必要替换，不重写整篇。）

- [ ] **Step 3: 补用法命令**

在 README 用法/示例段加：
```markdown
## 评估与检索自查

```powershell
# 单查检索自查（看三路召回与排序）
rag2.exe retrievecheck "通用硅酸盐水泥安定性需要通过哪两种方法判定合格？" 20

# 富指标评估（Group Recall/Complete + nDCG/Distractor/Redundancy）
rag2.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich

# 生成侧评估（条款引用 + 数值准确率）
rag2.exe eval eval/dataset_seed.json 30 rule --gen
```

> 标注集 `eval/retrieval_questions_100.annotated.json` 由 `scripts/mine_annotations.py` 生成；
> 完整人审文件用 `python scripts/mine_annotations.py --review-only --in <annotated.json>` 重建。
```

- [ ] **Step 4: 跑 baseline 并记录**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich 2>&1 | Tee-Object -FilePath logs/eval100_rich_baseline.txt
```
把关键数字（nDCG@10、Distractor Hit@10、Distractor-before-gold、Redundancy@10、Group Recall@20、Complete@20）抄进 README 的"当前水平"一句（一行即可，不贴全表）。
（`logs/` 已 gitignore，baseline 原始输出不入库。）

- [ ] **Step 5: Commit**

```powershell
git add README.md
git commit -m @'
docs(readme): 路线图状态改真实 + 评估/自查命令 + 富指标 baseline 一行

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- README.md
```

---

## Self-Review

**1. Spec 覆盖：**
- §2 目标①完整可审 → Task 1（--review-only 全量重建）；②`--rich` 消费 distractor/acceptable → Task 3；③报告含排序风险 → Task 3 报告段；④用指标决策下一步 → baseline（Task 4）+ spec §5-step3（执行期人工查，不在本 plan 自动化）。
- §4.1 review 只 2 题 → Task 1（修 + 全量重建）。§4.2 evaluator 未吃标注 → Task 2/3。§4.3 README → Task 4。
- §5 步骤1（review 修，含 review-only/缓存优先/cache_missing/allow-llm）→ Task 1 Step 5；步骤2（5 个指标，k={1,3,5,10,20}，复用 run_rich_eval，纯函数 doctest，仅 --rich 打印）→ Task 2/3；步骤3（查两道失败）→ 执行期手动（spec 明确是人工流程，非代码任务）。
- §6 口径：6.1 chunk_id 精确匹配 → Task 3（`distractor_set`/`acceptable_set` 用 chunk_id）;6.2 nDCG gain(2/1/0) → Task 2 ndcg + Task 3 gains 构建;6.3 distractor 空不进分母 → Task 3 报告 `if (r.distractor_total>0)`;6.4 Redundancy 首版 → Task 2 redundancy_at_k（签名去重）。
- §7 报告形态（保留 Group Recall/Complete + 追加富排序段 + 异常逐题）→ Task 3 Step 3。
- §8 测试：8.1 Python（review-only 不改 json、缓存命中不调 LLM、cache_missing）→ Task 1（纯函数单测 + 集成 Step 6 验证不改 json/cache 标记）;8.2 C++ doctest（nDCG/Distractor/before-gold/Redundancy）→ Task 2;8.3 集成验收 → Task 3 Step 5 + Task 1 Step 7。
- §9 README → Task 4。

**2. Placeholder 扫描：** 无 TBD/TODO/占位；每个 code step 给完整代码与确切命令。

**3. 类型一致性：**
- 纯函数签名 `count_in_set_at_k(vector<string>,set<string>,int)`/`first_rank_in_set`/`distractor_before_gold(int,int)`/`ndcg_at_k(vector<double>,vector<double>,int)`/`redundancy_at_k(vector<set<string>>,int)` 在 Task 2 声明=实现=doctest=Task 3 调用处一致。
- `RichCaseResult` 新字段 `distractor_total/distractor_in_k/ndcg/redundancy/first_distractor_rank/first_gold_rank` 在 Task 3 Step1 定义、Step2 填充、Step3 报告读取一致。
- Python `label_of`/`review_entry_from_annotated`/`rebuild_review_for_case`/`render_review(带 cache_status)` 在 Task 1 各步一致；复用既有 `resolve_gold_chunks`/`build_candidate_pool`/`fetch_chunk_context`/`cache_key`/`read_cache`/`reasons_from_response`/`build_user_message`/`build_gold_brief`/`llm_call_with_retry`。

**4. 已知限制：** review 重建受 Milvus ANN 微漂移影响→部分题 `cache_missing`（spec §5-step1-5 允许，标注不编造）；spec §5-step3 两道失败排查是人工流程，本 plan 只产出 baseline 供其使用。
