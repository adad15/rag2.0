# M3b BM25 全文检索与状态过滤设计

- 日期：2026-06-13
- 类型：子项目 spec（M3 第二刀：Milvus 全文检索升级 + BM25 路 + 同义词/分词词典 + status 过滤）
- 来源：[总体路线图](2026-05-31-rag-overview-roadmap-design.md) §3.1 M3、[M3a spec](2026-06-12-m3a-query-understanding-exact-recall-design.md) §9、[废弃的旧 M3 计划](../plans/2026-05-31-m3-text-retrieval-three-way.md) Task 2/5、总设计文档 §8.2/§9.0.2/§9.0.3，以及 2026-06-13 讨论
- 分支：V3.0（或后续新分支）
- 状态：设计已确认，待写实现计划
- 背景：M3a 把编号识别、PG 精确路、RRF 融合做完，但语义召回仍只有 dense 一路。M3a 边界示例"沥青针入度试验精度"暴露了 dense 的盲区：专业术语（针入度）会被整句语义稀释，口语近义词（精度 vs 库内"精密度"）召回不稳。M3b 补上 BM25 关键词路（Milvus 2.6 内置全文检索，jieba 分词 + 自定义词典）和同义词扩展，并把 M3a 预留的 status 过滤管道建起来。

---

## 0. 在 M3 两刀里的位置

```text
M3a  查询理解 + PG 精确路 + RRF      不动 Milvus schema      已完成
M3b  Milvus 全文检索升级 + BM25 路    collection 第二次重建    ← 本刀
```

接缝：M3a 已把 `Retriever` 契约演进为带 `RetrievalFilter`、把 `rrf_fuse` 建好。M3b 只需注册 `Bm25Retriever` 这一路新检索器、给 `RetrievalFilter` 加 `status` 字段、重建 collection；编排骨架、回查生成段不动。

---

## 1. 一句话目标

query 链路从"dense + 方法号精确"两路扩为"**dense + BM25 + 方法号精确**"三路：BM25 走 Milvus 内置全文检索（jieba 分词 + 自定义词典），查询侧叠加同义词扩展；所有向量召回默认 `status==现行` 过滤。

---

## 2. 核心设计决策（已与用户确认）

### 2.1 BM25 用 Milvus 内置全文检索，C++ 侧 RRF 融合（方案 A）

新增 `Bm25Retriever` 实现现有契约③，内部调一次 Milvus 稀疏搜索（`annsField=sparse`，送查询文本而非向量）。编排层把 dense / bm25 / 方法号三路输出一起喂给已有 `rrf_fuse`。

否决的备选：

- **Milvus 服务端 hybrid 搜索**（dense+sparse 单请求内部融合）：绕过 C++ RRF 与契约，PG 方法号精确路无法加入 Milvus 内部融合，与 M3a 架构割裂。
- **C++ 客户端自算 BM25**：要在 C++ 维护语料统计/倒排，重造 Milvus 已有能力，且用不上 jieba 分词器。

### 2.2 collection 第二次重建（drop + 新 schema + 重灌）

升级 schema 需新增 `status` / `text` / `sparse` 字段和 BM25 Function，Milvus 不支持在线改 schema，故 drop 旧 collection 后用新 schema 重建，再 `chunkload` 重灌两本标准（重新 embed ~700 个 4096 维向量，数分钟）。一次性操作，与 M2c-3 首次重建同款流程，实现计划给具体命令。

### 2.3 BM25 的 text 字段复用 `embedding_text`

不另造 `bm25_text` 列。`embedding_text` 已是 M2c-2 的受控语义文本（条款号+标题+正文+caption+公式），适合关键词匹配。Milvus 的 BM25 Function 从 `text` 字段自动生成 `sparse`，客户端不算稀疏向量。

### 2.4 status 来源：落库时查 standards.status 写入每个 Milvus 行

chunk 本身不带 status（它在 `standards` 表）。`load_chunks` 落库前查一次该标准的 `standards.status`，把它随每个 chunk 写进 Milvus 的 `status` 标量。当前数据全为"现行"，过滤对全现行库无害透传；M6 把某标准置"作废"后重灌，该标准全部向量的 status 即变。

### 2.5 同义词扩展只作用于 BM25 路、只在查询侧

`SynonymDict::expand` 把查询中命中的同义词追加到查询串（原词保留），仅喂给 `Bm25Retriever`。dense 路用原始 `clean_text`（dense 对同义词本就有一定泛化，叠加扩展词反而可能稀释语义）。

---

## 3. Milvus collection 新 schema（`ensure_collection_text`）

