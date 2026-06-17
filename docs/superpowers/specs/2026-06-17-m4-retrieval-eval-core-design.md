# M4 检索评估核心设计

- 日期：2026-06-17
- 类型：子项目 spec（M4 第一刀：检索侧评估闭环 + 指标 + `rag2 eval` 命令）
- 来源：原始 [M4 评估闭环计划](../plans/2026-05-31-m4-evaluation-loop.md)（CMake/C++17 时代，本刀更新为 MSBuild/C++20 并切片）、查询计划 spec [§9.3 评测闭环](2026-06-16-ragflow-lite-query-plan-design.md)、2026-06-16 讨论（查询计划 Phase 1 落地后"该继续 Phase 2 还是先做 M4"的决策）
- 分支：V3.2
- 状态：设计已选定；实现计划已就绪（[`2026-06-17-m4-retrieval-eval-core.md`](../plans/2026-06-17-m4-retrieval-eval-core.md)），待执行
- 背景：查询计划 Phase 1 修好了"哪些试验用到天平"的召回（V3.2），但**所有后续检索调参都没有度量**：weighted RRF 的权重、rerank 规则、覆盖阈值，改了不知道整体变好还是变坏。查询计划 spec §11 第一条就是"先搭评测集"，§9.3 写明与 M4 衔接。一个具体警示：Phase 1 实测 top-20 含 20 个不同试验的"仪具与材料"chunk 看着不错，但用真实 gold 一比，其中 T0557/T0556/T0558 等列的是压力机/万能机、**并不含天平**——没有评测集就发现不了这种"看着对、其实掺了假阳性"。

---

## 1. 一句话目标

建立可重复运行的**检索评估核心**：受版本管理的评估集 JSON + 纯检索指标（点查 `hit@k`/`MRR`、列举查 `coverage@method_no`）+ `rag2 eval <dataset.json> [k]` 命令，复用现有 `text_retrieve`，把"召回好不好、改动有没有提升"从肉眼变成有据可依。**只测量，不改检索逻辑。**

---

## 2. 核心设计决策（已与用户确认）

### 2.1 先做检索评估核心，生成侧留后续（M4 切片）

原 M4 计划一刀含检索指标 + 生成指标 + 数值后置校验 + 拒答阈值标定。本刀只切**检索侧**，因为它是当前最高杠杆、且是查询计划 Phase 2（加权/重排/覆盖）能否调参的**前置门**。生成侧（条款引用准确率、拒答正确性、数值准确率、数值幻觉硬拦截 §11.4、阈值标定）原样保留为 M4 后半段，另立计划。

**为什么先 M4 而非查询计划 Phase 2**：Phase 1 已灭火（无紧迫 bug）；Phase 2 的 §6 权重/规则离开度量就是盲调；M4 一次投入可衡量整条链路（查询计划、M5、未来 reranker、BGE-M3），且会先告诉你 Phase 1 到底够不够好。

### 2.2 评估用 C++，复用 text_retrieve（不重造检索）

评估运行器直接调现有 `text_retrieve(question, mv, embed, pg, syn, collection, per_path_k, top_k)`，对返回候选算指标。否决用 Python 另写一套：会重复实现检索管道、与真实 query 行为漂移。代价是评估需 Milvus/PG/embedding 在线（与 `retrievecheck` 同款前置）。

### 2.3 两类 gold：点查 vs 覆盖查

这是与原 M4（只有点查 gold）的关键扩展，因为"哪些试验用到天平"这类**列举/聚合题**的理想答案是**一组**试验、不是单条 clause：

- **点查**：`gold_clause_no`（配 `gold_standard_no`）或 `gold_method_no` 任一非空 → 衡量 gold 是否进 top-k、排第几。
- **覆盖查**：`gold_methods`（一组 method_no）非空 → 衡量召回覆盖到这组的多少（`coverage@method_no`）。

### 2.4 gold 对齐方式：回查候选元数据

候选是 `chunk_id`。运行器对每个候选 `pg.get_chunk(chunk_id) → RetrievalChunkRow`，取 `method_no` 与 `(standard_id|clause_no)`：

- 覆盖查：候选 `method_no` 序列 ∩ `gold_methods` 的去重计数。
- 点查（方法号）：候选 `method_no` 序列里首个等于 `gold_method_no` 的排名。
- 点查（条款号）：`gold_standard_no` 去空格经 `pg.find_standard_by_code` → `standard_id`，gold_key = `standard_id|gold_clause_no`，与候选 `standard_id|clause_no` 序列比对。

### 2.5 指标集：克制（YAGNI）

只做 `hit@k`、`MRR`（点查）+ `coverage@method_no`（覆盖查）。**纯函数**（`first_hit_rank`/`reciprocal_rank`/`hit_at_k`/`covered_count`），与 I/O 分离，可 doctest 单测。不做 nDCG/MAP 等——本阶段用不上，需要时再加。

