# M2c-2 受控三文本与检索 Chunk 设计

- 日期：2026-06-10
- 类型：子项目 spec（M2c 第二刀：三文本 / 检索 chunk；落库与 Milvus 接线另立 spec）
- 来源：[M2c-1 结构化建树 spec](2026-06-05-m2c1-structural-tree-design.md)、[M2b 进度与交接](../2026-06-04-m2b-progress-and-handoff.md) §5.2、[总设计文档](../../规范文档rag系统技术设计文档_整合版.md) §7.2/§7.6，以及 2026-06-10 讨论
- 分支：V2.2
- 状态：已确认核心边界，待写实现计划
- 背景：M2c-1 已把 `parse_cache/<id>.json` 转成 `tree_cache/<id>.json`，每个 `TreeNode` 已有 `node_id`、层级、父子关系、正文、图表题附件、公式、表格标记和页范围。M2c-2 不再解析 PDF，也不再重建树，而是把树上的叶子节点转换成可检索、可回填、可落库的 `RetrievalChunk`。本轮特别收紧旧设计中“retrieval_text = 标准号 + 路径 + 正文 + 元数据”的口径：**dense embedding 只吃受控语义文本，标准号/方法号/完整路径走结构化定位字段，避免公共前缀污染向量分数。**

---

## 0. 在 M2c 几刀里的位置

```text
M2c-1  结构化建树
       parse_cache -> tree_cache

M2c-2  受控三文本与检索 chunk
       tree_cache -> chunk_cache

M2c-3  落库与接线
       chunk_cache -> PostgreSQL retrieval_chunks / Milvus / ingest 替换
```

**接缝**：M2c-2 产出 `data/chunk_cache/<id>.json`。这样可以在不重跑 OCR、不重建树、不写数据库的情况下反复检查三文本质量。

---

## 1. 一句话目标

消费 M2c-1 的 `ClauseTree`，只为 `is_leaf=true` 的检索叶子生成 `RetrievalChunk`。每个 chunk 包含：

1. `atomic_text`：条款自己的权威文本，用于引用和展示。
2. `embedding_text`：真正送 dense embedding 的受控语义文本。
3. `context_text`：命中后给生成模型看的 small-to-big 上下文。
4. `metadata / locator`：标准号、方法号、条款号、完整路径、页码、附件关系等定位信息，默认不进入 dense embedding。

本轮不改 `ingest_file`，不改 PG schema，不调用 embedding，不写 Milvus。

---

## 2. 核心设计决策

### 2.1 `retrieval_text` 旧名拆分

总设计文档里用 `retrieval_text` 表示“用于检索的扩展文本”。这个方向是对的，但字段含义太宽，容易把标准号、完整路径、HTML、页码、bbox、JSON 字段名都塞进 dense embedding。

M2c-2 正式拆成三类：

| 类别 | 字段 | 是否 dense embedding | 说明 |
|---|---|---:|---|
| 权威正文 | `atomic_text` | 否 | 展示、引用、回答原文问题 |
| 语义召回 | `embedding_text` | 是 | dense 向量只吃它 |
| 定位信号 | `locator` / metadata | 否 | `standard_no`、`method_no`、`clause_no`、`path_text`，给精确匹配、过滤、BM25、rerank |
| 生成上下文 | `context_text` | 否 | 命中后再回填给 LLM |

如果 M2c-3 为兼容旧 schema 仍保留 `retrieval_text` 字段，则该字段应写入本 spec 的 `embedding_text`，不得写入 metadata 大杂烩。

### 2.2 只给叶子节点生成主 chunk

M2c-1 已经决定检索粒度：普通十进制规范与试验规程默认到 L3，深于检索深度的内容并入当前叶子。M2c-2 不重新切分，不为父节点生成主 chunk。

```text
5 技术状况评定            不生成主 chunk
└─ 5.1 路基               不生成主 chunk
   ├─ 5.1.1 损坏类型      生成 chunk
   ├─ 5.1.2 路基沉降      生成 chunk
   └─ 5.1.3 边坡坍塌      生成 chunk
```

父节点仍很重要，但它们只提供 `path_text`、`locator` 和 `context_text` 的回填材料。

### 2.3 定位信号不无脑 embedding

标准号、方法号、完整父级路径是“定位信号”，不是“语义正文”。如果每个 chunk 的向量文本都带同一段公共前缀，候选会被拉近，短条款尤其明显。

坏例子：

```text
JTG 3432-2024
4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料
2 仪具与材料
天平、标准筛、烘箱……
```

同一个 `T0302` 下的“适用范围、仪具、步骤、结果整理”都会共享前两行，dense 分数容易变得差不多。