```text
chunk_id     VarChar(256)  主键
node_id      VarChar(256)
standard_id  VarChar(128)
status       VarChar(32)               ← 新增标量
text         VarChar(8192)  enable_analyzer + jieba(自定义词典)  ← 新增
dense        FloatVector(dim)
sparse       SparseFloatVector         ← 新增，BM25 Function 输出
Functions: [ BM25: inputFieldNames=[text] → outputFieldNames=[sparse] ]
indexParams: dense(COSINE), sparse(BM25)
```

analyzer 配置（REST v2 create body 内 `text` 字段的 `analyzer_params`）：

```json
{ "tokenizer": { "type": "jieba", "dict": [<config/user_dict.txt 各行>] } }
```

实现计划提供完整 create-collection JSON；联调时若 Milvus 2.6 的 analyzer/Function 字段名有小版本差异，以报错信息为准微调（开放项）。

新增 `ensure_collection_text(collection, dim, user_dict)`，与旧 `ensure_collection` 并存：旧的留给可能的纯 dense 场景与既有测试，ingest/chunkload 改调新的。`ensure_collection_text` 仍是"不存在才创建"，结构升级靠一次性手动 drop（实现计划给步骤）。

---

## 4. 词典（两份受版本管理的文本文件）

### 4.1 `config/synonyms.txt`（领域同义词，查询侧扩展）

每行一组同义词，逗号分隔，`#` 开头为注释。种子内容：

```text
# 每行一组同义词，逗号分隔，查询侧 BM25 扩展用
沥青混合料,AC,沥青砼
针入度,贯入度
精度,精密度,重复性,允许误差
压实度,密实度
回弹模量,弹性模量
抗压强度,立方体抗压强度
延度,延伸度
```

### 4.2 `config/user_dict.txt`（jieba 自定义分词，建 collection 时装入 analyzer）

每行一个词。种子内容：

```text
针入度
压实度
回弹模量
抗压强度
沥青混合料
延度
软化点
集料筛分
水泥净浆
JTG/T
GB/T
```

两份均为种子，后续领域人员扩充。词典更新后需重建受影响 collection 与重灌（记录词典版本，留 M6 自动化）。

---

## 5. 检索层变更

### 5.1 `RetrievalFilter` 加 status

```cpp
struct RetrievalFilter {
    std::string standard_id;          // 空=不过滤
    std::string status = "现行";       // 默认只召回现行；空串=不按状态过滤
};
// to_milvus_expr：status 非空 → status == "现行"；standard_id 非空 → and standard_id == "..."
// 两者皆空 → 空串
```

`to_milvus_expr` 输出示例：`status == "现行"`、`status == "现行" and standard_id == "1221..."`。注意 M3a 的两个 `to_milvus_expr` 单测断言会变（默认 filter 现在产出 `status == "现行"` 而非空串），需同步更新。

### 5.2 `src/query/synonyms.{h,cpp}`（新增，纯逻辑）

```cpp
class SynonymDict {
public:
    void load_from_lines(const std::vector<std::string>& lines);
    void load_from_file(const std::string& path);
    // 查询中命中某词则追加其同义词（空格分隔，原词与已含词不重复追加）
    std::string expand(const std::string& query) const;
private:
    std::map<std::string, std::vector<std::string>> alias_;  // 词 -> 同组其他词
};
```

### 5.3 `src/retrieve/bm25_retriever.{h,cpp}`（新增，契约③）

```cpp
class Bm25Retriever : public Retriever {
public:
    Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
    // expand(query) → mv.search_bm25(collection, expanded, to_milvus_expr(filter), top_k)
    // → Candidate{source="bm25"}
};
```

### 5.4 Milvus REST 新增（`milvus_rest.{h,cpp}`）

- `ensure_collection_text(collection, dim, user_dict)`：§3 全文 schema。
- `insert_full(collection, chunk_id, node_id, standard_id, status, text, dense)`：写全字段（`sparse` 由 Function 自动生成，不传）。
- `search_bm25(collection, query_text, filter_expr, top_k)`：`annsField="sparse"`，`data=[query_text]`（原始文本，Milvus 分词+BM25），输出 `{chunk_id,node_id,standard_id}`，返回 `Hit`（复用现有结构）。
- `build_search_body` 已支持 filter（M3a）；BM25 请求体可复用或新增 `build_bm25_body` 纯函数。`build_insert_full_body` 纯函数可单测。

旧 `ensure_collection` / `insert`（4 字段）保留不删（既有测试引用），新链路改用 `_text` / `_full` 版本。

### 5.5 编排 `text_search.cpp` 加 BM25 路

```cpp
// dense 路（必跑）+ BM25 路（必跑）+ 方法号路（有方法号才跑）
DenseRetriever dense(mv, embed, collection);
Bm25Retriever bm25(mv, syn, collection);
lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
lists.push_back(bm25.retrieve(qa.clean_text, filter, per_path_k));
if (!qa.method_no.empty()) { PgExactRetriever exact(pg, qa.method_no); lists.push_back(...); }
fused = rrf_fuse(lists, 60, top_k);
// pin / 回查生成不变
```

