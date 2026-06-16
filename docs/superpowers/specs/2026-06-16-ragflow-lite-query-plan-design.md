# RAGFlow-lite 查询计划与意图化召回方案

- 日期：2026-06-16
- 类型：修复设计 spec（方案 B）
- 分支：V3.1
- 状态：设计已选定（含评测闭环、列举覆盖、文档侧权衡、机制限制等讨论结论）；等待确认后进入 implementation plan
- 关联诊断：[`2026-06-16-verbose-query-recall-failure-design.md`](./2026-06-16-verbose-query-recall-failure-design.md)

---

## 1. 背景

当前检索链路把用户原句同时送入 dense 和 BM25：

```text
question -> analyze_query.clean_text
         -> DenseRetriever(clean_text)
         -> Bm25Retriever(clean_text)
         -> RRF
```

这会让“公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平”这类长问失败。真正决定答案的是“天平”，但“公路、工程、水泥、混凝土、试验、规程、哪些、用到、了”也进入检索。BM25 被满库领域词污染，dense 被整句语义拉向“总则/目的适用范围”，两路一起跑偏。

RAGFlow 等成熟库的做法不是把原句直接交给全文检索，而是在**查询侧**做 term weight、停用词处理、同义词和字段/信号加权，在**文档侧**抽重要关键词与合成问题、做多字段加权，再把 full-text 与 dense 结合，并在 rerank 阶段继续利用标题、重要词、问题词等信号。本项目不直接搬 RAGFlow 的多字段索引，而是实现一个轻量版（只覆盖查询侧那一片，文档侧另立项，见 §13）：

```text
用户原句
-> QueryAnalysis 生成 QueryPlan
-> dense 使用 dense_text
-> BM25 使用 sparse_text
-> existing exact/method/clause recall
-> weighted RRF
-> intent-aware rerank（含列举覆盖）
```

---

## 2. 目标

1. 修复啰嗦自然语言列举问题导致的召回失败，尤其是“哪些试验用到了天平”这类问题，并保证**答全**（覆盖到尽量多的相关试验，而非只召回到几条）。
2. 保留现有标准号、条款号、方法号的精确抽取和置顶逻辑。
3. 不重建 Milvus collection，不引入训练型稀疏模型，不依赖 LLM 在线改写。
4. 让查询侧产物可观察，后续能在日志或 `retrievecheck` 中看到 `intent / sparse_text / dense_text / key_terms / section_hints`。
5. 短查询与普通事实问答不回归——并用最小评测集（§9.3）把“不回归”从肉眼判断变成有度量的回归闸。

---

## 3. 非目标

1. 不在本阶段实现 RAGFlow full 版的多字段索引，例如 `important_kwd`、`question_tks`、`title_tks`。
2. 不在本阶段接入 SPLADE、DeepImpact、COIL 等学习型稀疏检索。
3. 不做 LLM query rewrite、HyDE 或多 query expansion。
4. 不改变 chunk 入库结构和 Milvus schema。
5. 不把 reranker 作为召回失败的主要补救手段；rerank 只处理已召回候选的排序。

**关于非目标 1/4 的权衡（须显式承认）**：RAGFlow 的鲁棒性主要来自**文档侧 enrichment**——入库时给每个 chunk 抽 `important_kwd`、生成 `question_tks`，并对 title/关键词做多字段加权匹配。本方案把全部担子压在查询侧规则，换取“零重建、零 schema 改动”。代价是：文档表示仍是原始正文，含天平的长 chunk 里“天平”仍低频，只能靠查询侧把背景词剔干净来补偿，鲁棒性弱于文档侧加权。文档侧 enrichment 是价值最高的下一刀（需改 schema + 重灌），列入 §13，本刀不做。

---

## 4. 核心设计

### 4.1 QueryAnalysis 从“抽编号”升级为“查询计划”

当前 `QueryAnalysis` 只有：

```cpp
clean_text
standard_code
clause_no
method_no
```

方案 B 扩展为：