推荐做法：

```text
embedding_text:
2 仪具与材料
天平、标准筛、烘箱……

metadata / locator:
standard_no = JTG 3432-2024
method_no = T0302-2024
path_text = 4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料
```

后续查询链路应先识别 `T0302`、`JTC5210`、`5.1.2` 这类编号，用 exact match/filter/boost 缩小或重排候选，再用 `embedding_text` 做语义排名。

### 2.4 标题、正文、caption、公式可以受控进入 embedding

进入 `embedding_text` 的内容必须是用户可能自然问到的文本：

- 当前节点标题。
- 当前节点正文。
- 少量图题/表题 caption。
- 公式文本。

不进入 `embedding_text` 的内容：

- raw HTML 表格。
- bbox、page、source、ocr_confidence。
- JSON 字段名。
- 完整 `node_id`。
- 标准号、方法号、完整路径，默认只放 metadata。

caption 进入 embedding 的理由：用户可能直接问“路基沉降示意图在哪”“表 A-1 是什么”。如果 caption 只在 metadata 里，第一步 dense 召回可能命不中。图片本体、表格 HTML 仍不 embedding，命中后再由 metadata 回挂。

---

## 3. 数据模型

建议新增 `src/retrieve/retrieval_chunk.h`：

```cpp
struct RetrievalChunk {
    std::string chunk_id;      // node_id + "#main"
    std::string node_id;       // 对应 TreeNode
    std::string standard_id;
    std::string standard_no;
    std::string chunk_type;    // body | appendix | explanation

    std::string clause_no;     // 当前叶子 number
    std::string method_no;     // 最近的 T 方法号祖先，可空
    std::string title;         // 当前叶子标题
    std::string path_text;     // 完整可读路径，只作定位/展示，默认不 dense embedding

    std::string atomic_text;   // 条款自身权威文本
    std::string embedding_text;// dense embedding 输入
    std::string context_text;  // 命中后回填上下文

    std::vector<std::string> captions;
    std::vector<std::string> formulas;
    int page_start = 0;
    int page_end = 0;

    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::string suspect;
};

struct RetrievalChunkCache {
    int schema_version = 1;
    std::string standard_id;
    std::string standard_no;
    std::vector<RetrievalChunk> chunks;
};
```

`locator` 是逻辑分组，不要求实现成嵌套结构。`standard_no`、`method_no`、`clause_no`、`path_text`、`page_start/page_end` 都属于 locator 信号：可用于 exact match、过滤、BM25、rerank 和回显，默认不拼入 dense `embedding_text`。

不建议在 `RetrievalChunk` 中复制 `table_htmls`。表格 HTML 体积大、噪声重，M2c-2 只保留 `has_table` 和 caption 关系；表格结构化和表格 chunk 留给 M5 或后续专门任务。需要原始 HTML 时，仍以 `tree_cache` 的 `TreeNode.table_htmls` 为权威来源。

序列化接口：

```cpp
RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree);
std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache);
RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text);
void write_chunk_cache(const std::string& cache_path,
                       const RetrievalChunkCache& cache);
```

`chunk_cache` JSON 顶层应包含 `schema_version=1`、`standard_id`、`standard_no` 和 `chunks` 数组。单个 chunk 内也保留 `standard_id/standard_no`，便于后续单条重建和调试。

---

## 4. 三文本生成规则

### 4.1 `atomic_text`

`atomic_text` 是条款自身，尽量干净、权威、可引用。

生成规则：

1. 第一行写当前条款号和标题。
2. 后面写当前叶子 `text`。
3. 公式属于条款正文证据，可以追加。
4. caption 不进 `atomic_text`。
5. 父级路径、标准号、方法号不进 `atomic_text`。

示例：

```text
5.1.2 路基沉降

路基沉降应根据沉降深度和影响范围评定。

公式：
MQI = SCI + PQI + BCI + TCI
```

### 4.2 `embedding_text`

`embedding_text` 是 dense 向量输入。它应最大化语义可召回性，同时避免公共定位前缀污染。

默认组成：

1. 当前条款号和标题。
2. 当前条款正文。
3. 去重后的 caption，最多 3 条。
4. 公式文本，最多 5 条或 1200 字符。

默认不包含：

1. `standard_no`。
2. `method_no`。
3. 完整 `path_text`。
4. `page_start/page_end`。
5. `table_htmls`。

示例：

```text
5.1.2 路基沉降
路基沉降应根据沉降深度和影响范围评定。

相关图表题：
图5.1.2 路基沉降示意图

公式：
MQI = SCI + PQI + BCI + TCI
```

对 T 方法号文档：

```text
2 仪具与材料
天平、标准筛、烘箱……
```

