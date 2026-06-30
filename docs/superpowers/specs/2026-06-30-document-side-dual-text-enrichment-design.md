# 文档侧双文本检索增强设计

- 日期：2026-06-30
- 类型：检索改进设计
- 状态：设计确认，等待 implementation plan
- 关联问题：`rq-081`，查询为“通用硅酸盐水泥安定性需要通过哪两种方法判定合格？”
- 关联代码：`src/retrieve/retrieval_chunk.*`、`src/ingest/chunk_loader.cpp`、`src/db/pg_client.*`、`src/db/schema.sql`、`src/milvus/milvus_rest.cpp`

## 1. 背景

当前系统已经做了查询侧增强：

- dense 查询使用语义向量。
- BM25 查询使用查询侧的 `sparse_text` / `key_terms`。
- 精确路会补标准号、条款号、方法号、列举题关键词等命中。
- RRF 把 dense、BM25、PG 精确召回融合。

但是文档侧还没有完成对应增强。现在写入 Milvus BM25 `text` 字段的内容，其实仍然是 `embedding_text`。

也就是说：

```text
查询侧 BM25 文本：sparse_text / key_terms
文档侧 BM25 文本：embedding_text
```

`embedding_text` 当初是为 dense embedding 准备的，刻意不放标准号、完整路径、方法号等强关键词，目的是避免向量语义被噪声污染。这个选择对 dense 是对的，但对 BM25 不够好。

下一阶段要补齐文档侧：让 dense 和 BM25 各吃适合自己的文本。

## 2. 问题复盘

失败样本 `rq-081`：

```text
问题：通用硅酸盐水泥安定性需要通过哪两种方法判定合格？
gold：GB 175-2023 / 7.4.2
正确正文：
7.4.2 安定性
安定性
7.4.2.1沸煮法合格。
7.4.2.2压蒸法合格。
```

排查结果：

- 正确 chunk 已在库里。
- dense 能理解语义，正确 chunk 在 dense top100 里排名很靠前。
- BM25 没有召回正确 chunk。
- RRF 融合时，正确 chunk 只有 dense 一路支持，分数被多路噪声 chunk 挤到 top20 之外。

根因不是“查询侧完全没写好”，而是“文档侧 BM25 可检索文本太瘦”。

正确 chunk 的原始 `embedding_text` 主要是：

```text
7.4.2 安定性
安定性
7.4.2.1沸煮法合格。
7.4.2.2压蒸法合格。
```

它缺少用户问题里的强匹配线索：

- `通用硅酸盐水泥`
- `GB 175-2023`
- 上级路径里的“物理性能”等上下文
- `方法`
- `判定`
- `要求`

BM25 是关键词匹配型检索。文档侧没有这些词时，查询侧再怎么带 `key_terms`，也很难匹配到这个 chunk。

## 3. 目标

本阶段目标是引入文档侧“双文本路线”：

- `embedding_text`：继续服务 dense embedding，保持干净、短、语义集中。
- `bm25_text`：新增，专门服务 Milvus BM25，允许加入标准号、路径、标题、方法号、少量可解释检索词。

核心目标：

- 修复 `rq-081` 这类“正确 chunk 很短，但用户问题带标准名和判定意图”的漏召回。
- 不污染 dense embedding。
- 不用手工给每个标准维护关键词。
- 不引入 LLM 文档改写。
- 可以稳定重建 chunk cache、PG、Milvus。

## 4. 非目标

本阶段不做以下事情：

- 不做强制标准过滤。用户问题模糊时，硬过滤容易误伤。
- 不做 LLM 自动扩写文档关键词。首版先用确定性规则，便于测试和回滚。
- 不改 RRF 融合权重。
- 不改查询侧 planner 的整体策略。
- 不把 `context_text` 直接塞给 BM25。`context_text` 包含同级叶子，容易把 BM25 变吵。
- 不把所有增强内容塞回 `embedding_text`。

## 5. 核心设计

### 5.1 新增 `bm25_text`

在 `RetrievalChunk` 中新增字段：

```cpp
std::string bm25_text;
```

文本职责如下：

| 字段 | 用途 | 是否放标准号/路径 | 是否用于 dense |
| --- | --- | --- | --- |
| `embedding_text` | dense embedding | 否 | 是 |
| `bm25_text` | Milvus BM25 sparse index | 是 | 否 |
| `context_text` | 回答展示和上下文拼接 | 是 | 否 |
| `atomic_text` | 精简证据正文 | 否 | 否 |

这样做的意义很简单：

```text
dense 继续看“这段话本身是什么意思”
BM25 改成看“这段话在什么标准、什么路径、什么条款下，还带哪些可检索词”
```

