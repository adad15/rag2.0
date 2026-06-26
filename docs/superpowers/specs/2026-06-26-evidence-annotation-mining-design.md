# 证据标注挖掘工具（distractor / acceptable）设计

- 日期：2026-06-26
- 类型：检索评测数据增强工具（离线、一次性、造数据，不算指标、不改检索）
- 分支：V3.4
- 状态：设计已确认（2026-06-26 brainstorm），待编写实施计划
- 来源：
  - 检索评测集生成设计 [`2026-06-22-retrieval-eval-dataset-generation-design.md`](2026-06-22-retrieval-eval-dataset-generation-design.md) §6（样本格式）/§7.6（干扰项发现）/§14（逻辑组件）/§15（可恢复性）
  - 富检索指标第一刀 [`2026-06-23-rich-retrieval-metrics-design.md`](2026-06-23-rich-retrieval-metrics-design.md) §12（后续衔接：Distractor/nDCG 需先标注）
  - EvalCase 统一 schema [`2026-06-22-eval-schema-unification-design.md`](2026-06-22-eval-schema-unification-design.md)（`distractor_chunks`/`acceptable_chunks`/`must_have_groups`/`StableRef` 已在 schema 中）

---

## 1. 背景

富指标第一刀（Group Recall@k + Complete@k）已落地。后续两把指标刀——**Distractor@k / Redundancy@k**（需 `distractor_chunks` 标注）与 **nDCG@k**（需 `acceptable_chunks` 标注 + 相关性分级）——都卡在**标注缺失**上：

- `eval/dataset_seed.json`、`eval/retrieval_questions_100.json`、`eval/retrieval_questions_1000.json` 三个评测集里，`distractor_chunks` 与 `acceptable_chunks` **一条标注都没有**。
- 评测集的 gold 是**方法号/条款号级**（`StableRef`），而 `distractor_chunks` / `acceptable_chunks` 是 **chunk_id 级**。

spec（生成设计 §9.5）原话：“没有标注干扰项的样本不进入该指标分母。”因此指标刀若先上，跑出来全是空分母、无法在真实数据上验证。**本工具就是来生产这些标注的**：它是“数据先行”路线的第一刀，产出带标注的数据集，为后续 Distractor/nDCG 指标刀供真实数据。

本工具是生成设计 §14“Distractor Miner + Evidence Pack”两个组件的**聚焦切片**——只给**现有**问题挖标注，**不生成新题**（新题生成是 §7 完整流水线，本刀不做）。

---

## 2. 目标

给 `eval/retrieval_questions_100.json` 的每道题，自动产出两类 chunk_id 标注，写入一份**新文件**（原始集不动）：

- `distractor_chunks`：与问题/正确答案高度相似、但对象/版本/条件/结论错误的“像但错”chunk（检索器易误排到前面的陷阱）。
- `acceptable_chunks`：相关且有帮助、但非回答必需的 chunk（如父条款概述、等价表格）。

挖掘方式 = **embedding 候选 + LLM 复核**（生成设计 §7.6 推荐）。**只造数据，不算任何指标、不改检索/生成逻辑、不动 Milvus schema。**

---

## 3. 范围（已与用户确认）

**做：**
- 独立 **Python 脚本** `scripts/mine_annotations.py`（spec 生成设计 §14 明确允许生成器用非 C++）。
- 输入现有 `eval/retrieval_questions_100.json`（全 100 题），输出 `eval/retrieval_questions_100.annotated.json`。
- 候选 = 问题 embedding 的 **dense ANN 近邻**（绕开 BM25/RRF/planner 整条线，避免继承检索器偏差），默认 `top_n=30`（可 `--top-n` 调）。
- LLM（DeepSeek）逐题复核候选 → distractor / acceptable / irrelevant。
- 磁盘缓存 + temp0 + 增量续跑（可恢复）。
- `--review` 人审导出（markdown），供抽检。

**不做（YAGNI / 留后续）：**
- 不算 Distractor@k / Redundancy@k / nDCG@k（那是后续指标刀）。
- 不生成新题（§7 完整流水线：Corpus Sampler / Question Generator / Variant / Curator）。
- 不做拒答负例（`answerable=false`）。
- 不做第二模型交叉复核（生成设计 §7.5 的独立校验器；首刀只 auto 标注 + 人审导出）。
- 不改 C++ 检索/生成、不动 Milvus schema、不改 eval 评测器。
- 不标注 seed / 1000 集（首刀只 100 集；同脚本可后续复用到别的集，但本刀验收只针对 100）。