```cpp
enum class QueryIntent {
    GeneralFact,
    ClauseLookup,
    MethodLookup,
    ListByCondition
};

struct QueryAnalysis {
    std::string original_text;
    std::string clean_text;
    std::string dense_text;
    std::string sparse_text;
    std::vector<std::string> key_terms;
    std::vector<std::string> section_hints;
    QueryIntent intent = QueryIntent::GeneralFact;

    std::string standard_code;
    std::string clause_no;
    std::string method_no;
};
```

兼容策略：

- `original_text` 保存原始问题。
- `clean_text` 第一阶段仍保留，默认等于原始问题，用于兼容旧测试和旧调用。
- `dense_text` 给 dense retriever 使用。
- `sparse_text` 给 BM25 retriever 使用。
- 当新分析失败或产物为空时，`dense_text` 和 `sparse_text` 回退到 `clean_text`。

### 4.2 意图识别

先实现规则型意图识别：

| 意图 | 触发信号 | 检索倾向 |
|---|---|---|
| `ClauseLookup` | 抽到 `clause_no` | 条款精确置顶优先 |
| `MethodLookup` | 抽到 `method_no` | 方法号精确召回优先 |
| `ListByCondition` | 明确列举词 `哪些/有哪些/哪几项/哪几种`（`用到/需要/包含/使用/采用/涉及` 仅作弱信号，须与列举词共现） | BM25 权重提高，按条件词和章节提示重排 + 覆盖处理 |
| `GeneralFact` | 其他问题 | dense 与 BM25 均衡 |

优先级：

```text
ClauseLookup > MethodLookup > ListByCondition > GeneralFact
```

如果同一个问题既有方法号又问“需要哪些仪具”，例如“T0302 需要哪些仪具”，意图记为 `MethodLookup`，同时保留 `section_hints=["仪具"]`，因为方法号精确召回应优先。

触发词收紧的原因：`使用/采用/包含` 是大量普通事实问句也会用的动词，若单独触发 `ListByCondition` 会对本该 `GeneralFact` 的问题施加激进清洗，造成回归。故只让明确列举词触发，其余动词降为弱信号。

### 4.3 词类与权重

查询词先按规则分成四类：

| 类型 | 示例 | 处理 |
|---|---|---|
| 核心条件词 | `天平`、`烘箱`、`压力机`、`坍落度筒` | 进入 `key_terms` 和 `sparse_text` |
| 章节提示词 | `仪具`、`材料`、`设备`、`器具`、`试剂`、`步骤`、`结果` | 进入 `section_hints` 和 `sparse_text` |
| 领域背景词 | `公路`、`工程`、`水泥`、`混凝土`、`试验`、`规程` | 一般不进入列举类 BM25 主查询 |
| 句式停用词 | `哪些`、`什么`、`怎么`、`中`、`的`、`了`、`用到` | 不进入 BM25 主查询 |

这样能稳定命中“天平”这类短实体，也能避免把满库背景词送入 BM25。

**两个必须在实现计划里定死的机制问题：**

1. **`key_terms` 的抽取机制**。两种可选、失败模式不同，须明确选一：
   - *白名单实体词典*（维护一份仪器/材料词表）：能稳定命中已知词，但漏开放类——未登录仪器（如“马歇尔稳定度仪”）抽不到 → 产物空 → 回退原句 → 静默退回 bug。
   - *负向剔除残留*（原句减去 背景词 ∪ 停用词 ∪ 章节词，剩下算 `key_terms`）：开放性好，但依赖三张表的完整度。
   - 推荐：以负向剔除为主、白名单实体词典为辅（命中白名单的子串强制保留），兼顾覆盖与稳定。

2. **不要纯子串删除，要在真分词 token 上做**。库里真有仪器“试验筛”（T0502）；若按背景词“试验”做子串删除会变成“筛”，毁掉真关键词。同理“水泥净浆搅拌机”含“水泥”。故词类切分应在真分词结果上做：复用 Milvus `run_analyzer`（与索引同一份 jieba + `config/user_dict.txt`，保证查询/索引一致），或客户端引 cppjieba 复用同一词典。

