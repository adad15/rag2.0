# M2c-3 落库与接线设计

- 日期：2026-06-11
- 类型：子项目 spec（M2c 第三刀：chunk_cache 落库 PostgreSQL/Milvus，替换 ingest 与 query 回查链路）
- 来源：[M2c-2 受控三文本 spec](2026-06-10-m2c2-retrieval-chunks-design.md) §11、[总设计文档](../../规范文档rag系统技术设计文档_整合版.md) §5.3，以及 2026-06-11 讨论
- 分支：V2.4
- 状态：设计已确认，待写实现计划
- 背景：M2c-1/M2c-2 已把 PDF 解析产物逐步转换为 `tree_cache` 和 `chunk_cache`，三文本质量已用两份真实标准验证（157 + 537 个 chunk，验收全过）。但这两层产物还停留在磁盘缓存：PG 里只有 M1 老切分写的 `clause_nodes`，Milvus 目前无数据，query 链路还在回查老表。M2c-3 把 `chunk_cache` 真正接入系统：写 PG `retrieval_chunks` 表、embed 后写 Milvus、ingest 链路替换 M1 老切分、query 回查源切到 chunk。

---

## 0. 在 M2c 几刀里的位置

```text
M2c-1  结构化建树      parse_cache -> tree_cache        已完成
M2c-2  受控三文本       tree_cache -> chunk_cache        已完成
M2c-3  落库与接线       chunk_cache -> PG + Milvus + query 接线   本刀
```

---

## 1. 一句话目标

把 `RetrievalChunkCache` 落进 PostgreSQL 新表 `retrieval_chunks` 和 Milvus（dense 向量只 embed `embedding_text`），ingest 一条龙替换 M1 老切分，query 命中后回查 chunk 并把 `context_text` 喂给生成模型。

完成后，用户提问命中的是 M2c-2 的受控 chunk，三文本设计正式兑现。

---

## 2. 核心设计决策（已与用户确认）

### 2.1 入口形态：chunkload 命令 + 重写 ingest，共用 loader

- 新增 `rag2 chunkload <chunk_cache.json>`：从缓存直接落库，不重跑 OCR/建树。调试、重灌的快速通道。
- 重写 `ingest_file`：parse（带 parse_cache）→ `build_clause_tree` → `build_retrieval_chunk_cache` → 落库，一条龙。
- 两者共用新模块 `chunk_loader` 的 `load_chunks()`，不写两份落库逻辑。

### 2.2 幂等策略：按 standard_id 先删后插

落库前先删掉 PG 和 Milvus 中该 `standard_id` 的全部旧 chunk，再插入新数据。数据源头是 chunk_cache 文件，数据库只是它的投影；先删后插保证重灌结果与首次一致，杜绝树重建后 chunk 集合变化留下孤儿 chunk 污染检索。不采用主键 upsert（会留孤儿）。

### 2.3 Milvus collection：复用配置名，新结构重建

当前 Milvus 无数据。继续用 `config.json` 的 `milvus_collection`，不加新配置项。collection 结构升级为：主键 `chunk_id`，标量 `node_id`、`standard_id`，dense 向量维度来自配置。`ensure_collection` 保持"不存在才创建"，不做结构探测；若环境里残留主键为 `node_id` 的旧结构空壳 collection，属一次性升级操作，由实现计划给出手动 drop 步骤后再首跑 chunkload。

### 2.4 query 接线：原地改造（方案 A）

`answer_query` 接口与调用方式不变，内部回查源从 `clause_nodes` 切到 `retrieval_chunks`：

- 给 LLM 的正文：`clause.text` → `chunk.context_text`（small-to-big 上下文）。
- 引用元数据：`clause_no`、`path_text` 从 chunk 行取；标准号/名称/状态仍回查 `standards`。

不做新老并存（方案 B 否决：老链路在 Milvus 里没有数据，"并存"是空架子）。

### 2.5 clause_nodes 表：保留不删，新链路停写

老表及其中数据无害，保留兼容；ingest 重写后不再写入。`clause_splitter` 模块保留（仍有单测引用），只是从 ingest 链路退场。

---

## 3. PG schema（`src/db/schema.sql` 追加）

```sql
CREATE TABLE IF NOT EXISTS retrieval_chunks (
    chunk_id       TEXT PRIMARY KEY,
    node_id        TEXT,
    standard_id    TEXT REFERENCES standards(standard_id),
    chunk_type     TEXT,            -- body | appendix | explanation
    clause_no      TEXT,
    method_no      TEXT,
    title          TEXT,
    path_text      TEXT,
    atomic_text    TEXT,
    embedding_text TEXT,
    context_text   TEXT,
    captions       JSONB DEFAULT '[]',
    formulas       JSONB DEFAULT '[]',
    page_start     INT,
    page_end       INT,
    has_table      BOOLEAN DEFAULT FALSE,
    has_formula    BOOLEAN DEFAULT FALSE,
    has_figure     BOOLEAN DEFAULT FALSE,
    suspect        TEXT
);
CREATE INDEX IF NOT EXISTS idx_chunks_standard ON retrieval_chunks(standard_id);
CREATE INDEX IF NOT EXISTS idx_chunks_node ON retrieval_chunks(node_id);
```

