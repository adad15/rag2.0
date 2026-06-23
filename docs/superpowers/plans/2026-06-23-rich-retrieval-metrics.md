# 富检索指标第一刀（Group Recall@k + Complete@k）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 `rag2 eval` 加 opt-in `--rich`：从一次独立 top-20 召回派生 Group Recall@k 与 Complete@k（k∈{1,3,5,10,20}），建在统一证据组模型上，base 段（hit@k/MRR/coverage）零影响。

**Architecture:** 纯指标 `covered_groups_at_k`（集合重叠判覆盖，全量 TDD）+ 独立编排函数 `run_rich_eval`（自带 top-20 召回、构造组/候选标识集、infra-bound）+ `main.cpp` 接 `--rich` 开关追加报告段。`run_eval` 完全不动——富指标用独立召回，base 数字逐字不变。无新文件、无 vcxproj 改动。

**Tech Stack:** C++20、MSBuild + vcpkg、doctest、spdlog。沿用 [[rag2-build-setup]]。

**Scope:** 见 spec [`2026-06-23-rich-retrieval-metrics-design.md`](../specs/2026-06-23-rich-retrieval-metrics-design.md)。只做 Group Recall@k + Complete@k + 多 k 报告；**不**做 nDCG/Distractor/Redundancy/分层报告/数据集生成；不改检索逻辑、不动 base 段。分支 V3.4。

**构建/测试（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
当前基线：192 单测绿。Task 3 实跑需 Milvus/PG 在线（先 `docker ps --filter name=milvus-standalone` 确认 healthy，否则 `docker start milvus-etcd milvus-minio; docker start milvus-standalone`）。

---

## Task 1: 纯指标 `covered_groups_at_k`（全量 TDD）

**Files:**
- Modify: `src/eval/retrieval_metrics.h`, `src/eval/retrieval_metrics.cpp`
- Test: `tests/test_retrieval_metrics.cpp`

- [ ] **Step 1: 在 `src/eval/retrieval_metrics.h` 加声明**

文件顶部 `#include <vector>` 后加 `#include <set>`。在 `covered_count` 声明之后加：
```cpp
// 富指标核心：前 k 个候选覆盖了多少个证据组。
// group_keys[i] = 第 i 组的"可接受标识集"；cand_keys_by_rank[r] = 排名 r 候选的标识集
//（标识形如 "cid:<chunk_id>" / "m:<method_no>" / "c:<standard_id>|<clause_no>"）。
// 组被覆盖 = 它的标识集与某个 rank<k 的候选标识集有交集。空标识集的组永不被覆盖。纯函数。
int covered_groups_at_k(const std::vector<std::set<std::string>>& group_keys,
                        const std::vector<std::set<std::string>>& cand_keys_by_rank,
                        int k);
```

- [ ] **Step 2: 写失败测试到 `tests/test_retrieval_metrics.cpp`**

文件顶部已 `#include "eval/retrieval_metrics.h"`。追加 `#include <set>`（在现有 include 后）并加用例：
```cpp
TEST_CASE("covered_groups_at_k: single group covered by a matching candidate, respects k") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T9"}, {"m:T1"}, {"m:T2"}};
    CHECK(covered_groups_at_k(groups, cands, 3) == 1);
    CHECK(covered_groups_at_k(groups, cands, 1) == 0);   // T1 在 rank2，k=1 截断
}

TEST_CASE("covered_groups_at_k: equivalence within a group (any key hits)") {
    std::vector<std::set<std::string>> groups = {{"cid:c1", "m:T1"}};   // 组内等价标识
    std::vector<std::set<std::string>> cands = {{"cid:c1"}};            // 命中其一即覆盖
    CHECK(covered_groups_at_k(groups, cands, 1) == 1);
}

TEST_CASE("covered_groups_at_k: multi-group partial coverage") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}, {"m:T2"}, {"c:S|5.3"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}, {"c:S|5.3"}};
    CHECK(covered_groups_at_k(groups, cands, 2) == 2);   // T1、clause 命中；T2 没有
    CHECK(covered_groups_at_k(groups, cands, 10) == 2);
}

TEST_CASE("covered_groups_at_k: empty group key-set is never covered") {
    std::vector<std::set<std::string>> groups = {{}, {"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}, {"m:T2"}};
    CHECK(covered_groups_at_k(groups, cands, 10) == 1);   // 空组不算，T1 组算
}

TEST_CASE("covered_groups_at_k: k beyond candidate count clamps, no overflow") {
    std::vector<std::set<std::string>> groups = {{"m:T1"}};
    std::vector<std::set<std::string>> cands = {{"m:T1"}};
    CHECK(covered_groups_at_k(groups, cands, 20) == 1);
    CHECK(covered_groups_at_k({}, cands, 20) == 0);        // 空组集 → 0
}
```