**关于“权重”的机制限制（连续词权做不了，只能留/删）**：RAGFlow 给每个查询词连续 `term weight`、保留所有词只是权重不同。但 Milvus 内置 BM25 Function 只接收原始文本、**无法传 per-term 权重**，所以本方案只能用“离散四桶 + 增删词”，做不了真正的连续词权。要连续词权需自算稀疏向量（学习型稀疏 / 客户端 BM25），超出本刀范围（见 §13）。删除背景词比降权更激进、更易丢召回，故**仅对 `ListByCondition` 启用激进清洗，其余意图保守处理**。

### 4.4 查询文本生成

`sparse_text` 生成规则：

1. `ListByCondition`：核心条件词优先，追加章节提示词。
2. `MethodLookup`：方法号不作为 BM25 主词；若有章节提示，使用章节提示和剩余关键词。
3. `ClauseLookup`：保持原查询或用条款号置顶为主，BM25 只做辅助。
4. `GeneralFact`：删除句式停用词，保留领域实体和动作词。

`dense_text` 生成规则：

1. `ListByCondition`：改写成“查找试验方法中 [章节提示] 包含 [核心条件词] 的段落”。
2. `MethodLookup`：原句或“查找 [方法号] 的 [章节提示]”。
3. `ClauseLookup`：原句。
4. `GeneralFact`：原句或轻度去停用词后的句子。

> `dense_text` 是全案最未验证的部分：dense 怕裸关键词、偏好自然语义，但“查找…的段落”这种指令式措辞文档里不会出现，可能给 embedding 引入噪声；而短关键词式（如“混凝土试验 天平 仪具”）在对照实验里也能让 dense 召回对。实现时应对 `dense_text` 做 A/B（指令模板 vs 朴素短语），用 §9.3 评测集择优，不默认模板更好。

示例：

```text
原句:
公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平

intent:
ListByCondition

key_terms:
天平

section_hints:
仪具, 材料

sparse_text:
天平 仪具 材料

dense_text:
查找试验方法中仪具与材料包含天平的段落
```

> `section_hints=[仪具, 材料]` 并不在原句里，是“核心条件词是仪器 → 去仪具与材料章节找”的知识注入。其来源规则须在实现计划里定死：对 `ListByCondition` 且 `key_terms` 命中仪器类实体时补 `仪具/材料` hint；对非仪器列举（如“养护多少天”）不补错章节。

---

## 5. 检索链路修改

### 5.1 当前链路

```cpp
lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
lists.push_back(bm25.retrieve(qa.clean_text, filter, per_path_k));
```

### 5.2 方案 B 链路

```cpp
lists.push_back(dense.retrieve(qa.dense_text, filter, per_path_k));
lists.push_back(bm25.retrieve(qa.sparse_text, filter, per_path_k));
```

exact retriever、standard filter、clause pin 保持原有行为。

如果 `qa.sparse_text` 为空，则使用 `qa.clean_text`；如果 `qa.dense_text` 为空，也使用 `qa.clean_text`。这样即使新规则覆盖不足，也不会出现空查询。

---

## 6. 融合与意图化重排

### 6.1 Weighted RRF

现有 `rrf_fuse` 是等权：

```text
score += 1 / (k + rank + 1)
```

方案 B 增加轻量权重：

```text
GeneralFact:
dense 0.55
bm25  0.45

ListByCondition:
dense 0.35
bm25  0.65

ClauseLookup:
exact/pin 优先，dense/bm25 辅助

MethodLookup:
method exact 优先，dense/bm25 辅助
```

实现上新增 `weighted_rrf_fuse`，保留旧 `rrf_fuse` 不变，便于回滚。权重为初值、未调优，每次改动须用 §9.3 评测集回归。

### 6.2 Intent-aware rerank

对 `ListByCondition`，融合后再做轻量重排：

