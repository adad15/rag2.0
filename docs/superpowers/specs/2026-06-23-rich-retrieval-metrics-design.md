# 富检索指标（Group Recall@k + Complete@k）第一刀设计

- 日期：2026-06-23
- 类型：检索评估扩展 spec（富指标第一刀；只测量，不改检索逻辑）
- 分支：V3.4
- 状态：设计已确认（2026-06-23 brainstorm），待编写实施计划
- 来源：
  - 检索评测集生成设计 [`2026-06-22-retrieval-eval-dataset-generation-design.md`](2026-06-22-retrieval-eval-dataset-generation-design.md) §9（富指标全集）/§9.8（k 集）
  - EvalCase 统一 schema [`2026-06-22-eval-schema-unification-design.md`](2026-06-22-eval-schema-unification-design.md)（已实现：`must_have_groups`/`StableRef` 证据组模型）
- 背景：schema 统一后，现有 seed 的覆盖题已规范化为多证据组（压力机=22 组、万能=14、马歇尔=9），点查=1 组。因此 **Group Recall@k / Complete@k 现在就能在现有 seed 上跑出意义**（它们是 `coverage@method_no` 的推广）。Distractor@k/Redundancy@k 需要 `distractor_chunks` 标注（seed 没有），故本刀不做。

---

## 1. 目标

给 `rag2 eval` 加 opt-in 的 `--rich`：从一次 top-20 召回派生 **Group Recall@k** 与 **Complete@k**（k∈{1,3,5,10,20}），按证据组衡量"必要证据召回了多少 / 是否全齐"。纯测量，不改检索逻辑，现有 hit@k/MRR/coverage 报告不变。

---

## 2. 范围（已与用户确认）

**做：**
- `rag2 eval <dataset> [k] [planner] [--rich]`，`--rich` opt-in。
- Group Recall@k + Complete@k，固定 k∈{1,3,5,10,20}（§9.8）。
- 跑现有 seed 的证据组模型；无需新标注。
- 追加一个富指标报告段；现有 hit@k/MRR/coverage 段不动。

**不做（YAGNI / 留后续）：**
- nDCG@k、Distractor@k、Redundancy@k（§9.4-9.6；后两者需 `distractor_chunks` 标注）。
- §10 分层报告（按 query_type/difficulty/standard…）与失败清单/失败类型。
- §7 数据集生成流水线。
- 不可回答负例评估（§9.7）。
- 不改检索/生成逻辑、不动 Milvus schema。

---

## 3. 核心决策（已确认）

- **决策 1（采纳）**：第一刀只做 Group Recall@k + Complete@k + 多 k 报告，跑现有 seed。
- **决策 2（采纳）**：`--rich` opt-in 开关，追加报告段；默认不跑、现有报告零改动（与 `--gen` 同构）。
- **决策 3（采纳）**：富指标用**独立 top-20 召回**（`per_path_k=80, top_k=20`），与 base 段各自召回——保证 base 的 hit@k/coverage 数字完全不受影响。`--rich` 时每条多一次召回，eval 规模（~18 条）可忽略。
- **决策 4**：k 集固定 {1,3,5,10,20}。富指标数字是**全新口径**（k≤20），不与旧 `coverage@30` 比较，无"逐字一致"约束。

---

## 4. 指标定义

设某用例有 `G = must_have_groups.size()` 个必要证据组，top-k 候选覆盖其中 `g` 个。

### 4.1 组覆盖判定（covered）

一个证据组 `grp` 在 top-k 内被覆盖，当且仅当存在排名 `< k` 的候选 `c` 与 `grp` 匹配。匹配 = 满足任一：
- `c.chunk_id ∈ grp.chunk_ids`（富格式直接给 chunk）；
- ∃ `grp.stable_refs` 中的 `r`：`r.method_no` 非空且 `c.method_no == r.method_no`；
- ∃ `r`：`r.clause_no` 非空且 `c` 的 `standard_id|clause_no == resolve(r.standard_no)|r.clause_no`（`resolve` = `pg.find_standard_by_code(strip_spaces(standard_no))`，与现 `run_eval` 条款匹配同口径）。

“组内等价、命中任一即覆盖”由此自然满足。

### 4.2 Group Recall@k 与 Complete@k

- `Group Recall@k = g / G`（G=0 的用例不计入富指标）。单证据组退化为 hit@k。
- `Complete@k = 1` 当且仅当 `g == G`，否则 0。数据集层取均值（= 完整率）。

---

## 5. 组件（小而专）

### 5.1 纯指标：`src/eval/retrieval_metrics.{h,cpp}`（扩，全量 TDD）

infra 层把"组的可接受标识集"和"候选按排名的标识集"都构造成带前缀的字符串集合，纯函数只做集合重叠判定：

```cpp
#include <set>
// 前 k 个候选覆盖了多少个组。group_keys[i] = 第 i 组的可接受标识集；
// cand_keys_by_rank[r] = 排名 r 候选的标识集（含 "cid:<id>" / "m:<method_no>" / "c:<sid>|<clause>"）。
// 组被覆盖 = 它的标识集与某个 rank<k 候选的标识集有交集。
int covered_groups_at_k(const std::vector<std::set<std::string>>& group_keys,
                        const std::vector<std::set<std::string>>& cand_keys_by_rank,
                        int k);
```
`Group Recall@k = covered/G`、`Complete@k = (covered==G)` 由调用方按 `G=group_keys.size()` 平凡推导（不再单列函数，DRY）。

