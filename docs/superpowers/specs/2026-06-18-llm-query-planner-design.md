# LLM 查询计划（第 1 档主动式检索）设计

- 日期：2026-06-18
- 类型：增强设计 spec
- 分支：V3.3
- 关联诊断：[`2026-06-16-verbose-query-recall-failure-design.md`](./2026-06-16-verbose-query-recall-failure-design.md)
- 关联方案：[`2026-06-16-ragflow-lite-query-plan-design.md`](./2026-06-16-ragflow-lite-query-plan-design.md)（本刀显式翻掉其"不做 LLM 在线改写"非目标，理由见 §2）

---

## 1. 背景

`build_query_plan` 现在靠三张手工词表 + 源码硬编码驱动查询理解：

- `config/query_terms.txt`（stopword / background / section / **instrument**）
- `config/synonyms.txt`（BM25 同义词扩展）
- `config/user_dict.txt`（jieba 用户词典）
- [`query_analysis.cpp:78`](../../../src/query/query_analysis.cpp:78) 硬编码默认章节 `{"仪具","材料"}` 与改写模板

两个问题叠加：

1. **维护跑步机**。每接一本新规范，三张表都要手工过一遍；覆盖率永远等于人工勤快程度。
2. **白名单天花板**。[`build_query_plan`](../../../src/query/query_analysis.cpp) 的列举改写**只在命中 `[instrument]` 白名单时触发**。"哪些试验用到温度计"能 work（在表里），"哪些试验用到拌和锅"不在表里就静默退回 verbose-query 老毛病（含答案的 chunk 进不了 top-k，召回全是泛总则）。

决策：引 LLM 进查询回路，让模型直接读问题、抽判别词、判意图，替掉词表的模糊匹配那一半。这是"主动式检索"分档里的**第 1 档**——只动查询理解，下游召回不变。第 2 档（迭代召回）按需再做，第 3 档（完整 agent）不做。

### 1.1 为什么只做第 1 档

| 档位 | 内容 | 取舍 |
|---|---|---|
| 第 1 档（本刀） | LLM 查询理解替规则表，下游召回原样 | 解维护跑步机 + 半个 verbose-query 病；一次调用，下游仍确定 |
| 第 2 档（预留） | 上面加迭代召回（查→判覆盖→改词再查） | 专治列举"答不全"；每问多 1~3 次调用 |
| 第 3 档（不做） | 规划 + 工具调用 + 自我纠错 | 与本系统溯源/确定性命门正面冲突 |

第 1 档即可解掉主诉（维护过重），性价比最高，且每一步都能用 M4 评估集证伪。

---

## 2. 目标与非目标

### 目标

1. 干掉 instrument/synonym/stopword 词表的模糊匹配维护负担——LLM 抽判别词不需要任何白名单，泛化天生。
2. 修复白名单未覆盖的列举类查询（"哪些试验用到拌和锅"）召回失败。
3. 保住三道确定性闸：磁盘缓存、固定 prompt + temp0、规则保底（见 §6）。
4. 全程可用 M4 评估集量化 LLM vs 规则，作为是否默认启用的硬闸。

### 非目标

1. 不做迭代召回（第 2 档），只留挂点（§9）。
2. 不做文档侧 enrichment（important_kwd / question_tks）——那是更深的表示侧一刀，另立项。
3. 不删三张词表文件——它们继续喂规则保底路。
4. 不动编号抽取：标准号/条款号/方法号继续走正则，永不交给 LLM。
5. 不改 Milvus schema、不改 chunk 入库结构。

### 显式承认的局限

本刀只补查询侧。"拌和锅"在那条长 chunk 里仍然低频——**表示侧的病还在**，LLM 抽得再准也只能把背景词剔干净来补偿，鲁棒性弱于文档侧加权。文档侧 enrichment 仍是后续价值最高的一刀。

---

## 3. 架构与改动面

只在 `build_query_plan` 这一个函数后面动刀。下游 [`text_retrieve`](../../../src/retrieve/text_search.cpp) 的多路召回 / RRF / keyterm 直查 / 条款置顶**一行不改**——它拿到的还是同一个 `QueryAnalysis`，不关心字段是谁填的。

```text
question
  ├─ analyze_query()              正则抽 standard/clause/method（不动，永远确定）
  └─ build_query_plan() 变身调度器：
        ├─ llm_fill_plan()        新：调 DeepSeek 填模糊字段
        │     失败 ↓
        └─ rule_fill_plan()       现有词表逻辑，降级为保底
```

`QueryAnalysis` 结构体不变（已有 `intent/key_terms/section_hints/sparse_text/dense_text`），LLM 与规则填的是同一组字段。

---

## 4. 三个组件

- **`analyze_query`**（不动）：正则抽 `standard_code/clause_no/method_no`，填 `clean_text`。编号是精确量，继续走正则。
- **`rule_fill_plan(qa, question, terms)`**（重构，非改逻辑）：把现在 `build_query_plan` 里那段词表匹配 + 改写逻辑原样抽出成独立函数，地位从主力降为保底。三张词表文件继续喂它。
- **`llm_fill_plan(qa, question, chat)`**（新）：调 DeepSeek 填 `intent/key_terms/section_hints/sparse_text/dense_text`。

