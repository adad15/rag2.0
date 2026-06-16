# RAGFlow-lite 查询计划与意图化召回方案

- 日期：2026-06-16
- 类型：修复设计 spec（方案 B）
- 分支：V3.1
- 状态：设计已选定，等待用户 review 后进入 implementation plan
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

RAGFlow 的成熟做法不是把原句直接交给全文检索，而是在查询侧做 term weight、停用词处理、同义词和字段/信号加权，再把 full-text 与 dense 结合，并在 rerank 阶段继续利用标题、重要词、问题词等信号。本项目不直接搬 RAGFlow 的多字段索引，而是实现一个轻量版：

```text
用户原句
-> QueryAnalysis 生成 QueryPlan
-> dense 使用 dense_text
-> BM25 使用 sparse_text
-> existing exact/method/clause recall
-> weighted RRF
-> intent-aware rerank
```

---

## 2. 目标

1. 修复啰嗦自然语言列举问题导致的召回失败，尤其是“哪些试验用到了天平”这类问题。
2. 保留现有标准号、条款号、方法号的精确抽取和置顶逻辑。
3. 不重建 Milvus collection，不引入训练型稀疏模型，不依赖 LLM 在线改写。
4. 让查询侧产物可观察，后续能在日志或 `retrievecheck` 中看到 `intent / sparse_text / dense_text / key_terms`。
5. 短查询与普通事实问答不回归。

---

## 3. 非目标

1. 不在本阶段实现 RAGFlow full 版的多字段索引，例如 `important_kwd`、`question_tks`、`title_tks`。
2. 不在本阶段接入 SPLADE、DeepImpact、COIL 等学习型稀疏检索。
3. 不做 LLM query rewrite、HyDE 或多 query expansion。
4. 不改变 chunk 入库结构和 Milvus schema。
5. 不把 reranker 作为召回失败的主要补救手段；rerank 只处理已召回候选的排序。

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
| `ListByCondition` | `哪些/有哪些/哪几项/用到/需要/包含/使用/采用/涉及` | BM25 权重提高，按条件词和章节提示重排 |
| `GeneralFact` | 其他问题 | dense 与 BM25 均衡 |

优先级：

```text
ClauseLookup > MethodLookup > ListByCondition > GeneralFact
```

如果同一个问题既有方法号又问“需要哪些仪具”，例如“T0302 需要哪些仪具”，意图可记录为 `MethodLookup`，同时保留 `section_hints=["仪具"]`，因为方法号精确召回应优先。

### 4.3 词类与权重

查询词先按规则分成四类：

| 类型 | 示例 | 处理 |
|---|---|---|
| 核心条件词 | `天平`、`烘箱`、`压力机`、`坍落度筒` | 进入 `key_terms` 和 `sparse_text` |
| 章节提示词 | `仪具`、`材料`、`设备`、`器具`、`试剂`、`步骤`、`结果` | 进入 `section_hints` 和 `sparse_text` |
| 领域背景词 | `公路`、`工程`、`水泥`、`混凝土`、`试验`、`规程` | 一般不进入列举类 BM25 主查询 |
| 句式停用词 | `哪些`、`什么`、`怎么`、`中`、`的`、`了`、`用到` | 不进入 BM25 主查询 |

第一版不依赖分词库新增能力，采用“子串命中 + 词表规则”即可。这样能稳定命中“天平”这类短实体，也能避免把满库背景词送入 BM25。

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

实现上可以新增 `weighted_rrf_fuse`，也可以先在 `Candidate.source` 列表外附带权重配置。第一版推荐新增一个函数，避免改动现有 `rrf_fuse` 的行为，便于回滚。

### 6.2 Intent-aware rerank

对 `ListByCondition`，融合后再做轻量重排：

| 条件 | 调整 |
|---|---|
| 正文/标题/路径含任一 `key_terms` | 加分 |
| 正文/标题/路径含任一 `section_hints` | 加分 |
| 标题或路径显示 `仪具与材料/仪具/材料/设备` | 加分 |
| 标题或路径显示 `总则/目的/适用范围/引用标准` | 降权 |
| 同一 `method_no` 已出现多个候选 | 后续候选轻微降权，提升试验多样性 |

第一版如果融合阶段拿不到标题/路径正文，可以先只基于后续可取到的 chunk metadata 做 rerank；若当前 `Candidate` 信息不足，则把 rerank 放到能访问 chunk 详情的上游或下游，并保持第一版只做 query rewrite + weighted RRF。

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

需要更新旧断言：`clean_text` 继续等于原句，新增字段按规则生成。

### 9.2 检索验收

使用原失败样本：

```text
retrievecheck "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平" 20
```

通过标准：

1. top20 中出现多个不同试验方法的“仪具与材料”chunk。
2. 命中的正确 chunk 正文含“天平”。
3. “总则/目的/适用范围/引用标准”不再占据主要 top 结果。
4. 对照短查询不回归：
   - `天平`
   - `混凝土试验天平`
   - `天平 仪具`
5. 条款和方法号查询不回归：
   - `JTG 3420 第5.1.2条是什么规定`
   - `T0302 需要哪些仪具`

---

## 10. 风险与控制

| 风险 | 控制 |
|---|---|
| 规则词表覆盖不足 | 产物为空时回退原句；后续通过失败样本扩展词表 |
| 过度删除领域词导致普通问题变差 | 只对 `ListByCondition` 激进清洗，`GeneralFact` 保守处理 |
| `sparse_text` 太短导致召回过宽 | 加 `section_hints`，并在 rerank 中要求核心词命中 |
| 重排需要 chunk 标题/正文但 Candidate 信息不足 | 第一版先完成 query rewrite + weighted RRF；metadata rerank 放到可访问 chunk 详情的位置 |
| 权重调参影响面大 | 新增 weighted RRF 函数，保留旧 RRF 作为回滚路径 |

---

## 11. 实施顺序

1. 扩展 `QueryAnalysis` 结构，保持旧字段兼容。
2. 实现意图识别、词表规则、`sparse_text` 和 `dense_text` 生成。
3. 更新 `text_search.cpp`，dense/BM25 分别使用新查询文本。
4. 新增 weighted RRF，按 intent 选择 dense/BM25 权重。
5. 增加 `ListByCondition` 的轻量 rerank；如果候选缺少必要 metadata，则先跳过 metadata rerank 并记录限制。
6. 补充 query analysis 单元测试和检索回归验证。

---

## 12. 通过标准

方案 B 完成时必须满足：

1. 原失败问题能召回多个不同方法的“仪具与材料”含“天平”chunk。
2. `analyze_query` 能输出可观察的 `intent / sparse_text / dense_text / key_terms / section_hints`。
3. 现有标准号、条款号、方法号解析行为不回归。
4. 不需要重建 Milvus collection。
5. 不提交运行日志或无关产物。