- [ ] **Step 3: 运行测试，确认失败**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected：链接失败（`covered_groups_at_k` 未定义）。

- [ ] **Step 4: 在 `src/eval/retrieval_metrics.cpp` 实现**

顶部 include 区加 `#include <algorithm>` 与 `#include <set>`。在文件末尾追加：
```cpp
int covered_groups_at_k(const std::vector<std::set<std::string>>& group_keys,
                        const std::vector<std::set<std::string>>& cand_keys_by_rank,
                        int k) {
    int kk = std::min(static_cast<int>(cand_keys_by_rank.size()), std::max(0, k));
    int covered = 0;
    for (const auto& gk : group_keys) {
        bool hit = false;
        for (int r = 0; r < kk && !hit; ++r)
            for (const auto& key : cand_keys_by_rank[r])
                if (gk.count(key)) { hit = true; break; }
        if (hit) ++covered;
    }
    return covered;
}
```

- [ ] **Step 5: 运行测试，确认通过 + 全量**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*covered_groups_at_k*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：`covered_groups_at_k` 系列全 PASS；全量绿（197 用例，0 failed——本任务 +5）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/retrieval_metrics.h src/eval/retrieval_metrics.cpp tests/test_retrieval_metrics.cpp
git commit -F - <<'EOF'
feat(eval): covered_groups_at_k 纯指标 (Group Recall/Complete 核心)

按"可接受标识集"集合重叠判证据组覆盖(组内等价命中任一即覆盖、空组不算、k 截断)，纯函数全量单测。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 2: 编排 `run_rich_eval`（独立 top-20 召回，build-only）

**Files:**
- Modify: `src/eval/eval_runner.h`, `src/eval/eval_runner.cpp`

- [ ] **Step 1: 在 `src/eval/eval_runner.h` 加报告类型 + 函数声明**

在 `EvalReport` 结构体之后、`run_eval` 声明之前（或之后均可）加：
```cpp
// 富检索指标（--rich）：每条用例在 ks 各 k 上覆盖的证据组数。
struct RichCaseResult {
    std::string question;
    int group_total = 0;                 // G = must_have_groups.size()
    std::vector<int> covered;            // 与 RichReport::ks 对齐：每个 k 的覆盖组数
};
struct RichReport {
    std::vector<int> ks;                 // {1,3,5,10,20}
    std::vector<RichCaseResult> results; // 仅含 must_have_groups 非空的用例
};

// 独立做 top-20 召回，按证据组算 Group Recall@k/Complete@k 的覆盖数据。
// 与 run_eval 互不影响（各自召回）。infra-bound：需 Milvus/PG/embedding 在线。
RichReport run_rich_eval(const std::vector<EvalCase>& cases,
                         milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                         const SynonymDict& syn, const std::string& collection,
                         const QueryPlanner& planner);
```

- [ ] **Step 2: 在 `src/eval/eval_runner.cpp` 实现 `run_rich_eval`**

顶部 include 区加 `#include <set>`（`retrieval_metrics.h`/`text_search.h` 已包含）。在文件末尾（`run_eval` 定义之后）追加：
```cpp
RichReport run_rich_eval(const std::vector<EvalCase>& cases,
                         milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                         const SynonymDict& syn, const std::string& collection,
                         const QueryPlanner& planner) {
    RichReport rep;
    rep.ks = {1, 3, 5, 10, 20};
    for (const auto& c : cases) {
        if (c.must_have_groups.empty()) continue;   // 纯数值题/无检索 gold → 不计入

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
                }
            }
            group_keys.push_back(std::move(gk));
        }

        RichCaseResult cr;
        cr.question = c.question;
        cr.group_total = static_cast<int>(group_keys.size());
        for (int kk : rep.ks)
            cr.covered.push_back(covered_groups_at_k(group_keys, cand_keys, kk));
        rep.results.push_back(std::move(cr));
    }
    return rep;
}
```
（`strip_spaces` 是本文件匿名命名空间里已有的助手；`covered_groups_at_k` 来自 `eval/retrieval_metrics.h`；`find_standard_by_code`/`get_chunk` 是 `PgClient` 既有方法。`run_eval` 不改一行。）