`text_retrieve` 签名增加 `const SynonymDict& syn` 参数（由 `answer_query` 透传，`cmd_query` 加载词典）。filter 默认 `status==现行`，dense 路也随之带 status 下推。

### 5.6 入库 `chunk_loader.cpp` 写 status + text

`load_chunks` 增加一个 `status` 入参（调用方查 `standards.status` 得到，默认"现行"），insert 改用 `insert_full(...)` 写入 `status` 与 `text=embedding_text`。`ensure_collection` 改为 `ensure_collection_text`（在 ingest/chunkload 命令层，传入加载好的 user_dict）。

### 5.7 命令层 `main.cpp`

- `cmd_query`：加载 `config/synonyms.txt` → `SynonymDict`，透传给 `answer_query`。
- `cmd_ingest` / `cmd_chunkload`：加载 `config/user_dict.txt`，建 collection 用 `ensure_collection_text`；落库前查标准 status 传入 `load_chunks`。

`answer_query` 签名增加 `const SynonymDict& syn`。

---

## 6. 数据流（M3b 后）

```text
"沥青针入度试验精度"
  → analyze_query（无编号）→ filter{status:现行}
  ├ dense 路: search_dense(filter) ──────────┐
  ├ BM25 路: expand→"沥青针入度试验精度 贯入度 精密度 重复性 允许误差"
  │          search_bm25(filter) ────────────┤  关键词咬"针入度"，同义词补"精密度"
  │ (方法号路: 本例无编号，跳过)               │
  ▼                                           ▼
  rrf_fuse(dense + bm25) → pin(无) → PG 回查 → context_text → DeepSeek
```

正是 M3a"裸奔靠 dense"示例在 M3b 被补强之处。

---

## 7. 不做（M3b 边界）

- 不做密级 access_level（单机自用，已砍）。
- 不做 rerank、表格 cell 定位、一跳引用扩展（M5）。
- 不做完整版本生命周期（M6；M3b 只建 status 管道并默认过滤现行）。
- 不用 Milvus 服务端 hybrid（用 C++ RRF）。
- 不做评估集与调参（M4；RRF k=60、per_path_k=20 沿用经验默认）。
- 不另造 `bm25_text` 列（复用 embedding_text）。

---

## 8. 测试策略

必须覆盖（doctest，纯逻辑 TDD）：

1. **SynonymDict::expand**：命中追加同义词、无命中原样返回、原词保留、已含同义词不重复追加。
2. **to_milvus_expr 带 status**：默认 filter → `status == "现行"`；带 standard_id → `status == "现行" and standard_id == "..."`；status 与 standard_id 皆空 → 空串。同步更新 M3a 两个旧断言。
3. **build_insert_full_body**：JSON 含 chunk_id/node_id/standard_id/status/text/dense，不含 sparse。
4. **build_bm25_body / search_bm25 请求体**：`annsField=="sparse"`、`data` 为文本数组、含 filter、outputFields 含三标量。
5. **ensure_collection_text create body**：fields 含 status/text/sparse，text 带 analyzer，functions 含 BM25(text→sparse)，indexParams 含 sparse。
6. **既有测试全绿**：契约/编排签名变更后相关测试更新而非删除。

端到端验收（真库，重灌后手动）：

- 术语/近义查询（"沥青针入度试验精度"类、库内有对应标准时）BM25 召回不塌陷，目标条款进 top_k。
- M3a 五场景回归（条款置顶、方法成路、标准收窄、无编号回归、无效标准号回退）在加 BM25 路后仍成立。
- status 过滤：手动把某标准在 standards 置"作废"并重灌，默认查询不再召回其条款（验证管道，非 M6 完整生命周期）。

---

## 9. 验收标准

1. collection 重建为全文 schema（status/text/sparse/BM25 Function），两本标准重灌成功（embedded==chunk）。
2. `Bm25Retriever` 经 Milvus 全文检索召回，作为独立一路进 RRF。
3. 同义词扩展只作用于 BM25 路；jieba 自定义词典生效（专业术语整词召回）。
4. 默认 `status==现行` 过滤下推 dense 与 BM25 两路；置"作废"重灌后默认不召回。
5. M3a 五场景回归全部成立。
6. 全量 doctest 通过；新增纯函数测试覆盖 §8 列表。

---

## 10. 后续衔接

- **M4**：评估集上调 RRF k、per_path_k、三路权重；量化 BM25 对术语召回的增益。
- **M5**：rerank 接在 RRF 之后；同义词/分词词典随领域反馈扩充。
- **M6**：版本生命周期填真 status（改版自动作废）、词典版本化与受影响索引自动重建、PG↔Milvus 一致性。
