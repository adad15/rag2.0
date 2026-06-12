# M3a 查询理解与精确召回设计

- 日期：2026-06-12
- 类型：子项目 spec（M3 第一刀：查询理解 + PG 精确路 + RRF 融合；BM25/同义词/status 过滤留 M3b）
- 来源：[总体路线图](2026-05-31-rag-overview-roadmap-design.md) §3.1 M3、[M2c-2 spec](2026-06-10-m2c2-retrieval-chunks-design.md) §2.3/§5.2/§11、[M2c-3 spec](2026-06-11-m2c3-chunk-persistence-design.md) §13，以及 2026-06-12 讨论
- 分支：V2.4（或后续新分支）
- 状态：设计已确认，待写实现计划
- 背景：M2c-3 后全链路已打通，但 query 仍是单路 dense。M2c-2 特意把标准号/方法号/条款号从向量文本中剥离、存为 `retrieval_chunks` 的结构化列，并把"识别编号 → exact match/filter 收窄候选"的任务记给了 M3。M3a 兑现这笔账：让带编号的问题稳定命中，让方法号/标准号正确收窄检索范围。本刀不动 Milvus collection schema。

---

## 0. 在 M3 两刀里的位置

```text
M3a  查询理解 + PG 精确路 + RRF      不动 Milvus schema   ← 本刀
M3b  Milvus 全文检索升级 + BM25 路    collection 第二次重建 + 同义词/分词词典 + status 过滤
```

接缝：M3a 把 `Retriever` 契约演进为带 `RetrievalFilter`，并建好 RRF 融合与编排层。M3b 只需注册 `Bm25Retriever` 并扩展 filter 字段，接口与编排不再动。

---

## 1. 一句话目标

query 链路升级为：**查询理解（抽编号）→ dense 路（可按标准收窄）+ 方法号精确路 → RRF 融合 → 条款号命中置顶 → 现有回查生成链路**。

---

## 2. 核心设计决策（已与用户确认）

### 2.1 编号分角色（而非统一融合或精确短路）

三类编号指向的范围不同，参与召回的方式也不同：

| 编号 | 例子 | 指向 | 角色 |
|---|---|---|---|
| 条款号 | "第 5.1.2 条" | 单个 chunk | **置顶**：PG 精确命中后插队到结果第 1 位 |
| 方法号 | "T0302" | 一组 chunk（约 4-8 个） | **成路**：PG 取该方法全部 chunk 作为一路候选，与 dense 做 RRF 融合 |
| 标准号 | "JTG 3420" | 一整本（数百 chunk） | **收窄**：不产生候选，转成 `standard_id` 下推 Milvus 标量过滤 |

理由：条款号无歧义应一锤定音；方法号下有多个 chunk，需要 dense 语义在域内挑选（RRF 让两路同时命中者自然上浮）；标准号指向整本书，作为候选无意义，作为过滤器正合适。这与 M2c-2 spec §5.2 的设计意图一致。

否决的备选：统一 RRF（条款号问题会被 dense 噪声挤下去）；精确优先短路（丢失 dense 补充的相关条款）。

### 2.2 演进 Retriever 契约（方案 A）

`Retriever::retrieve(query, top_k)` → `retrieve(query, const RetrievalFilter&, top_k)`。

- `RetrievalFilter` M3a 只有一个字段 `standard_id`（空=不过滤）；M3b 加 `status` 等。
- `DenseRetriever` 适配新签名，filter 翻译成 Milvus 标量过滤表达式下推。
- 新增 `PgExactRetriever`（方法号路）实现同一契约。
- 条款号置顶不是 Retriever：它绕过排序语义，放编排层。

否决的备选：不动接口在编排层直连（M3b 还得二次手术）；全塞 answer_pipeline（纯逻辑无法单测）。

### 2.3 顺手完成 `Candidate.clause_id` → `chunk_id` 改名

M2c-3 遗留的统一改名（candidate.h 注释里记的账），本刀触碰检索层所有调用点，是改名的最佳时机。

### 2.4 解析失败的宽松回退

标准号解析出来但库里查不到（如用户记错号）→ 忽略收窄、回退全库 dense，并记 warn 日志；不拒答。条款号/方法号查不到 → 该路为空，dense 路照常。单机自用，宽松优于严格。

---

## 3. 查询理解模块 `src/query/query_analysis.{h,cpp}`

纯函数，无 IO：

```cpp
struct QueryAnalysis {
    std::string clean_text;    // 原始问题（dense 路输入）
    std::string standard_code; // 归一化裸代号，如 "JTC5210-2018" / "JTC5210"（空=未指定）
    std::string clause_no;     // 归一化条款号，如 "5.1.2"（空=未指定）
    std::string method_no;     // 归一化方法号，如 "T0302-2024"（空=未指定）
};

QueryAnalysis analyze_query(const std::string& question);
```

