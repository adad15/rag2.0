# 评测 EvalCase 统一 Schema 对齐设计

- 日期：2026-06-22
- 类型：评测数据契约对齐设计（M4.2 生成侧 + 检索评测集生成 两份 spec 的 schema 统一）
- 分支：V3.4
- 状态：设计已确认（2026-06-22），待编写实施计划
- 来源：
  - M4 检索评估核心 [`2026-06-17-m4-retrieval-eval-core-design.md`](2026-06-17-m4-retrieval-eval-core-design.md)（现有 `EvalCase`：`gold_method_no`/`gold_clause_no`/`gold_standard_no`/`gold_methods`）
  - M4.2 生成侧评估 [`2026-06-22-m4.2-generation-eval-design.md`](2026-06-22-m4.2-generation-eval-design.md)（已实现，加了 `gold_values`）
  - 检索评测集生成 [`2026-06-22-retrieval-eval-dataset-generation-design.md`](2026-06-22-retrieval-eval-dataset-generation-design.md)（证据组模型 `must_have_groups`/`stable_refs`/`acceptable_chunks`/`distractor_chunks` + 富指标）
- 背景：两份新 spec 同时扩展 `EvalCase` 但方向不同——M4.2 往"生成答案 gold"（数值/引用）扩，检索评测集文档往"证据组 gold"（多证据/等价/干扰）扩，且后者写作时未纳入 M4.2 的字段。两者**不矛盾、是正交两轴**，但需要一次统一，避免出现两套并行的真相源、以及采用富格式时丢失 M4.2 的生成 gold。

---

## 1. 目标

给出一个统一的 `EvalCase`，使**同一道题**可同时承载两条正交的评测轴，且向后兼容现有扁平数据：

1. **检索证据轴**：必要证据组、等价证据、可接受证据、干扰项（供现有 hit@k/MRR/coverage 与未来 Group Recall/Complete/nDCG/Distractor 消费）。
2. **生成答案轴**：数值准确率（`gold_values`）+ 引用准确率（从证据组的 `stable_refs` 派生）。

本设计只定义**数据契约 + 解析规范化 + 各 evaluator 的消费规则**，不实现富检索指标本身（那是后续独立刀）。

---

## 2. 核心洞察：两条正交 gold 轴

| 轴 | 评什么 | gold 来源 | 消费方 |
|---|---|---|---|
| 检索证据轴 | 必要证据召回/排序/干扰 | `must_have_groups` / `acceptable_chunks` / `distractor_chunks` | `run_eval`（现状指标）+ 未来富指标 |
| 生成答案轴 | 引用对不对、数值对不对 | `generation.gold_values`（数值）+ 证据组 `stable_refs`（引用） | `run_generation_eval`（M4.2 `--gen`） |

**关键简化**：M4.2 的"引用 gold"≡ 证据组的 `stable_refs`（标准号 + 方法号/条款号）。引用不另列字段，从证据组派生。仅 `gold_values` 是生成轴独有。

---

## 3. 采纳的设计决策（用户已确认推荐项）

- **决策 1（采纳）**：去掉结构体上的扁平字段，统一到证据组模型；on-disk 仍接受旧扁平写法，由 `parse_dataset` **规范化**进统一内存模型。理由：单一内存真相源最干净；旧 JSON 零改动。
- **决策 2（采纳）**：多证据题的引用命中口径＝**所有必要组的 ref 都被引用**才算 `cite_hit`（严格，对齐"完整引用"）。单组题退化为与现 M4.2 完全一致。
- **决策 3（采纳）**：`gold_values` 归位到 `generation` 子对象；`parse_dataset` **同时接受顶层旧写法**（M4.2 现有 `dataset_seed.json` 不改）。
- **决策 4（采纳）**：分阶段——本设计对应的实施是"**schema 统一 + 规范化解析 + 现有两个 evaluator 改读统一模型、指标零变化**"这一刀（纯重构，可被现有 180 单测 + `eval`/`--gen` 回归保护）。富检索指标（Group Recall/Complete/nDCG/Distractor/Redundancy）为后续独立刀。

---

## 4. 统一 JSON Schema（on-disk）

### 4.1 富格式（新生成数据用）