### 2.6 种子集用真实 gold，扩充留领域

天平覆盖用例的 `gold_methods` 是从 `data/chunk_cache/5797633264338373449.json` 导出的"仪具/材料章节含天平"的 **23 个真实方法号**（非编造）；另两条方法点查用真实方法号。沿用原 M4 §15.5 哲学：计划交付**格式 + 可跑骨架（3 条）**，扩到 ~100 条由领域人员按流程标注，**不代写领域金标**。

### 2.7 沿用 MSBuild/C++20，不重建索引、不改检索

原 M4 计划写于 CMake/C++17 时代，本刀更新为 MSBuild + vcpkg / C++20（见 [[rag2-build-setup]]）。评估只读不写，不动 Milvus schema、不改 `text_retrieve`/查询计划任何逻辑。

---

## 3. 评估集 schema（`eval/dataset_seed.json`）

JSON 对象数组，每条：

| 字段 | 含义 |
|---|---|
| `question` | 查询问题（必填） |
| `note` | 备注/题型（可空） |
| `gold_standard_no` | 标准号文本，如 `"JTG 3420"`（点查·条款用；可空） |
| `gold_clause_no` | 条款号，如 `"5.1.2"`（点查·条款用；可空） |
| `gold_method_no` | 方法号，如 `"T0521-2005"`（点查·方法用；可空） |
| `gold_methods` | 一组 method_no（覆盖查用；空=非覆盖查） |

规则：`gold_methods` 非空 → 覆盖查；否则 `gold_method_no` 或 `gold_clause_no` 非空 → 点查；都空 → 不计分（仅观察）。

---

## 4. 指标定义

- **`first_hit_rank(candidate_keys, gold_key)`**：首个命中的 1-based 排名，无命中 0。
- **`hit@k`**：`rank ∈ [1,k]`。
- **`MRR`**：点查样本 `Σ (1/rank) / 点查样本数`（miss 记 0）。
- **`coverage@method_no`**：`覆盖到的 gold method 去重数 / |gold_methods|`。

汇总报告：点查 `hit@k`（命中/总数）+ `MRR`；每条覆盖查 `coverage@k`（百分比 + N/total）。

---

## 5. 组件

| 文件 | 职责 | 性质 |
|---|---|---|
| `src/eval/dataset.{h,cpp}` | `EvalCase` + `parse_dataset(json)` | 纯，可单测 |
| `src/eval/retrieval_metrics.{h,cpp}` | 四个指标纯函数 | 纯，可单测 |
| `src/eval/eval_runner.{h,cpp}` | `run_eval`：跑 text_retrieve→回查→算指标→汇总 | 集成（需 Milvus/PG），不单测，实测验收 |
| `src/main.cpp` | `eval` 子命令 | — |
| `eval/dataset_seed.json` | 种子集（天平覆盖 + 2 方法点查） | 真实 gold |

详见实现计划 [`2026-06-17-m4-retrieval-eval-core.md`](../plans/2026-06-17-m4-retrieval-eval-core.md)。

---

## 6. 不做（本刀边界）

- 生成侧指标：条款引用准确率、拒答正确性、数值准确率。
- 数值后置校验硬拦截（原 M4 §11.4）与拒答阈值标定。
- nDCG/MAP 等高级指标。
- 自动扩充评估集 / 难例回流。
- 改任何检索逻辑（query plan、RRF、retriever）——本刀只测量。
- 评估集扩到 ~100（领域人员后续按 §15.5 流程做）。

---

## 7. 验收标准

1. `parse_dataset` 正确解析点查与覆盖查两类 gold；非数组抛错；空数组返回空。
2. 四个指标纯函数 doctest 覆盖（含去重、miss、空输入）。
3. `rag2 eval eval/dataset_seed.json 20` 打印每条 per-case（point hit@N / coverage N/23）+ 汇总（hit@k、MRR、coverage@k）。
4. 产出 Phase 1 在"天平"列举题上的 **coverage baseline 数字**（预计 ~50%）。
5. 全量 doctest 绿，无回归；不重建 collection、不改检索逻辑。

---

## 8. 后续衔接

- **M4 后半段**（生成侧）：条款引用/拒答/数值准确率指标 + 数值幻觉硬拦截 + 阈值标定。
- **用 baseline 驱动查询计划 Phase 2**：若 coverage 偏低，做 spec §6.3（`method_no` 去重 + 覆盖）+ §6.1（加权 RRF）+ §6.2（intent rerank），每改一步用本评估集回归。
- **扩充评估集**到 ~100 条（四题型 + 难例），让指标有统计意义。