抽取规则与复用：

1. **标准号**：复用 `extract_standard_no`（src/ingest/standard_meta）抽取，再归一化为去空格裸代号。允许不带年份（"JTC 5210" → "JTC5210"）。
2. **条款号**：正则识别"第 X.X.X 条"与裸多级号（≥2 级，如 5.1.2），复用/对齐 `parse_clause_no` 的文法约定；单级数字（如"第 5 章"）不算条款号。
3. **方法号**：与入库侧同一文法（`T\s*\d{4}\s*-?\s*(\d{4})?`，全角破折号归一），**把入库侧 retrieval_chunk.cpp 匿名命名空间里的 `extract_method_no_from_text`/`normalize_dashes` 提为共享函数**（移到可被两侧 include 的位置），保证查询侧与库内字符串逐字相等可比。允许不带年份的 "T0302"（匹配时前缀比较）。
4. `clean_text` 保持原问题不变（编号词不抠除——dense 对噪声不敏感，抠除反而可能破坏语义）。

---

## 4. 检索层变更

### 4.1 `src/retrieve/retrieval_filter.h`

```cpp
struct RetrievalFilter {
    std::string standard_id;   // 空=不过滤。M3b 加 status 等字段。
};

// 空 filter 返回空串；否则形如 standard_id == "..."
std::string to_milvus_expr(const RetrievalFilter& f);
```

### 4.2 契约演进 `src/retrieve/retriever.h`

```cpp
virtual std::vector<Candidate> retrieve(const std::string& query,
                                        const RetrievalFilter& filter,
                                        int top_k) = 0;
```

### 4.3 `candidate.h` 改名

`Candidate.clause_id` → `Candidate.chunk_id`，注释同步，所有引用点更新（dense_retriever、rrf、text_search、answer_pipeline）。

### 4.4 `DenseRetriever` 适配

embed 后调用带 filter 的 Milvus 搜索；`source = "dense"`。

### 4.5 Milvus REST：search 带 filter

`build_search_body` 增加可选 `filter` 参数（空串则不写入 filter 键），`MilvusRest::search` 签名加 filter 参数。纯函数可单测。collection schema 不动。

### 4.6 `PgExactRetriever`（方法号路）`src/retrieve/pg_exact_retriever.{h,cpp}`

```cpp
class PgExactRetriever : public Retriever {
public:
    // 每次查询由 text_search 用解析出的 method_no 现场构造（构造注入，
    // 因为契约③签名不携带编号）。method_no 为空时 retrieve 返回空列表。
    PgExactRetriever(PgClient& pg, std::string method_no);
    // 非空：PG 按 method_no 前缀匹配取该方法全部 chunk，
    // 按 clause_no 文档序排列，score=1.0，source="exact"。
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
};
```

匹配语义统一为**前缀匹配**（`method_no LIKE 'T0302%'`）："T0302" 命中 "T0302-2024"（用户常省年份）；输入带年份 "T0302-2024" 时前缀匹配恰为全等，无需特殊分支。filter.standard_id 非空时叠加 `AND standard_id=`。

### 4.7 `PgClient` 新增三个方法

```cpp
// 按归一化裸代号查标准：standard_no 去空格后 LIKE '%code%'，现行优先，返回 standard_id（空=未找到）
std::string find_standard_by_code(const std::string& code);
// 方法号前缀匹配取 chunk 行（chunk_id/clause_no 等轻量列），按 clause_no 排序
std::vector<RetrievalChunkRow> chunks_by_method(const std::string& method_prefix,
                                                const std::string& standard_id /*可空*/);
// 条款号精确命中（标准收窄可空），返回 chunk_id 列表
std::vector<std::string> chunk_ids_by_clause(const std::string& clause_no,
                                             const std::string& standard_id /*可空*/);
```

库内 `standard_no` 是全称（如"公路技术状况评定标准（JTC 5210-2018）"），匹配按 `REPLACE(standard_no,' ','') LIKE '%' || code || '%'` 实现，code 已是去空格裸代号。无 PG 测试桩，这三个方法不写单测（沿用惯例），由端到端验收覆盖。

### 4.8 RRF 融合 `src/retrieve/rrf.{h,cpp}`

```cpp
// score = Σ 1/(k+rank)，按 chunk_id 去重，来源合并（"dense+exact"），截断 top_k。纯函数。
std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k);
```

k=60 经验默认，M4 评估集调参。

### 4.9 编排 `src/retrieve/text_search.{h,cpp}`