### 5.2 `bm25_text` 首版内容

首版 `bm25_text` 由确定性函数生成，建议命名为：

```cpp
compose_bm25_text(const ClauseTree& tree,
                  const TreeNode& n,
                  const std::map<std::string, const TreeNode*>& by_id)
```

建议格式：

```text
标准：GB 175-2023
标准代号：GB 175-2023 GB175-2023
路径：7 技术要求 > 7.4 物理性能 > 7.4.2 安定性
条款：7.4.2 安定性
检索词：通用硅酸盐水泥 安定性 方法 判定 要求 合格
正文：
7.4.2 安定性
安定性
7.4.2.1沸煮法合格。
7.4.2.2压蒸法合格。
```

字段来源：

- `标准`：来自 `tree.standard_no`。
- `标准代号`：原始标准号，加一个紧凑形式，例如 `GB 175-2023` 和 `GB175-2023`。
- `路径`：来自现有 `path_text_for`。
- `条款`：来自 `node_label(n)`。
- `正文`：来自当前叶子的标题、正文、图表题、公式，接近当前 `embedding_text`。
- `检索词`：由少量规则生成，不手写到配置里。

### 5.3 关键词怎么添加

这里的“关键词”不是用户人工一个个补，也不是 LLM 猜出来。

首版关键词只来自三类确定性来源：

1. 元数据词

   来自标准号、标准名、方法号、条款号、路径标题。

   例子：

   ```text
   GB 175-2023
   GB175-2023
   通用硅酸盐水泥
   7.4.2
   安定性
   ```

2. 标题和路径词

   从 `path_text` 和当前标题里去重保留。

   例子：

   ```text
   技术要求
   物理性能
   安定性
   ```

3. 少量意图词

   这些词由简单规则触发，用来补用户常见问法和标准正文之间的词差。

   建议首版规则：

   | 触发条件 | 添加词 |
   | --- | --- |
   | 正文含 `合格` / `不合格` | `判定`、`要求`、`合格` |
   | 正文含 `应` / `不得` / `不应` / `应符合` | `要求`、`规定` |
   | 正文同时含 `法` 和 `合格` | `方法`、`试验方法`、`判定` |
   | chunk 有 `method_no` | 原始方法号和紧凑方法号 |

对 `rq-081` 来说，正文含 `沸煮法合格`、`压蒸法合格`，因此会补：

```text
方法 判定 要求 合格
```

这样用户问“哪两种方法判定合格”时，BM25 文档侧终于有词可以接住。

### 5.4 Milvus schema 不需要重命名

Milvus 里已经有 `text` 字段和 BM25 function：

```text
text -> sparse
```

本阶段不需要把 Milvus 字段改名为 `bm25_text`。代码侧做映射即可：

```text
Milvus row.text = RetrievalChunk.bm25_text
Milvus row.dense = embed(RetrievalChunk.embedding_text)
```

也就是：

```cpp
std::vector<float> vec = embed.embed(c.embedding_text);
mv.insert_full(collection, c.chunk_id, c.node_id, c.standard_id,
               status, c.bm25_text, vec);
```

这样 dense 和 sparse 的输入在写入时真正分离。

## 6. 数据流

改造前：

```text
ClauseTree
  -> RetrievalChunk.embedding_text
  -> embed(embedding_text)
  -> Milvus dense

RetrievalChunk.embedding_text
  -> Milvus text
  -> Milvus BM25 sparse
```

改造后：

```text
ClauseTree
  -> RetrievalChunk.embedding_text
  -> embed(embedding_text)
  -> Milvus dense

ClauseTree
  -> RetrievalChunk.bm25_text
  -> Milvus text
  -> Milvus BM25 sparse
```

查询侧保持：

```text
query.embedding_text -> dense search
query.sparse_text / key_terms -> BM25 search
```

文档侧补齐：

```text
chunk.embedding_text -> dense index
chunk.bm25_text -> BM25 index
```

## 7. 需要修改的模块

### 7.1 `src/retrieve/retrieval_chunk.h`

改动：

- `RetrievalChunk` 新增 `bm25_text`。
- `RetrievalChunkCache.schema_version` 从 `1` 升到 `2`。

原因：

- 老 cache 没有 `bm25_text`，不能直接拿来灌 Milvus。
- schema version 升级后，chunkload 可以明确提示用户重新生成 cache。

### 7.2 `src/retrieve/retrieval_chunk.cpp`

改动：

- 新增 `compose_bm25_text`。
- 新增 `compose_bm25_terms` 或同等小函数，用于生成 `检索词` 行。
- `chunk_from_leaf` 填充 `c.bm25_text`。
- JSON 序列化和反序列化加入 `bm25_text`。