与总设计 §5.3 的差异：

| 总设计字段 | 本刀处理 |
|---|---|
| `retrieval_text` | 落地为 `embedding_text`（M2c-2 已正式改名拆分） |
| `bm25_text` | 不加，留 M3（届时可由 atomic/embedding 派生） |
| `dense_vector_id` | 不加，向量 ID 就是 `chunk_id` |
| `metadata_json` | 不加大杂烩字段，定位信号都是显式列 |

这张表是命中后回查的权威源：Milvus 只负责"找到谁"，PG 负责"它是什么"。

---

## 4. Milvus 变更（`src/milvus/milvus_rest.{h,cpp}`）

1. `ensure_collection(collection, dim)`：建表字段改为 `chunk_id`（主键，VarChar）、`node_id`（VarChar）、`standard_id`（VarChar）、`dense`（FloatVector, dim）。
2. 新增 `drop_collection(collection)`：用于旧结构升级（一次性）与测试清理。
3. 新增 `delete_by_standard(collection, standard_id)`：REST v2 entities/delete，filter 形如 `standard_id == "..."`。
4. `insert(...)` 签名扩展为携带 `chunk_id`、`node_id`、`standard_id` 三个标量。
5. `search(...)`：output_fields 带三个标量；`Hit` 结构加 `chunk_id` 字段。
6. `build_insert_body` / `build_search_body` / 新增 `build_delete_body` 保持纯函数风格，可单测。

旧结构检测从简：不做自动探测，`chunkload`/`ingest` 第一次运行前由用户用一次性手段（或 `ensure_collection` 失败时的报错提示）drop 旧 collection。当前 Milvus 无数据，这是一次性的低风险操作。

---

## 5. 新模块 `src/ingest/chunk_loader.{h,cpp}`

```cpp
struct ChunkLoadResult {
    int chunk_count = 0;     // cache 中 chunk 总数
    int embedded_count = 0;  // 成功 embed 并写入 Milvus 的数量
    int deleted_count = 0;   // 先删后插阶段删除的 PG 旧 chunk 行数
};

// chunk_cache -> PG retrieval_chunks + Milvus。按 standard_id 先删后插。
// dense 向量只 embed chunk.embedding_text。
ChunkLoadResult load_chunks(const RetrievalChunkCache& cache,
                            PgClient& pg,
                            milvus::MilvusRest& mv,
                            EmbeddingClient& embed,
                            const std::string& collection);
```

执行顺序：

1. `pg.delete_chunks_by_standard(cache.standard_id)`。
2. `mv.delete_by_standard(collection, cache.standard_id)`。
3. 逐 chunk：`pg.insert_chunk(row)` → `embed.embed(c.embedding_text)` → `mv.insert(...)`。
4. 返回计数；`embedded_count < chunk_count` 时调用方报警并返回非 0。

规模预期：两份真实标准约 700 chunk，逐条云 embedding 可接受；批量优化留 M5。

`PgClient` 配套新增：

```cpp
struct RetrievalChunkRow { /* 与 retrieval_chunks 列一一对应的轻量结构 */ };

void delete_chunks_by_standard(const std::string& standard_id);
void insert_chunk(const RetrievalChunkRow& row);
std::optional<RetrievalChunkRow> get_chunk(const std::string& chunk_id);
```

`RetrievalChunkRow` 是 db 层自己的结构，不复用 `src/retrieve/retrieval_chunk.h` 的 `RetrievalChunk`，避免 db 层反向依赖 retrieve 层；两者的字段映射函数 `chunk_to_row()` 放在 `chunk_loader`，是纯函数、可单测。

---

## 6. `ingest_file` 重写（`src/ingest/ingest_pipeline.cpp`）

签名不变，内部换骨架：

1. parse（带 parse_cache，现状保留）。
2. `standard_id = hash(file_path)` 照旧；`upsert_standard`，`standard_no` 从树取（M2c-1 已抽真号，优先于 M1 的首页正则）。
3. `build_clause_tree(doc, standard_id)` → 顺手 `write_tree_cache`。
4. `build_retrieval_chunk_cache(tree)` → 顺手 `write_chunk_cache`（与 treecheck/chunkcheck 产物路径一致，便于排查）。
5. `load_chunks(...)`。
6. `IngestResult.clause_count` 语义变为 chunk 数。

删除：`split_clauses` 调用、`insert_clause` 调用、逐页循环。

---

## 7. query 接线（`src/generate/answer_pipeline.cpp`、`src/retrieve/dense_retriever.cpp`）