```cpp
// 查询理解 → 解析标准号为 standard_id（失败回退全库+warn）
// → dense 路 + 方法号路（有方法号才跑）→ RRF
// → 条款号命中置顶（pin_exact_clause，纯函数：插队+去重+截断）
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv, EmbeddingClient& embed,
                                     PgClient& pg, const std::string& collection,
                                     int per_path_k, int top_k);
```

`pin_exact_clause(fused, pinned_chunk_ids, top_k)` 提为纯函数：被置顶者若已在 fused 中则上移到最前并去重（原 `source` 追加 `"+pin"`）；不在则插入第 1 位（`source = "exact_pin"`，score=1.0）。总数截断 top_k。

### 4.10 `answer_query` 接线

签名不变，内部把 `retriever.retrieve(question, top_k)` 换成 `text_retrieve(...)`；fragment 组装与回查逻辑不动。`main.cpp` 的 `cmd_query` 不再构造 `DenseRetriever`（编排层内部构造），调用点简化。per_path_k 默认 `top_k*4`（=20），top_k 仍 5。

---

## 5. 示例（端到端预期行为）

1. **"JTC 5210 第 5.1.2 条是什么规定"** → standard_code=JTC5210 收窄 + clause_no=5.1.2 置顶 → 第 1 位必为 5.1.2 路基沉降，后续为 dense 补充。
2. **"T0302 需要哪些仪具"** → method_no=T0302 路取出该方法全部 chunk；dense 全库找"需要哪些仪具"；RRF 后"2 仪具与材料"两路同中排第 1。
3. **"JTG 3420 里水泥怎么取样"** → 收窄到试验规程 537 chunk 内 dense 检索；评定标准的条款不会出现。
4. **"路基沉降怎么评定"**（无编号）→ 行为与 M2c-3 现状完全一致（dense 全库），回归不变性。
5. **"JTG 9999 的规定"**（库中无此标准）→ warn 日志 + 全库 dense 回退，不拒答。

---

## 6. 不做（M3a 边界）

- 不动 Milvus collection schema（text/sparse/status 标量、BM25 Function → M3b）。
- 不做同义词词典、jieba 自定义分词（→ M3b）。
- 不做 status 现行过滤（Milvus 无该标量 → M3b）。
- 不做表格意图标记 wants_table（M5 才消费，YAGNI）。
- 不做密级 access_level（单机自用，砍掉）。
- 不做 rerank、一跳引用扩展（M5）。
- 不做评估集与调参（M4；k=60/per_path_k=20 为经验默认）。

---

## 7. 测试策略

必须覆盖（doctest，纯函数 TDD）：

1. **query_analysis**：三类编号的抽取与归一化——标准号带/不带年份、全半角空格；条款号"第X.X.X条"/裸号/单级不算；方法号 `T 0302-2024`/`T0302`/全角破折号；无编号问题三字段全空且 clean_text 原样。
2. **方法号共享函数**：入库侧与查询侧产出一致（同一函数，入库侧既有测试保持绿）。
3. **rrf_fuse**：双路融合排序、chunk_id 去重、来源合并、top_k 截断、单路退化。
4. **pin_exact_clause**：置顶插队、已在列表时上移去重、截断。
5. **build_search_body 带 filter**：filter 空/非空的请求体 JSON 断言。
6. **既有测试全绿**：契约签名变更后 dense/answer_pipeline 相关现有测试更新而不删除。

端到端验收（真库手动）：§5 的 5 个示例各跑一遍，验证置顶/收窄/融合/回归不变性/宽松回退。

---

## 8. 验收标准

1. 条款号问题（§5 例 1）第 1 位稳定返回该条款，引用来源标记含 exact。
2. 方法号问题（§5 例 2）目标 chunk 进入 top2，且该方法相关 chunk 占比明显提升。
3. 标准号收窄（§5 例 3）结果全部属于指定标准。
4. 无编号问题（§5 例 4）与 M2c-3 行为一致（回归不变）。
5. 无效标准号（§5 例 5）回退全库不拒答。
6. `Candidate.chunk_id` 改名完成，仓库内无 `clause_id` 残留引用。
7. 全量 doctest 通过；新增纯函数测试覆盖 §7 列表。

---

## 9. 后续衔接

- **M3b**：注册 `Bm25Retriever`（契约已就位）；`RetrievalFilter` 加 `status`；Milvus collection 第二次重建（text/sparse/BM25 Function/status 标量）+ chunkload 重灌；同义词与分词词典。
- **M4**：评估集上调 RRF k、per_path_k、top_k；验证置顶策略对条款命中率的增益。
- **M5**：rerank 接在 RRF 之后；表格意图与 cell 定位。