不要默认生成：

```text
JTG 3432-2024
4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料
2 仪具与材料
天平、标准筛、烘箱……
```

因为 `JTG 3432-2024` 和 `T 0302-2024 集料筛分试验` 会在该方法下所有 chunk 中重复，容易拉平分数。`T0302` 应进 `method_no`，交给 M3 的查询理解和精确匹配。

### 4.3 `context_text`

`context_text` 是命中后给生成模型看的 small-to-big 上下文，不参与 dense embedding。

默认策略：

1. 找到当前叶子的最近父节点。
2. 收集该父节点下的叶子文本，按文档顺序拼接。
3. 包含当前叶子和同父级兄弟叶子。
4. 字符上限第一版设为 6000，超过时优先保留当前叶子，再向前后兄弟扩展。
5. 开头可放完整 `path_text`，因为这是命中后上下文，不会污染向量。

示例：

```text
路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降

5.1 路基

5.1.1 路基损坏类型
路基损坏包括沉陷、坍塌、冲刷等……

5.1.2 路基沉降
路基沉降应根据沉降深度和影响范围评定。

5.1.3 边坡坍塌
边坡坍塌应按坍塌规模和影响范围评定。
```

如果父级范围过大，先退化为“当前叶子 + 前一叶子 + 后一叶子”。M2c-2 只做确定性截断，不做 token 估算和 LLM 压缩。

---

## 5. 示例

### 5.1 普通十进制规范

M2c-1 树：

```text
standard_no = JTC 5210-2018

5 技术状况评定
└─ 5.1 路基
   ├─ 5.1.1 路基损坏类型
   ├─ 5.1.2 路基沉降
   │  text: 路基沉降应根据沉降深度和影响范围评定。
   │  captions: 图5.1.2 路基沉降示意图
   │  formulas: MQI = SCI + PQI + BCI + TCI
   └─ 5.1.3 边坡坍塌
```

M2c-2 chunk：

```json
{
  "chunk_id": "sid:5/5.1/5.1.2#main",
  "node_id": "sid:5/5.1/5.1.2",
  "standard_no": "JTC 5210-2018",
  "chunk_type": "body",
  "clause_no": "5.1.2",
  "method_no": "",
  "title": "路基沉降",
  "path_text": "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降",
  "atomic_text": "5.1.2 路基沉降\n\n路基沉降应根据沉降深度和影响范围评定。\n\n公式：\nMQI = SCI + PQI + BCI + TCI",
  "embedding_text": "5.1.2 路基沉降\n路基沉降应根据沉降深度和影响范围评定。\n\n相关图表题：\n图5.1.2 路基沉降示意图\n\n公式：\nMQI = SCI + PQI + BCI + TCI",
  "context_text": "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n\n5.1.1 ...\n5.1.2 ...\n5.1.3 ...",
  "captions": ["图5.1.2 路基沉降示意图"],
  "has_figure": true,
  "has_formula": true,
  "has_table": false
}
```

用户问“路基沉降怎么评定”，dense 靠正文命中。用户问“路基沉降示意图在哪”，dense 靠 caption 命中。命中后用 `path_text/page_start/captions` 回挂图表附件。

### 5.2 T 方法号试验规程

M2c-1 树：

```text
standard_no = JTG 3432-2024

4 集料试验
└─ T 0302-2024 集料筛分试验
   ├─ 1 适用范围
   ├─ 2 仪具与材料
   │  text: 天平、标准筛、烘箱……
   └─ 3 试验步骤
```

M2c-2 chunk：

```json
{
  "chunk_id": "sid:4/T0302-2024/2#main",
  "node_id": "sid:4/T0302-2024/2",
  "standard_no": "JTG 3432-2024",
  "chunk_type": "body",
  "clause_no": "2",
  "method_no": "T0302-2024",
  "title": "仪具与材料",
  "path_text": "4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料",
  "atomic_text": "2 仪具与材料\n\n天平、标准筛、烘箱……",
  "embedding_text": "2 仪具与材料\n天平、标准筛、烘箱……",
  "context_text": "路径：4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料\n\n1 适用范围 ...\n2 仪具与材料 ...\n3 试验步骤 ..."
}
```

用户问“T0302 需要哪些仪具”时：

1. M3 查询理解识别 `method_no=T0302-2024`，先 exact/filter 到该方法范围。
2. dense 在该范围内用 `embedding_text` 让“仪具与材料”胜出。
3. 命中后把 `context_text` 给生成模型。

这样比“每条都 embedding `T0302 集料筛分试验`”更稳。

---

## 6. `chunkcheck` CLI

新增命令：

```text
rag2 chunkcheck data/tree_cache/<id>.json
```