1. `DenseRetriever`：Milvus `Hit` 现在带 `chunk_id`，`Candidate.clause_id` 字段沿用但语义变为 chunk_id（统一改名留 M3，本刀不动接口）。
2. `answer_query`：`pg.get_clause(...)` → `pg.get_chunk(...)`；
   - `f.text = chunk.context_text`
   - `f.clause_no = chunk.clause_no`、`f.path = chunk.path_text`
   - `f.standard_no/standard_name/status` 仍回查 `standards`。
3. 候选为空、回查为空的拒答逻辑照旧。

---

## 8. `chunkload` CLI（`src/main.cpp`）

```text
rag2 chunkload data/chunk_cache/<id>.json
```

行为：

1. 读取并解析 chunk_cache（不存在/为空/解析失败报错退出，沿用 chunkcheck 模式）。
2. 检查 `standards` 表有无该 `standard_id`，没有则补一行占位（`standard_no` 与 `standard_name` 都用 cache 的 `standard_no`，`file_path` 留空），保证外键成立。
3. `ensure_collection` → `load_chunks`。
4. 打印：`chunks=N embedded=M deleted_old=K wrote pg+milvus`；`M < N` 返回 1。

usage 行更新为 `<smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck|chunkload>`。

---

## 9. 错误处理

- **chunk_cache 损坏/缺失**：报错退出，不碰数据库。
- **embedding 单条失败**：warn 日志、跳过该条（PG 行已在、Milvus 缺向量），结束时 `embedded_count < chunk_count` 返回非 0；因先删后插幂等，重跑一次 chunkload 即全量修复。
- **PG/Milvus 连接失败**：异常向上抛，命令层捕获报错（现有模式）。
- **删旧成功但插入中断**：库中该标准数据不完整，重跑 chunkload 修复；不引入事务跨库一致性（单机自用，重跑成本低）。

---

## 10. 测试策略

必须覆盖：

1. **Milvus 请求体纯函数**：新 `build_insert_body`（三标量+向量）、`build_delete_body`（filter 表达式）、`build_search_body`（output_fields 含 chunk_id/node_id/standard_id）的 JSON 断言。
2. **`chunk_to_row` 映射**：`RetrievalChunk` → `RetrievalChunkRow` 全字段无损，captions/formulas 序列化为 JSON 数组文本。
3. **loader 顺序逻辑**：通过 mock/fake 或拆出的纯函数验证"先删后插"调用顺序与计数统计（不做真网络调用）。
4. **answer_pipeline 回查切换**：命中 chunk 后 fragment 取 `context_text/clause_no/path_text`（若现有测试用 mock PG，更新之；没有则补一个最小用例）。
5. **既有测试全绿**：`clause_splitter` 等保留模块的测试不动。

端到端验收（手动）：`chunkload` 灌两份真实 chunk_cache → `query "路基沉降怎么评定"` 与 `query "T0302 需要哪些仪具"` 返回带引用回答，引用展示 `clause_no` 与 `path_text`。

---

## 11. 不做

- 不做编号识别、exact match、BM25、RRF、rerank（M3）。
- 不做本地 embedding、批量 embedding（M5）。
- 不做表格结构化 `spec_tables`（M5）。
- 不做版本管理、增量重建（M6）。
- 不做图片附件回挂、视觉路（M7/M8）。
- 不加 `bm25_text`、`dense_vector_id`、`metadata_json` 列。
- 不删 `clause_nodes` 表、不删 `clause_splitter` 模块。

---

## 12. 验收标准

1. `rag2 chunkload data/chunk_cache/<id>.json` 能把两份真实 chunk_cache 灌进 PG + Milvus，`embedded_count == chunk_count`，重复执行结果一致（先删后插幂等）。
2. PG `retrieval_chunks` 行数等于 chunk_cache 中 chunk 数；三文本与定位字段逐列无损。
3. Milvus 向量只来自 `embedding_text`；标量带 `chunk_id/node_id/standard_id`。
4. `rag2 ingest <pdf>` 一条龙跑通：不再写 `clause_nodes`，产出 tree_cache/chunk_cache 副产品，最终库内状态与 treecheck→chunkcheck→chunkload 三步等价。
5. `rag2 query` 命中后 LLM 上下文为 `context_text`，引用元数据来自 chunk 行；老 `get_clause` 不再出现在 query 链路。
6. 全量 doctest 通过；新增纯函数测试覆盖 §10 列表。

---

## 13. 后续衔接

- **M3 查询理解**：`method_no/clause_no/chunk_type` 已是显式列，可直接做编号识别 → PG 精确过滤/Milvus filter → dense 排名 → rerank；`Candidate.clause_id` 届时统一改名 `chunk_id`。
- **M5 文本路增强**：`bm25_text` 列、批量 embedding、表格结构化在此表上扩展。
- **M6 版本管理**：`standards.status` 已在 fragment 中透传，版本切换可在 `delete_by_standard` 基础上做增量。

一句话：M2c-3 之后，"解析→建树→三文本→落库→检索→生成"全链路打通，M2 阶段收官，M3 起在稳定的 chunk 底座上加检索智能。