### 5.2 富指标报告类型 + 收集：`run_eval`（infra）

`src/eval/eval_runner.h` 加：
```cpp
struct RichCaseResult {
    std::string question;
    int group_total = 0;                 // G
    std::vector<int> covered;            // 与 ks 对齐：每个 k 的覆盖组数
};
struct RichReport {
    std::vector<int> ks;                 // {1,3,5,10,20}
    std::vector<RichCaseResult> results; // 仅含 must_have_groups 非空的用例
};
```
`run_eval` 加一个 `bool rich` 参数（或并行的 `run_rich_eval`，实施计划定）。`rich` 为真时：对每条 `must_have_groups` 非空的用例独立做 top-20 召回 → 回查候选 `chunk_id/method_no/(sid|clause)` → 为每组建可接受标识集（§4.1，`standard_no` 经 `find_standard_by_code` 解析）→ 对每个 k∈ks 调 `covered_groups_at_k` → 填 `RichReport`。

### 5.3 报告打印：`main.cpp`

`--- 富检索指标 (--rich) ---`：
- 每条紧凑一行（头条 k=20）：`[grp] G=<G> recall@20=<g>/<G> complete@20=<0|1>  <question>`
- 汇总（遍历 ks）：`Group Recall@k: <均值>   Complete@k: <完整率>`（每个 k 一行）

### 5.4 接线：`main.cpp` `cmd_eval`

解析 `--rich`（与 `--gen` 并列，任意位置）；`rich` 时在现有报告段后调富指标收集并打印。`--rich` 与 `--gen` 可同时给。

---

## 6. 数据流（每条 --rich 用例）

```
question
  ├─（base 段：现有 run_eval 单 k，照旧、不受影响）
  └─（--rich）独立 top-20 召回 → 候选标识(cid/method/sid|clause)
        + 各组可接受标识集(§4.1)
        → covered_groups_at_k 在 k∈{1,3,5,10,20}
        → RichReport
```

---

## 7. 报告形态（示例）

```
[base 段：point hit@30 / MRR / coverage@30 —— 不变]

--- 富检索指标 (--rich) ---
[grp] G=22 recall@20=20/22 complete@20=0  公路工程试验中哪些试验用到压力机
[grp] G=1  recall@20=1/1   complete@20=1  T0521 …需要哪些仪具
...
Group Recall@1: 0.42   Complete@1: 0.20
Group Recall@3: ...
Group Recall@5: ...
Group Recall@10: ...
Group Recall@20: 0.88   Complete@20: 0.55
```
（具体数字以实跑为准；上为格式示例。）

---

## 8. 错误处理

- 用例 `must_have_groups` 为空（纯数值题/无检索 gold）→ 不计入富指标（跳过，不进 `RichReport.results`）。
- 候选 `pg.get_chunk` 回查失败 → 该候选标识集为空（不匹配任何组），沿用现有稳健做法。
- `stable_ref.standard_no` 解析不到 standard_id → 该 clause 维度不参与匹配（与现 run_eval 条款匹配同口径）。
- 富格式组既无 chunk_ids 也无可解析 stable_refs → 该组可接受集为空，永不被覆盖（Complete 会因此判 0，符合“证据缺失”语义）。

---

## 9. 测试策略

- **纯指标 `covered_groups_at_k`**：全量 doctest——单组命中/未命中、多组部分覆盖、组内等价（多标识任一命中）、k 截断（命中在 k 之后不算）、空组集、候选不足 k。
- **`run_eval --rich` 收集层**：infra-bound（Milvus/PG），不单测，靠 `rag2 eval eval/dataset_seed.json 30 rule --rich` 实跑验收 + 人工核对几条（压力机 G=22、点查 G=1、马歇尔 Complete@20=0）。

---

## 10. 验收

1. 全量单测绿（含 `covered_groups_at_k` 新用例）。
2. `eval … --rich` 打印富指标段（每条 + k 集汇总），不带 `--rich` 时输出与现状逐字一致（base 段零回归）。
3. 现有 base 段（hit@k/MRR/coverage）数字在加了 `--rich` 后仍与不加时一致（独立召回，互不影响）。
4. 人工核对：点查 Complete@20=1；某覆盖题 G 与其 gold 方法数一致；Group Recall@k 随 k 单调不降。
5. `--rich` 与 `--gen` 可同时使用，各段互不干扰。

---

## 11. 不做（本刀边界）

- nDCG@k / Distractor@k / Redundancy@k。
- 分层报告、失败清单、失败类型分类。
- 数据集生成与 distractor/acceptable 标注。
- 改检索逻辑、改 base 段口径、动 Milvus schema。

---

## 12. 后续衔接

- **Distractor@k + Redundancy@k 刀**：需先给数据集标 `distractor_chunks`（§9.5/§9.6）。
- **nDCG@k 刀**：需 `acceptable_chunks` 标注 + 相关性分级 + 等价去重增益（§9.4）。
- **分层报告 + 失败清单**（§10）：在富指标稳定后加诊断维度。
- **已知缺口（spawn_task `task_5761ac77`）**：`derive_legacy_view` 对富格式多组覆盖（含 clause-only 组）会少算——本刀的 `covered_groups_at_k` 直接吃证据组、**不经** `derive_legacy_view`，故富指标路不受该缺口影响；该缺口仍需在动 base 段或载入富数据前处理。