---

## 4. 核心决策（已确认）

- **决策 1（采纳）**：第一刀 = 给现有 100 题挖 `distractor_chunks` + `acceptable_chunks` 标注，产出新数据集；**不生成新题、不算指标**。
- **决策 2（采纳）**：挖掘 = embedding 候选 + LLM 复核（非纯规则、非纯人工）。
- **决策 3（采纳）**：用独立 Python 脚本（psycopg2 连 PG，`requests` 打 embedding/DeepSeek HTTP，Milvus 走 REST 或 pymilvus）。
- **决策 4（采纳）**：候选锚点 = **问题 embedding 的近邻**（非 gold-chunk 近邻）。理由：Distractor@k 关心“对这道查询，像但错的 chunk 会不会挤进 top-k”，问题锚点直接对准该 population。
- **决策 5（采纳）**：候选语料范围 = **全库**（不按 standard 过滤）。理由：生成设计 §7.6 把“同条款号但属另一规范”“同方法旧版本”列为重要干扰来源，跨标准“像但错”最有价值。
- **决策 6（采纳）**：首刀只 auto 标注（`validation_status:"auto"`、`expert_review:"unreviewed"`）+ 人审导出；不做二次模型复核。

---

## 5. 架构与数据流

单文件脚本，内部拆成小而专的纯函数 + 明确的 IO 边界（PG / Milvus / embedding / DeepSeek / 文件）。

```
config.json ──► load_config()                      # 端点/密钥/conninfo
eval/...100.json ──► load_cases()                  # 读题（统一 schema + legacy 扁平兼容）

for each case（带断点续跑）:
  resolve_gold_chunks(case, pg)                    # gold 方法号/条款号 → chunk_id 集（排除自身用）
        │
  embed_text(question) ──► milvus_search(clause_text, top_n)   # dense ANN 候选
        │
  build_candidate_pool(hits, gold_ids, n)          # 排除 gold 及其等价、截断 top-N      ← 纯函数
        │
  fetch_chunk_context(cand_ids, pg)                # title / method_no / clause_no / 正文片段
        │
  llm_classify(question, gold_brief, cands)        # DeepSeek 一次调用（temp0，缓存，重试）
        │
  parse_llm_labels(resp_json, cand_ids)            # → {distractor:[], acceptable:[]}     ← 纯函数
        │
  assemble_annotated_case(case, labels, meta)      # 写回字段 + provenance                ← 纯函数
        ▼
  增量写 out.json（已标注题跳过；中断可续）
（可选）write_review_md(results) ──► --review out.md
```

---

## 6. 组件（小而专）

| 组件 | 类型 | 输入 | 输出 | 职责 |
|---|---|---|---|---|
| `load_config` | IO | config.json | dict | 读 PG conninfo / Milvus / embedding / DeepSeek 端点与密钥 |
| `load_cases` | 纯 | json 文本 | list[case] | 解析评测集；兼容 `must_have_groups` 与 legacy 扁平字段 |
| `resolve_gold_chunks` | IO(PG) | case | set[chunk_id] | gold 方法号(按 stem 前缀)/条款号 → 该标准下全部 chunk_id |
| `embed_text` | IO(HTTP) | question | vector[4096] | SiliconFlow `/v1/embeddings`，请求体 `{model,input}` |
| `milvus_search` | IO | vector | list[(chunk_id,standard_id,score)] | `clause_text` 集 dense ANN，top_n |
| `build_candidate_pool` | 纯 | hits, gold_ids, n | list[chunk_id] | 排除 gold 及其等价、去重、截断 top-N |
| `fetch_chunk_context` | IO(PG) | cand_ids | list[chunk_ctx] | 取 title / method_no / clause_no / 正文片段(截断) |
| `llm_classify` | IO(HTTP)+缓存 | question, gold_brief, cands | json 文本 | DeepSeek 一次调用打标签；磁盘缓存 + temp0 + 重试 |
| `parse_llm_labels` | 纯 | resp_json, cand_ids | {distractor,acceptable} | 解析标签；丢弃越界 id / 非法标签；失败→空 |
| `assemble_annotated_case` | 纯 | case, labels, meta | case' | 写回两字段 + `generation` 溯源块 |
| `write_review_md` | IO | results | review.md | 每题：问题/gold/各候选标签+理由，供人审 |