```json
{
  "case_id": "eval-v1-000123",
  "question": "混凝土抗压强度试验需要哪些设备，加载速度是多少？",
  "query_type": "multi_evidence",
  "difficulty": "hard",
  "answerable": true,
  "language_variant": "colloquial",
  "source_standard_ids": ["std_x"],
  "must_have_groups": [
    {
      "group_id": "equipment",
      "chunk_ids": ["chunk_eq_1", "chunk_eq_2"],
      "stable_refs": [ {"standard_no":"JTG 3420-2020","method_no":"T0521-2005","clause_no":"2"} ]
    },
    {
      "group_id": "loading_rate",
      "chunk_ids": ["chunk_lr"],
      "stable_refs": [ {"standard_no":"JTG 3420-2020","method_no":"T0521-2005","clause_no":"5.3"} ]
    }
  ],
  "acceptable_chunks": ["chunk_parent_summary"],
  "distractor_chunks": ["chunk_similar_test"],
  "generation": {
    "gold_values": ["25", "0.5"],
    "cite_required": true,
    "reference_answer": "供人工审核，不参与打分"
  },
  "provenance": {
    "generator_version": "prompt-v1",
    "validation_status": "auto_pass",
    "expert_review": "unreviewed"
  }
}
```

### 4.2 旧扁平格式（仍合法，自动规范化）

```json
{ "question":"T0521 …需要哪些仪具", "gold_method_no":"T0521-2005" }
{ "question":"…用到压力机", "gold_methods":["T0316-2024","T0350-2005"] }
{ "question":"…初凝终凝是多少", "gold_values":["45","390"] }
{ "question":"JTC 5210 第7.3.1条…", "gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1" }
```

字段缺省：所有新字段可空；`answerable` 缺省 `true`；`query_type`/`difficulty` 缺省 `unknown`。

---

## 5. 统一 C++ 内存模型

```cpp
struct StableRef {                 // 持久定位（chunk 重建后可回解析）
    std::string standard_no;       // "JTG 3420-2020"
    std::string method_no;         // "T0521-2005"（可空）
    std::string clause_no;         // "5.3"（可空）
};

struct EvidenceGroup {             // 组内等价(命中任一)，组间共同必要
    std::string group_id;
    std::vector<std::string> chunk_ids;    // 首选；空时由 stable_refs 解析
    std::vector<StableRef>  stable_refs;    // 持久锚点；引用评分也用它
};

enum class QueryType { ClauseMethodLocate, SingleFact, Procedure, Condition,
                       ParamFormulaTable, Compare, MultiEvidence, CrossClause, Unknown };
enum class Difficulty { Easy, Medium, Hard, Unknown };

struct GenerationGold {            // M4.2 生成轴
    std::vector<std::string> gold_values;   // 数值准确率（逐值）
    bool cite_required = false;             // 引用是否参与评分（解析时按 stable_refs 自动置位）
    std::string reference_answer;           // 仅人工审核
    bool empty() const { return gold_values.empty() && !cite_required; }
};

struct EvalCase {
    std::string case_id;           // 缺省由 question 归一化 hash 兜底
    std::string question;
    std::string note;

    QueryType   query_type = QueryType::Unknown;
    Difficulty  difficulty = Difficulty::Unknown;
    bool        answerable = true;
    std::string language_variant;
    std::vector<std::string> source_standard_ids;

    std::vector<EvidenceGroup> must_have_groups;   // 检索证据轴核心
    std::vector<std::string>   acceptable_chunks;
    std::vector<std::string>   distractor_chunks;

    GenerationGold generation;                      // 生成答案轴（M4.2）

    std::string generator_version, validation_status, expert_review;  // 溯源（可空）
};
```

> 旧 `gold_standard_no/gold_clause_no/gold_method_no/gold_methods/gold_values` **不在结构体上**，解析时规范化（§6）。下游只面对一种模型。

---

## 6. 规范化映射（旧扁平 → 统一）—— `parse_dataset` 负责

| 旧写法 | 规范化结果 |
|---|---|
| `gold_method_no:"T0521-2005"` | 1 个 `EvidenceGroup{stable_refs:[{method_no}]}`；`query_type=ClauseMethodLocate` |
| `gold_clause_no` + `gold_standard_no` | 1 个 `EvidenceGroup{stable_refs:[{standard_no,clause_no}]}` |
| `gold_methods:[…N]`（覆盖查） | **N 个** `EvidenceGroup`，每个 `{stable_refs:[{method_no}]}` |
| `gold_values:[…]`（顶层） | `generation.gold_values` |