### 4.1 调度器优先级

抽到 `clause_no`/`method_no` 时，intent 由编号决定（`ClauseLookup > MethodLookup`），**LLM 不能推翻**——守住 ragflow-lite spec 既定优先级。只有无编号时才采信 LLM 判的 `ListByCondition` / `GeneralFact`。`standard_code` 收窄逻辑不变（仍由 `text_retrieve` 查 PG 做 filter）。

---

## 5. LLM 契约

固定 system prompt + 2~3 个 few-shot（含"天平"反面例、一个 ClauseLookup、一个普通事实问），要求严格 JSON 输出，`temperature=0`：

```json
{
  "intent": "ListByCondition",
  "key_terms": ["天平"],
  "section_hints": ["仪具", "材料"],
  "sparse_text": "天平 仪具 材料",
  "dense_text": "查找试验方法中使用天平的仪具与材料"
}
```

- `intent` 取四枚举之一，非法值 → 当作失败走保底。
- 解析用 nlohmann/json，任何 schema 不符即失败。

**区分"失败"与"有效但极简"**：

- LLM 判 `GeneralFact` 且不给改写（`sparse_text`/`dense_text` 空）是**合法计划**，不算失败——下游回退 `clean_text` 本就是既有行为。这种情况**不**踢回规则路，因为模型是有意判定"这是普通问题，原句直接查"。
- 只有真正失败（超时 / HTTP 错 / JSON 解析失败 / schema 非法 / intent 非法）才走保底。
- 边界一例：LLM 判 `ListByCondition` 却给出空 `key_terms`，视为失败（自相矛盾）走保底。

---

## 6. 三道确定性闸

1. **磁盘缓存**：按归一化问题（trim / 折叠空白 / 统一大小写）做键，缓存 JSON 产物，复用 [`parse_cache`](../../../src/parse/parse_cache.h) 的磁盘范式，落 `data/query_plan_cache/`。同问第二次零调用、结果恒定，且支持离线复跑评估。缓存键含 `prompt_version`——prompt 改了键跟着失效。
2. **固定 prompt + temp0**：`prompt_version` 常量写进源码并记日志。
3. **规则保底**：见 §7。

---

## 7. 错误处理与回退

LLM 真正失败时——超时（5s）、HTTP 错、JSON 解析失败、schema 非法、intent 非法、`ListByCondition` 却空 `key_terms`——**一律 `spdlog::warn` + 退回 `rule_fill_plan`**（"有效但极简"的判定见 §5，不在此列）。编号字段由 `analyze_query` 独立产出，任何情况都不丢。最坏退化成今天的行为，绝不更差。

**模式开关**：`RAG_QUERY_PLANNER = rule | llm | auto`，默认 `auto`（LLM 主、规则保底）。`rule` 强制走老路（回归对照），`llm` 强制走 LLM（失败仍保底，但便于隔离观察）。

---

## 8. 测试与评估闸

### 8.1 测试细缝

`llm_fill_plan` 接受注入的 `std::function<std::string(const std::string& system, const std::string& user)>`（最轻方案，无需给 `DeepSeekClient` 加虚函数）。生产传 `DeepSeekClient::chat` 的包装；单测传 lambda 喂假 JSON。

### 8.2 单元测试（doctest，纯函数）

1. 合法 JSON → 字段正确填入 `QueryAnalysis`。
2. 残缺 / 垃圾 JSON → 触发保底（断言走了 `rule_fill_plan`）。
3. 缓存命中 → 不调用注入函数（用计数 lambda 断言零调用）。
4. 抽到 clause_no/method_no 时编号覆盖 LLM intent。
5. 归一化键：大小写/空白不同的同义问题命中同一缓存。

### 8.3 评估闸（验收标准）

`rag2 eval` 在 M4 数据集跑 `planner=rule` vs `planner=llm` 两遍，对比 recall@k / 答全度量：

- **不回归**：verbose-query spec §2.3 那组短查询 + 普通事实问答指标不下降。
- **有提升**："哪些试验用到天平"整句的召回里出现多个不同 method_no 的"2 仪具与材料"含天平 chunk。
- 把"天平"整句案例加进评估集（若还没有）。

**没过这道闸，不准把默认模式设为 `llm`/`auto`。**

---

## 9. 第 2 档预留挂点

第 2 档（迭代召回）的挂点：`text_retrieve` 首轮召回后，对 `ListByCondition` 加一个覆盖度判断（够不够 / 跑没跑偏）→ 不足则改词再查一轮。本刀**只在文档标注挂点位置，不实现**。

---

## 10. 性能

单问多一次 DeepSeek 调用（temp0，输出 ~100 token），缓存命中后为零。工程师面向工具，单问多 1~2s 可接受。