- [ ] **Step 3: 构建（build-only，无新单测）+ 全量测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译/链接错误；全量绿（197，0 failed——本任务不加单测）。

- [ ] **Step 4: Commit**

```powershell
git add src/eval/eval_runner.h src/eval/eval_runner.cpp
git commit -F - <<'EOF'
feat(eval): run_rich_eval 独立 top-20 召回 + 证据组覆盖收集

RichReport：每条用例在 k∈{1,3,5,10,20} 的覆盖组数；自带召回、不动 run_eval；
组/候选标识集(cid/method/sid|clause)喂 covered_groups_at_k。infra-bound。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 3: `main.cpp` 接 `--rich` + 报告段（实跑验收）

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: `cmd_eval` 加 `bool rich` 参数 + 打印富指标段**

`cmd_eval` 签名（现 `static int cmd_eval(const Config& cfg, const std::string& dataset_path, int k, const std::string& planner_mode = "", bool gen = false)`）末尾加 `, bool rich = false`：
```cpp
static int cmd_eval(const Config& cfg, const std::string& dataset_path, int k,
                    const std::string& planner_mode = "", bool gen = false, bool rich = false) {
```
在 `if (gen) { ... }` 块**之后**、`return 0;` 之前加：
```cpp
        if (rich) {
            spdlog::info("[eval] rich metrics on");
            RichReport rr = run_rich_eval(cases, mv, embed, pg, syn, cfg.milvus_collection, planner);
            std::cout << "\n--- 富检索指标 (--rich) ---\n";
            const int last = static_cast<int>(rr.ks.size()) - 1;
            for (const auto& r : rr.results) {
                int g_last = (last >= 0 && last < static_cast<int>(r.covered.size())) ? r.covered[last] : 0;
                std::cout << "[grp] G=" << r.group_total
                          << " recall@" << rr.ks[last] << "=" << g_last << "/" << r.group_total
                          << " complete@" << rr.ks[last] << "=" << (g_last == r.group_total ? 1 : 0)
                          << "  " << r.question << "\n";
            }
            for (size_t i = 0; i < rr.ks.size(); ++i) {
                double recall_sum = 0.0; int complete = 0, n = 0;
                for (const auto& r : rr.results) {
                    if (r.group_total == 0) continue;
                    recall_sum += static_cast<double>(r.covered[i]) / r.group_total;
                    if (r.covered[i] == r.group_total) ++complete;
                    ++n;
                }
                if (n > 0)
                    std::cout << "Group Recall@" << rr.ks[i] << ": " << (recall_sum / n)
                              << "   Complete@" << rr.ks[i] << ": "
                              << (static_cast<double>(complete) / n) << "\n";
            }
        }
```
（`RichReport`/`run_rich_eval` 来自 `eval/eval_runner.h`，`main.cpp` 已 include 它。`mv/embed/pg/syn/planner`/`cfg.milvus_collection` 都在 `cmd_eval` 作用域内。）

- [ ] **Step 2: 派发块解析 `--rich`**

把 `if (cmd == "eval") { ... }` 块替换为：
```cpp
    if (cmd == "eval") {
        bool gen = false, rich = false;
        std::vector<std::string> pos;       // "eval" 之后的位置参数（剔除 --gen/--rich）
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--gen") gen = true;
            else if (a == "--rich") rich = true;
            else pos.push_back(a);
        }
        if (pos.empty()) {
            std::cout << "usage: rag2 eval <dataset.json> [k] [rule|llm|auto] [--gen] [--rich]\n";
            return 1;
        }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        int k = (pos.size() >= 2) ? std::max(1, std::atoi(pos[1].c_str())) : 20;
        std::string pmode = (pos.size() >= 3) ? pos[2] : "";
        return cmd_eval(cfg, pos[0], k, pmode, gen, rich);
    }
```

- [ ] **Step 3: 构建 + 全量测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误；全量绿（197，0 failed）。

- [ ] **Step 4: 实跑——base 段零回归（不带 --rich）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule 2>&1 | Select-String -Pattern "point|MRR|coverage@" | Out-String
```
Expected（与基线逐字一致）：点查 hit@30 5/5、MRR 1、coverage 22/22 14/14 5/9；**无**富指标段。