| 条件 | 调整 |
|---|---|
| 正文/标题/路径含任一 `key_terms` | 加分 |
| 正文/标题/路径含任一 `section_hints` | 加分 |
| 标题或路径显示 `仪具与材料/仪具/材料/设备` | 加分 |
| 标题或路径显示 `总则/目的/适用范围/引用标准` | 降权 |
| 同一 `method_no` 已出现多个候选 | 后续候选轻微降权，提升试验多样性（见 §6.3） |

rerank 需要 chunk 的 title/路径/正文，而当前 `Candidate` 只带 `chunk_id/score/source/standard_id`，不含正文。故 rerank 是一个**需要 join PG metadata 的后融合阶段**：在 `text_retrieve` 内对候选 `pg.get_chunk` 拉一遍（retrievecheck 展示时已是此做法），或上移到能访问 chunk 详情处。第一版若该阶段拿不到 metadata，可先只做 query rewrite + weighted RRF，并记录该限制。

> 须标明：此 intent-rerank 是“含关键词就加分”的**手写启发式临时替身**，不是成熟 RAG 的 rerank 模型（cross-encoder / BGE-reranker）。召回稳定后应替换为模型（§13）。

### 6.3 列举类覆盖（ListByCondition 专属）

“哪些试验用到天平”本质是**聚合/列举**：理想答案要**覆盖 N 个试验各一条**，而 top-k + RRF 是“按相关性取前 k”，并非“每组取代表”。库里 24 个含天平试验、`top_k=20`，朴素 top-k 很可能覆盖不全，且同一强相关试验会占多条、挤掉其它试验。故对 `ListByCondition` 增加覆盖处理：

1. **放大召回窗**：`per_path_k` 与融合 `top_k` 在 list intent 下放大（如 `top_k`→40），给覆盖留空间。
2. **按 `method_no` 去重保覆盖**：同一 `method_no` 在最终列表默认只保留最高分一条，其余降权或折叠，避免单个试验刷屏。
3. **多样性约束（MMR-lite）**：在 §6.2 rerank 基础上，对“已出现的 `method_no`”施加递增惩罚，优先把新试验提上来，最大化覆盖到的试验数。
4. 最终 `top_k` 仍可截断给生成端，但**覆盖度按去重后不同 `method_no` 数衡量**（见 §9.3 `coverage@method_no`）。

这解决“答全”，与 §4/§5 的“召回到”互补：召回得到含天平 chunk 是前提，覆盖处理保证它们分属尽量多的试验。

---

## 7. 端到端例子

### 7.1 失败样本

输入：

```text
公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平
```

旧链路：

```text
BM25 query = 原句
Dense query = 原句
结果 = 总则 / 目的适用范围 / 引用标准
```

新链路：

```text
BM25 query = 天平 仪具 材料
Dense query = 查找试验方法中仪具与材料包含天平的段落
ListByCondition rerank = 含天平 + 含仪具/材料 上浮
覆盖处理 = 按 method_no 去重 + 多样性惩罚，尽量覆盖更多试验
```

期望结果：

```text
Txxxx 第2节 仪具与材料 ... 天平 ...
Tyyyy 第2节 仪具与材料 ... 天平 ...
Tzzzz 第2节 仪具与材料 ... 天平 ...
```

### 7.2 方法号问题

输入：

```text
T0302 需要哪些仪具
```

分析：

```text
intent = MethodLookup
method_no = T0302
section_hints = 仪具
sparse_text = 仪具 材料 设备
dense_text = 查找 T0302 的仪具与材料
```

行为：

```text
PgExactRetriever 先召回 T0302
BM25/dense 只作为补充排序信号
```

### 7.3 普通事实问题

输入：

```text
路基沉降怎么评定
```

分析：

```text
intent = GeneralFact
sparse_text = 路基 沉降 评定
dense_text = 路基沉降怎么评定
```

行为：

```text
dense 与 BM25 近似均衡
不触发列举类章节 hint 和泛章节降权
```

### 7.4 条款问题

输入：

```text
JTG 3420 第5.1.2条是什么规定
```

分析：