行为：

1. 读取 `tree_cache`。
2. 构建 `RetrievalChunk`。
3. 写入 `data/chunk_cache/<id>.json`。
4. 打印体检表。

建议输出：

```text
chunkcheck <id> | standard_no=JTC 5210-2018 format=A_decimal
  chunks=123
  avg_atomic_chars=...
  avg_embedding_chars=...
  avg_context_chars=...
  with_caption=...
  with_formula=...
  with_table=...
  suspect=...
  empty_embedding=0
  long_embedding_over_2000=...
  wrote data/chunk_cache/<id>.json
```

质量门禁第一版只报警，不阻断：

- `empty_embedding > 0`。
- `long_embedding_over_2000` 较多。
- 有 caption/table/formula 的节点未反映到 flags。
- chunk 数量与 `tree.nodes where is_leaf=true` 不一致。

---

## 7. 实现组件

| 文件 | 职责 |
|---|---|
| `src/retrieve/retrieval_chunk.h` | `RetrievalChunk` 数据结构和接口 |
| `src/retrieve/retrieval_chunk.cpp` | 从 `ClauseTree` 生成 chunk，JSON 序列化 |
| `tests/test_retrieval_chunk.cpp` | 三文本、metadata、context 回填单测 |
| `src/main.cpp` | 新增 `chunkcheck` 命令 |
| `rag2.0/rag2.0.vcxproj` | 加新源文件 |
| `rag2.0.tests/rag2.0.tests.vcxproj` | 加新测试文件 |

命名放在 `src/retrieve/`，因为本轮产物已经是检索单元，而不是结构建树的一部分。

---

## 8. 测试策略

必须覆盖：

1. **只为叶子生成 chunk**：父节点不生成，chunk 数等于叶子数。
2. **`atomic_text` 干净**：包含当前号、标题、正文、公式；不含 caption、标准号、完整路径。
3. **`embedding_text` 受控**：包含当前标题、正文、caption、公式；不含标准号、方法号、完整路径、HTML、页码。
4. **定位 metadata 完整**：`standard_no`、`method_no`、`clause_no`、`path_text`、页范围从树正确派生。
5. **chunk 类型完整**：body / appendix / explanation 能从 `node_id` 或树分组规则派生并序列化。
6. **T 方法号不污染 dense**：`method_no` 字段有值，但 `embedding_text` 不含 `T0302`。
7. **`context_text` small-to-big**：包含完整路径和同父级兄弟叶子，超过上限时优先保留当前叶子。
8. **JSON round trip**：所有关键字段往返无损。

---

## 9. 不做

- 不调用 embedding API。
- 不创建或修改 PG 表。
- 不写 Milvus。
- 不替换 `ingest_file` 的 M1 老切分。
- 不做 query parser、exact match、BM25、RRF、rerank。
- 不做表格 cell 结构化。
- 不做图片裁剪、bbox 归属、视觉 embedding。
- 不把 `table_html` 直接塞进 dense 文本。
- 不把标准号、方法号、完整父级路径默认塞进 dense 文本。

这些留给 M2c-3、M3、M5、M7/M8。

---

## 10. 验收标准

1. `chunkcheck` 能读取 M2c-1 的 `tree_cache` 并写出 `chunk_cache`。
2. `chunk_cache` 中每个 chunk 都有非空 `atomic_text`、`embedding_text`、`context_text`。
3. chunk 数量等于树中 `is_leaf=true` 的节点数。
4. `embedding_text` 不含 raw HTML、完整 `path_text`、标准号、方法号。
5. caption 和公式能受控进入 `embedding_text`。
6. 标准号、方法号、条款号、路径和页码保存在 metadata 字段中，供 M2c-3/M3 使用。
7. body / appendix / explanation chunk 能区分，不靠裸条款号防撞。
8. 单测覆盖普通十进制规范和 T 方法号试验规程两个 profile。

---

## 11. 后续衔接

M2c-3 应消费 `chunk_cache`：

1. 扩展 PG schema 或新增 `retrieval_chunks` 表。
2. 把 `atomic_text`、`embedding_text`、`context_text`、metadata 写入 PG。
3. 调 embedding 时只使用 `embedding_text`。
4. Milvus 标量字段至少带 `node_id`、`standard_id`，后续可加 `method_no`、`clause_no`、`chunk_type`。
5. `query` 链路仍先保持老逻辑，等 M3 再引入编号识别、exact match、BM25、RRF。

一句话：M2c-2 先把“每个树叶应该怎样被向量召回、怎样被精确定位、怎样被上下文回填”做成稳定缓存；M2c-3 再负责把它接进数据库和向量库。