- [ ] **Step 5: 实跑——`--rich` 段（需 Milvus/PG 在线）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule --rich 2>&1 | Select-String -Pattern "grp\]|Group Recall@|Complete@|point|MRR|coverage@" | Out-String
```
Expected：
- base 段仍 5/5 / MRR 1 / coverage 22/22 14/14 5/9（独立召回，互不影响）。
- 出现「--- 富检索指标 (--rich) ---」；点查 3 条（T0521/T0702/T0316）`G=1 … complete@20=1`；压力机 `G=22`、万能 `G=14`、马歇尔 `G=9`；汇总打印 Group Recall@{1,3,5,10,20} 与 Complete@{1,3,5,10,20}。
人工核对：① 点查 Complete@20=1；② 覆盖题 G 与其 gold 方法数一致（压力机 22、万能 14、马歇尔 9）；③ Group Recall@k 随 k 单调不降。

- [ ] **Step 6: 实跑——`--gen --rich` 共存（可选确认）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule --gen --rich 2>&1 | Select-String -Pattern "引用准确率|数值准确率|Group Recall@20|Complete@20" | Out-String
```
Expected：生成侧段（引用 5/5、数值 18/18）与富指标段都出现、互不干扰。

- [ ] **Step 7: Commit**

```powershell
git add src/main.cpp
git commit -F - <<'EOF'
feat(eval): wire --rich into cmd_eval (富检索指标报告段)

eval 加 --rich(任意位置, 可与 --gen 共存) opt-in；调 run_rich_eval 打印每条 G/recall@20/complete@20
+ k∈{1,3,5,10,20} 的 Group Recall/Complete 汇总；base 段独立召回零影响。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Self-Review

**1. Spec 覆盖（逐节对照 `2026-06-23-rich-retrieval-metrics-design.md`）：**
- §2 范围（--rich opt-in、Group Recall+Complete、k∈{1,3,5,10,20}、追加段、base 不动）→ Task 1/2/3。
- §3 决策（第一刀只两指标、--rich 开关、独立 top-20 召回、固定 k 集）→ Task 2（独立召回 per_path_k=80/top_k=20、ks={1,3,5,10,20}）+ Task 3（开关）。
- §4.1 组覆盖判定（chunk_id / method_no / sid|clause 任一命中、resolve standard_no）→ Task 2 组/候选标识集构造 + Task 1 集合重叠。§4.2 Group Recall/Complete → Task 3 汇总推导（covered/G、covered==G）。
- §5 组件（retrieval_metrics 纯函数、run_rich_eval、报告、main 接线）→ Task 1/2/3。§6 数据流、§7 报告形态 → Task 3 打印。
- §8 错误处理（空 must_have_groups 跳过、get_chunk 失败候选空集、standard 解析不到、空组永不覆盖）→ Task 2（continue/空集）+ Task 1（空组不算）。
- §9 测试（纯指标 TDD、--rich infra-bound 实跑）→ Task 1 单测 + Task 3 Step 4-6。§10 验收 → Task 3 各 Expected。

**2. Placeholder 扫描：** 无 TBD；纯函数、run_rich_eval、main 接线、报告打印均给完整代码与确切命令。报告示例数字标注"以实跑为准"。

**3. 类型一致性：** `covered_groups_at_k(vector<set<string>>, vector<set<string>>, int)`(Task1) ↔ Task2 调用一致；`RichCaseResult{question,group_total,covered}`/`RichReport{ks,results}`(Task2 头) ↔ Task3 打印字段一致；`run_rich_eval(cases,mv,embed,pg,syn,collection,planner)` 签名(Task2) ↔ Task3 调用实参一致；复用既有 `text_retrieve`/`pg.get_chunk`/`pg.find_standard_by_code`/`strip_spaces`/`cfg.milvus_collection`。

**4. 已知限制（实现者须知）：**
- 富指标固定 top-20 召回，k 报到 20；旧 coverage@30 是 k=30 口径，两者不可比（spec §3 决策 4）——富指标数字全新，无"逐字一致"约束，只验 base 段不变。
- 富指标只吃 `must_have_groups`，**不经** `derive_legacy_view`，故不受 spawn_task `task_5761ac77` 缺口影响。
- `--rich` 每条多一次召回（独立于 base），eval 规模可忽略；这是换 base 零影响的代价（spec §3 决策 3）。