```text
intent = ClauseLookup
standard_code = JTG3420
clause_no = 5.1.2
dense_text = 原句
sparse_text = 原句或轻度清洗
```

行为：

```text
标准号过滤 + 条款置顶保持最高优先级
```

---

## 8. 实现边界

### 8.1 推荐新增或修改的文件

```text
src/query/query_analysis.h
src/query/query_analysis.cpp
src/retrieve/text_search.cpp
src/retrieve/rrf.h
src/retrieve/rrf.cpp
tests/test_query_analysis.cpp
```

如果需要单独承载规则，可新增：

```text
src/query/query_terms.h
src/query/query_terms.cpp
```

### 8.2 第一版不修改的文件

```text
src/milvus/milvus_rest.cpp
src/retrieve/bm25_retriever.cpp
src/retrieve/dense_retriever.cpp
```

原因：

- Milvus schema 不变。
- BM25 retriever 仍接收一个查询字符串。
- Dense retriever 仍接收一个查询字符串。
- 改动集中在 query analysis 和 retrieval orchestration。

> 例外：若 §4.3 选用 Milvus `run_analyzer` 做查询分词，则需在 `milvus_rest` 增加一个只读的 `run_analyzer` 调用（不改 schema），或改为客户端 cppjieba 以完全不动 `milvus_rest`。

---

## 9. 测试与验收

### 9.1 单元测试

新增 query analysis 测试：

```text
输入: 公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平
断言:
intent == ListByCondition
key_terms 包含 天平
section_hints 包含 仪具 / 材料
sparse_text == 天平 仪具 材料
dense_text 包含 天平、仪具、材料
standard_code/clause_no/method_no 不误抽
```

保留现有测试：

```text
JTC 5210-2018 第5.1.2条是什么规定
JTG 3420 里水泥怎么取样
T0302 需要哪些仪具
路基沉降怎么评定
第5章讲了什么
```

需要更新旧断言：`clean_text` 继续等于原句，新增字段按规则生成。另加“试验筛”类反例，断言不被子串切坏。

### 9.2 检索验收

使用原失败样本：

```text
retrievecheck "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平" 20
```

通过标准：

1. top20 中出现多个不同试验方法的“仪具与材料”chunk。
2. 命中的正确 chunk 正文含“天平”。
3. “总则/目的/适用范围/引用标准”不再占据主要 top 结果。
4. 对照短查询不回归：`天平`、`混凝土试验天平`、`天平 仪具`。
5. 条款和方法号查询不回归：`JTG 3420 第5.1.2条是什么规定`、`T0302 需要哪些仪具`。

### 9.3 评测闭环（最小评测集，先于调参）

肉眼看“天平”一条样本无法判断改动是否整体变好。在动 weighted RRF 权重、词表、rerank 规则**之前**，先搭一个最小评测集，把调参从“盲调”变成“有度量”。

- **评测集**：~15–20 条问题，覆盖四种 intent（列举 / 条款 / 方法号 / 普通事实），每条标注**期望命中**的 `method_no` 或 `chunk_id`。把 §9.2 的对照查询（`天平`、`混凝土试验天平`、`天平 仪具`）固化进去作回归锚点。
- **指标**：
  - `recall@k`：期望 chunk 是否进 top-k；
  - **`coverage@method_no`（列举类专属）**：原失败问题命中的不同试验数 / 期望试验数（呼应 §6.3）；
  - 普通事实 / 条款 / 方法号问题的 `recall@k` 不下降（回归闸）。
- **用法**：每改一次权重 / 词表 / 规则，跑一遍评测集；任何普通问题指标下降即视为回归，须修或回滚。
- **形态**：做成 `retrievecheck` 的批量模式（读评测集 JSON、逐条算命中），或独立小脚本；与仓库 M4 evaluation-loop 计划衔接。

这是本方案里最便宜、收益最高的一块：没有它，§6 的所有权重 / 惩罚都是猜。

---

## 10. 风险与控制