embedding 用 config 的同模型/端点，请求体 `{model, input:text}`，与入库同路径、**无 query/doc 前缀差异**（见 C++ `build_embedding_request_body`，仅 `model`+`input`），故查询向量与库内 chunk 向量同空间。

### 6.1 gold 解析细节

- 方法号：`SELECT chunk_id FROM retrieval_chunks WHERE method_no LIKE '<stem>%' AND standard_id = ANY(<source_standard_ids>)`，`<stem>` = gold 方法号去年份（如 `T0301-2024`→`T0301`），与 C++ `chunks_by_method` 同口径（按 stem 前缀取该方法全部版本 chunk，全部计入“gold 等价”一并排除）。
- 条款号：`... WHERE clause_no = '<clause>' AND standard_id = ANY(<source_standard_ids>)`，与 `chunk_ids_by_clause` 同口径。
- gold 维度来源：合并 `must_have_groups[].stable_refs[]`（method_no/clause_no）与 legacy 扁平 `gold_method_no` / `gold_clause_no` / `gold_methods`。
- `source_standard_ids` 缺失时退回 `stable_ref.standard_no` 经 `standards` 表归一匹配（与 C++ `find_standard_by_code` 同口径）；首刀 100 集均带 `source_standard_ids`，此分支为兜底。

---

## 7. LLM 判定契约

单题一次 DeepSeek 调用（temp0）。

**输入（user message）**：问题 + gold 简述（gold 方法号/条款号 + 标题）+ 编号候选表，每条含：序号、chunk_id、method_no/clause_no、title、正文片段（截断 ~300 字）。

**system prompt 判据**（固定，改它须同步改版本常量 `GENERATOR_VERSION`，见 §8）：
- `distractor`：主题/标题/仪器与正确答案高度相似，但**试验对象 / 版本 / 适用条件 / 结论错误**——会诱导检索器误判的“像但错”。
- `acceptable`：与问题相关、对理解有帮助，但**不是回答必需**（如父条款概述、等价表格、背景说明）。
- `irrelevant`：与问题无实质关系 → 丢弃。
- gold 本身（正确答案 chunk）**不应**出现在候选里（已在 §6 排除）；若 LLM 仍判某候选等同 gold，应标 `irrelevant`（不进任何标注），不得标 distractor。

**输出（严格 JSON）**：`[{"chunk_id":"...","label":"distractor|acceptable|irrelevant","reason":"..."}]`，无解释、无代码块围栏。

**解析稳健**：非数组 / 解析失败 → 视为该题标注失败（见 §8）；越界 chunk_id（不在候选集）→ 丢该条；非法 label → 丢该条。

---

## 8. 确定性、可恢复性与错误处理（生成设计 §15）

- **版本常量** `GENERATOR_VERSION`（首版 `"annot-v1"`）：贯穿缓存键、跳过判断与溯源块（§9.1 `generation.generator_version`）。改 prompt/判据须同步改它。
- **磁盘缓存**：LLM 响应缓存到 `data/annotation_cache/`，键 = `normalize(question) + 候选 id 集 + GENERATOR_VERSION`。同题（含候选集不变）重跑零调用。
- **增量写**：输出文件已含某题的标注（且 `generator_version` 匹配）则跳过；中断后可续，不重复调用。
- **LLM 非法 JSON**：固定次数重试（如 2 次）；仍失败 → 该题标 `generation_error`，`distractor_chunks`/`acceptable_chunks` 留空，**不静默修补**。
- **候选 chunk PG 回查失败**：该候选不进上下文（不参与判定），稳健跳过。
- **gold 解析为空**（库里查不到该方法/条款）：记录告警，仍对该题挖候选（gold 排除集为空），LLM 判定照常。
- **外部服务（PG/Milvus/embedding/DeepSeek）连不上**：整次运行报错退出，不产出半成品文件（避免误导性部分标注）。

---

## 9. 产物格式

### 9.1 标注数据集 `eval/retrieval_questions_100.annotated.json`

原 case 原样保留，补两字段 + 溯源块：