保持不变：

- `compose_embedding_text` 不加入标准号、路径、方法号。
- `compose_context_text` 继续用于上下文展示，不直接复用为 BM25 文本。

### 7.3 `src/ingest/chunk_loader.cpp`

改动：

- `chunk_to_row` 填充 `row.bm25_text`。
- `load_chunks` 中 dense 仍然用 `c.embedding_text`。
- `insert_full` 的 `text` 参数改用 `c.bm25_text`。
- 如果 `cache.schema_version < 2` 或某个 chunk 的 `bm25_text` 为空，应给出清晰错误，提示重新生成 chunk cache。

关键代码方向：

```cpp
std::vector<float> vec = embed.embed(c.embedding_text);
mv.insert_full(collection, c.chunk_id, c.node_id, c.standard_id,
               status, c.bm25_text, vec);
```

### 7.4 `src/db/schema.sql`

改动：

- `retrieval_chunks` 新增 `bm25_text TEXT`。
- 对已存在的数据库，增加幂等迁移：

```sql
ALTER TABLE retrieval_chunks ADD COLUMN IF NOT EXISTS bm25_text TEXT;
```

原因：

- PG 要保存 `bm25_text`，方便排查 BM25 为什么命中或没命中。
- 这也是后续做评估审计和 case review 的基础。

### 7.5 `src/db/pg_client.h` / `src/db/pg_client.cpp`

改动：

- `RetrievalChunkRow` 新增 `bm25_text`。
- `insert_chunk` 写入和更新 `bm25_text`。
- `get_chunk` 读出 `bm25_text`。

暂不改：

- `chunks_containing` 首版继续查 `embedding_text`。

原因：

- `chunks_containing` 是 PG 精确内容直查，主要用于“正文确实包含某词”的补召回。
- `bm25_text` 含标准号、路径和意图词，如果直接用于 PG LIKE，容易放大误召回。

### 7.6 `src/milvus/milvus_rest.cpp`

Milvus schema 可以保持不变：

- 字段仍叫 `text`。
- BM25 function 仍然从 `text` 生成 `sparse`。

需要做的是调用侧传入新的 `bm25_text`。

如果后面想让 debug 更直观，可以只改变量名和注释，不需要改 Milvus 字段名。

## 8. 长度和噪声控制

`bm25_text` 不能越长越好。太长会让 BM25 误以为很多 chunk 都相关。

首版控制规则：

- 不直接使用完整 `context_text`。
- 不拼同级叶子正文。
- `路径` 只拼 ancestor 到当前叶子。
- `检索词` 去重。
- 图表题最多保留 3 条。
- 公式最多保留 5 条。
- 总长度建议控制在 3500 字符以内，硬上限必须小于 Milvus `text.max_length = 8192`。

## 9. 重建策略

代码改完后，旧数据不会自动变好。原因是 Milvus BM25 的 sparse index 是写入时由 `text` 字段生成的。

必须重建或重新灌入 chunk：

```text
重新生成 chunk cache
  -> 写入 PG retrieval_chunks.bm25_text
  -> 写入 Milvus text=bm25_text
  -> Milvus 重新生成 sparse 向量
```

最低要求：

- 对参与 eval 的标准重新生成 chunk cache。
- 重新执行 chunkload。
- 重新跑目标 case 和 100 题 rich eval。

如果只改代码但不重新 chunkload，BM25 仍然会用旧的 `embedding_text` 索引，评估结果不会体现新设计。

## 10. 测试设计

### 10.1 chunk 单元测试

文件：`tests/test_retrieval_chunk.cpp`

新增断言：

- `embedding_text` 仍不包含标准号和完整路径。
- `bm25_text` 包含标准号。
- `bm25_text` 包含路径。
- `bm25_text` 包含条款标题。
- `bm25_text` 包含由规则添加的检索词。
- 方法类 chunk 的 `bm25_text` 包含 `method_no` 和方法标题。

示例期望：

```cpp
CHECK(c.embedding_text.find("JTC 5210") == std::string::npos);
CHECK(c.embedding_text.find("技术状况评定 >") == std::string::npos);

CHECK(c.bm25_text.find("JTC 5210-2018") != std::string::npos);
CHECK(c.bm25_text.find("技术状况评定 >") != std::string::npos);
```

### 10.2 JSON round trip 测试

文件：`tests/test_retrieval_chunk.cpp`

新增断言：

- `schema_version == 2`。
- `bm25_text` 序列化后能读回来。
- 旧 schema 缺 `bm25_text` 时，解析可以成功，但 chunkload 不允许继续写入 Milvus。