**等价关系（指标向后兼容的依据）**：
- 旧 `coverage@method_no` ≡ 新 **Group Recall@k**（N 个单方法组覆盖几个）。
- 旧点查 `hit@k`/`MRR` ≡ 单组的首命中排名。

即 M4 旧指标是新模型的特例——扩展不推翻（对齐检索评测集 spec §13）。

---

## 7. 各 evaluator 消费规则（同一模型，各取所需）

- **`run_eval`（现状指标，过渡保留，指标零变化）**：从 `must_have_groups` 派生所需的 method/clause 候选序列——单组→点查 `hit@k`/`MRR`；多组(单方法)→`coverage@method_no`。读统一模型，结果与现状逐字一致（由回归保护）。
- **富检索指标（后续刀）**：直接吃 `must_have_groups`/`acceptable_chunks`/`distractor_chunks`。
- **`run_generation_eval`（M4.2 `--gen`，最小改动）**：
  - 数值：`generation.gold_values`（不变）。
  - 引用：从 `must_have_groups[].stable_refs` 派生期望引用；单组＝与现 M4.2 一致；多组＝所有必要组 ref 均被引用才 `cite_hit`（决策 2）。方法号仍取 stem 去年份。
  - 参与条件：`generation.cite_required || !generation.gold_values.empty()`。

---

## 8. 解析失败与稳健性

- 富格式里 `must_have_groups` 既无 `chunk_ids` 也无 `stable_refs` → 该组非法，整条标 `needs_review`，不计分（对齐检索评测集 spec §15）。
- `chunk_id` 失效 → 先用 `stable_refs` 回解析；无法唯一解析 → `stale_gold`，不参与评分。
- 非法 `query_type`/`difficulty` 字符串 → 落 `Unknown`，不报错。
- 顶层旧 `gold_values` 与 `generation.gold_values` 同时出现 → 以 `generation.gold_values` 为准并 warn。

---

## 9. 测试策略（本刀）

纯函数 / 解析层，全量 doctest：
- 旧四类扁平样本规范化正确（method 点查、clause 点查、coverage、gold_values）。
- 富格式解析：多组、组内等价 chunk、acceptable/distractor、generation、provenance、metadata。
- 缺省值（answerable 默认 true、query_type/difficulty 默认 Unknown）。
- 顶层 `gold_values` 与 `generation.gold_values` 冲突时取后者 + warn。
- 非法证据组 → needs_review。
- `run_eval` 在统一模型下对现 `dataset_seed.json` 跑出的 hit@k/MRR/coverage 与重构前**逐字一致**（回归）。
- `run_generation_eval` 在现 `dataset_seed.json` 上引用 5/5、数值 18/18 不变（回归）。

---

## 10. 验收

1. `parse_dataset` 同时解析旧扁平与新富格式，规范化为统一 `EvalCase`。
2. 现有 180 单测全绿 + 新增解析单测全绿。
3. `eval eval/dataset_seed.json 30 rule --gen` 的检索段与生成段数字与重构前**逐字一致**（hit@k/MRR/coverage 不变；引用 5/5、数值 18/18 不变）。
4. 下游 `run_eval`/`run_generation_eval` 不再读任何扁平字段，只读统一模型。

---

## 11. 不做（本刀边界）

- 不实现富检索指标（Group Recall/Complete/nDCG/Distractor/Redundancy）——后续独立刀。
- 不实现数据集生成流水线（Corpus Sampler … Report Writer）——检索评测集 spec 的后续。
- 不改任何检索/生成逻辑，不动 Milvus schema。
- 不改 M4.2 已冻结的指标口径（引用双含、数值逐值）。

---

## 12. 后续衔接

- **富检索指标刀**：基于本统一模型实现检索评测集 spec §9 的 Group Recall@k / Complete@k / nDCG / Distractor@k / Redundancy@k + 分层报告。
- **数据集生成刀**：检索评测集 spec §7/§14 的离线生成 + 校验 + 抽检流水线，产出富格式数据集。