```json
{
  "case_id": "rq-001",
  "question": "...",
  "gold_method_no": "T0301-2024",
  "source_standard_ids": ["12104388589027850934"],
  "distractor_chunks": ["<chunk_id>", "..."],
  "acceptable_chunks": ["<chunk_id>", "..."],
  "generation": {
    "generator_version": "annot-v1",
    "annotation_source": "embedding+llm",
    "validation_status": "auto",
    "expert_review": "unreviewed",
    "candidate_top_n": 30
  }
}
```

`distractor_chunks` / `acceptable_chunks` 字段名与 C++ `EvalCase`（`parse_dataset`）一致，后续指标刀可直接 `parse_dataset` 读入。

### 9.2 人审导出 `--review out.md`（可选）

每题一段：问题、gold（方法号/条款号+标题）、候选清单（每条：label、method_no/clause_no、title、reason、正文片段），供你抽检 distractor 是否确实“像但错”、acceptable 是否确实“相关非必需”、gold 是否被误标。

---

## 10. 测试策略

- **纯函数 pytest**（无网络，mock LLM/embedding 响应）：
  - `load_cases`：兼容 `must_have_groups` 与 legacy 扁平四种 gold 输入。
  - `resolve_gold_chunks`：方法号 stem 前缀、条款号、合并多来源（用 mock PG 游标）。
  - `build_candidate_pool`：排除 gold/等价、去重、top-N 截断、候选不足 N。
  - `parse_llm_labels`：合法多标签、非法 JSON、越界 chunk_id、非法 label、空数组、自相矛盾（gold 被标 distractor 应被 §7 规则化为 irrelevant）。
  - `assemble_annotated_case`：两字段写回 + 溯源块 + 原字段不丢。
- **集成验收**（真实服务）：对 5~10 题实跑，人工核对 `--review` 导出几条；确认缓存命中（重跑零 LLM 调用）、增量续跑（删一题输出后重跑只补该题）。
- 注：Python 工具，独立于 C++ doctest；引入轻量 pytest（`scripts/tests/` 或 `scripts/test_mine_annotations.py`）。

---

## 11. 验收标准

1. `python scripts/mine_annotations.py --in eval/retrieval_questions_100.json --out eval/retrieval_questions_100.annotated.json` 跑通，产出合法 JSON。
2. 输出每题含 `distractor_chunks` / `acceptable_chunks`（可为空数组）+ `generation` 溯源块；原字段不丢。
3. 输出可被 C++ `parse_dataset` 无错读入（字段名/类型对齐）。
4. 候选不含 gold 及其等价 chunk（同方法各版本、同条款）。
5. 同题重跑零 LLM 调用（缓存命中）；删除输出中某题后重跑只补该题（增量续跑）。
6. `--review` 导出可读 markdown；人工抽检 5~10 条：distractor 确为“像但错”、acceptable 确为“相关非必需”、gold 未被误标，错误率可接受（首刀目标 ≤ 20%，否则回头调 prompt/候选数）。
7. pytest 纯函数用例全绿。
8. 外部服务故障时不产出半成品标注文件。

---

## 12. 后续衔接

- **Distractor@k / Redundancy@k 指标刀**：消费本刀产出的 `distractor_chunks`，纯函数 + 接入 `eval --rich`（Distractor@k / Distractor Hit@k / Distractor-before-gold / Redundancy@k）。
- **nDCG@k 指标刀**：消费 `acceptable_chunks` + 必要证据组，离散增益（必要组首命中 2、acceptable 首命中 1、同组等价重复 0）+ 等价去重。
- **专家抽检 / 二次模型复核**：在标注稳定后加（生成设计 §7.5/§8）；首刀只人审导出。
- **复用到 seed / 1000 集**：同脚本换 `--in/--out` 即可，本刀验收只覆盖 100 集。
- **§7 完整出题流水线**：生成全新问题（非本刀），跨多 spec。

---

## 13. 不做（本刀边界）

- 不算任何检索指标。
- 不生成新题、不做拒答负例。
- 不改 C++ 检索/生成、不动 Milvus schema、不改 eval 评测器。
- 不做第二模型交叉复核、不冻结 dev/test/challenge 划分。
- 不让当前检索器（BM25/RRF/planner 整条线）参与候选产生——只用裸 dense ANN。