### 10.3 chunk loader 测试

文件：`tests/test_chunk_loader.cpp`

新增或更新断言：

- `chunk_to_row` 保留 `bm25_text`。
- Milvus `insert_full` 的 text 参数应来自 `bm25_text`。
- embedding client 的输入仍来自 `embedding_text`。
- schema version 过旧时给出明确失败。

### 10.4 PG 测试

相关测试如果已有 PG fake 或集成测试，应覆盖：

- `insert_chunk` 写入 `bm25_text`。
- `get_chunk` 读回 `bm25_text`。
- `chunks_containing` 暂时仍查 `embedding_text`，避免行为无意扩大。

### 10.5 目标 case 验证

改完并重建数据后，运行：

```text
retrievecheck --query "通用硅酸盐水泥安定性需要通过哪两种方法判定合格？" --top 20
```

验收：

- top20 中出现 `GB 175-2023 / 7.4.2` 对应 chunk。
- BM25 路径本身能召回该 chunk，或者 RRF top20 能稳定召回该 chunk。

### 10.6 全量 eval 验证

使用当前 100 题 rich eval baseline 对比：

```text
Group Recall@20: 0.986667
Complete@20: 0.98
nDCG@20: 0.786285
Distractor-before-gold: 0.0705882
```

验收：

- `rq-081` 从 MISS 变为 HIT。
- `Group Recall@20` 不低于当前 baseline。
- `Complete@20` 不低于当前 baseline。
- `nDCG@20` 不出现明显下降，建议下降不超过 `0.01`。
- `Distractor-before-gold` 不出现明显恶化，建议上升不超过 `0.02`。

## 11. 风险和缓解

### 11.1 BM25 变吵

风险：

- 每个 chunk 都加标准号和路径后，某些标准名查询可能召回很多同标准 chunk。

缓解：

- 不把 `context_text` 整段塞入 BM25。
- `检索词` 只用少量规则。
- dense 不受影响。
- 通过 rich eval 观察 distractor 指标。

### 11.2 旧 cache 混用

风险：

- 老 chunk cache 没有 `bm25_text`。
- 如果直接 chunkload，Milvus 会写入空 text 或旧 text。

缓解：

- `schema_version` 升到 2。
- chunkload 检查 schema version。
- 失败信息明确提示重新生成 chunk cache。

### 11.3 误把增强词用于精确召回

风险：

- 如果 PG `chunks_containing` 改查 `bm25_text`，可能因为“要求”“判定”这类泛词召回过多。

缓解：

- 首版 `chunks_containing` 继续查 `embedding_text`。
- BM25 使用 `bm25_text`，让排序模型承担泛词噪声。

### 11.4 文本超过 Milvus 限制

风险：

- Milvus `text.max_length` 当前是 `8192`。

缓解：

- `bm25_text` 生成函数内部做长度控制。
- 超长时优先裁剪图表题、公式和正文尾部，不裁剪标准号、路径、条款和检索词。

## 12. 验收标准

代码实现完成后，必须满足：

- C++ 单元测试通过。
- Python annotation 测试不回退。
- chunk cache schema version 更新到 2。
- PG 中能看到 `retrieval_chunks.bm25_text`。
- Milvus 写入时 `text` 使用 `bm25_text`，dense vector 使用 `embedding_text`。
- 目标问题 `rq-081` top20 能召回 `GB 175-2023 / 7.4.2`。
- 100 题 rich eval 指标不低于当前核心 baseline。

## 13. 给小白看的总结

可以把检索想成两个人在找资料：

```text
dense 像一个懂意思的人
BM25 像一个按关键词翻目录的人
```

以前我们只给两个人同一张纸，这张纸为了 dense 写得很干净，但关键词不够全。dense 看得懂，BM25 却翻不到。

现在要改成两张纸：

```text
给 dense 的纸：只写正文重点，别塞太多目录信息
给 BM25 的纸：写正文 + 标准号 + 路径 + 条款 + 少量检索词
```

这样 dense 不被干扰，BM25 也终于有关键词可以匹配。

这就是“文档侧双文本路线”。

## 14. 下一步

确认本 spec 后，进入 implementation plan。建议实现顺序：

1. 加 `bm25_text` 数据结构和 JSON cache schema v2。
2. 写 `compose_bm25_text` 和关键词规则。
3. 改 PG schema / row / insert / get。
4. 改 chunk_loader，让 Milvus `text` 使用 `bm25_text`。
5. 补单元测试。
6. 重建 chunk cache 和 Milvus 数据。
7. 跑目标 case 和 100 题 rich eval。