| 风险 | 控制 |
|---|---|
| 规则词表覆盖不足，产物为空回退原句 = 静默退回 bug | 负向剔除 + 白名单实体词兜底；用 §9.3 评测集监控召回，靠失败样本扩词表 |
| 纯子串切坏真关键词（如“试验筛”） | 在真分词 token 上做词类切分（`run_analyzer` / cppjieba），加反例单测 |
| 过度删除领域词导致普通问题变差 | 只对 `ListByCondition` 激进清洗，`GeneralFact` 保守；触发词收紧到明确列举词 |
| `sparse_text` 太短导致召回过宽 | 加 `section_hints`，并在 rerank 中要求核心词命中 |
| `dense_text` 模板措辞引入噪声 | A/B 指令模板 vs 朴素短语，用评测集择优 |
| 重排需要 chunk 标题/正文但 Candidate 信息不足 | 第一版先完成 query rewrite + weighted RRF；metadata rerank 放到可访问 chunk 详情处 |
| 列举题召回到但答不全 | §6.3 放大 k + `method_no` 去重 + 多样性惩罚；`coverage@method_no` 度量 |
| 权重调参影响面大 | 新增 weighted RRF 函数，保留旧 RRF 作为回滚路径；每改必跑评测集 |

---

## 11. 实施顺序

1. **先搭 §9.3 最小评测集**（含 §9.2 对照查询），作为后续每步的度量基线。
2. 扩展 `QueryAnalysis` 结构，保持旧字段兼容。
3. 实现意图识别、词表规则、`sparse_text` 和 `dense_text` 生成；词类切分在**真分词 token** 上做（`run_analyzer` 或 cppjieba，见 §4.3），避免子串切坏词。
4. 更新 `text_search.cpp`，dense/BM25 分别使用新查询文本。
5. 新增 weighted RRF，按 intent 选择 dense/BM25 权重；**每改权重跑一遍评测集**。
6. 实现 §6.3 列举覆盖：list intent 放大 k + `method_no` 去重 + 多样性惩罚。
7. 增加 `ListByCondition` 的轻量 rerank；如果候选缺少必要 metadata，则先跳过 metadata rerank 并记录限制。
8. 补充 query analysis 单元测试和检索回归验证（用 §9.3 评测集，非单条肉眼）。

---

## 12. 通过标准

方案 B 完成时必须满足：

1. 原失败问题能召回多个不同方法的“仪具与材料”含“天平”chunk。
2. `analyze_query` 能输出可观察的 `intent / sparse_text / dense_text / key_terms / section_hints`。
3. 现有标准号、条款号、方法号解析行为不回归。
4. 不需要重建 Milvus collection。
5. 列举类 `coverage@method_no` 达到约定阈值（如原失败问题命中 ≥N 个不同试验，见 §6.3/§9.3）。
6. 全量改动在 §9.3 评测集上：列举/天平类指标上升，普通事实/条款/方法号指标不回归。
7. 不提交运行日志或无关产物。

---

## 13. 已知限制与后续

本方案是“成熟 RAG 的查询侧那一片”，以下为**主动放弃或暂用替身**的部分，单独立项推进，不在本刀完成：

1. **文档侧 enrichment（下一刀，价值最高）**：入库时给每个 chunk 抽 `important_kwd`、生成 `question_tks`，并做 title/关键词多字段加权匹配——RAGFlow 鲁棒性的主来源。需改 Milvus schema + 重灌，与本刀“不重建”冲突，故另立 spec。
2. **连续 term weight**：受限于 Milvus 内置 BM25 无法传 per-term 权重（§4.3），本刀只能“留/删”。要连续词权需上学习型稀疏（BGE-M3 等）或客户端 BM25。
3. **rerank 用模型替换手写规则**：§6.2 的 intent-rerank 是启发式临时替身；成熟做法是 cross-encoder / BGE-reranker，召回稳定后引入。
4. **同义词/扩展织入查询计划**：现有 `SynonymDict` 仅在 `Bm25Retriever` 对整句展开；应按词类对 `key_terms` 做同义词 + 细粒度子词扩展（如“天平 / 台秤 / 电子秤”）。
