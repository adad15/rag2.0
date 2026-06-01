# M8 block 级视觉增强 —— 设计文档

- 日期：2026-05-31
- 类型：里程碑设计 spec（M8）
- 上游：[总览路线图](2026-05-31-rag-overview-roadmap-design.md)、[技术设计文档整合版](../../规范文档rag系统技术设计文档_整合版.md)
- 对应文档章节：§5.5（page_blocks）、§8.3（block 级入库）、§17.4
- 前置里程碑：M7（page 级视觉路已跑通并经评估）

---

## 1. 定位与目标

M8 把视觉路从 **page 级**升级到 **block 级**，解决 M7 暴露的"命中页面但无法准确定位条款"的过粗问题（§18.1 风险行）。block 级把页面拆成版面块（text/table/figure/formula/header/footer），对块裁剪图建索引，从而把视觉命中精确到条款/表格/图，而非整页。

目标（§17.4）：建立 page_blocks、裁剪四类块、建 `block_visual`、block→clause 映射、block-level rerank，提升视觉路条款定位精度。

## 2. 范围

**做：** 版面 block 切分与裁剪、block 图像向量化、Milvus `block_visual` 集合、block→clause 映射、block 级视觉 Reranker（Qwen3-VL-Reranker-8B）。

**不做：** both/auto（M9）；对图纸的深度语义理解（§1.2 非目标第3项：不追求对所有图纸深度理解）。

## 3. 前置依赖

- M7 的视觉路框架（VisualRetriever 契约③实现、视觉 embedding 服务、page image 存储）；
- M2 已预留 `page_blocks` 表结构（§5.5）与 block 坐标的解析能力；
- **block 切分依赖版面分析**——来自 MinerU（§6.3 输出 page block + block 坐标 + 阅读顺序）或 poppler 渲染 + 版面检测；
- GPU：Qwen3-VL-Embedding（块图像）、Qwen3-VL-Reranker-8B（§4.3.2）。

## 4. 架构与组件

在 M7 视觉路上，把召回粒度从 page 换成 block，**仍是同一个 VisualRetriever 契约③形状**，只是底层集合与映射更细：

```text
query → 视觉 Embedding 编码
  → Milvus block_visual 召回(top-k 块)
  → 视觉 Reranker(Qwen3-VL-Reranker, block top20→top5)
  → block → clause 映射(page_blocks.related_node_id)
  → clause_id 回查 PG(权威源)
  → ContextFragment(契约⑤) → DeepSeek
```

新增/扩展组件：
- `BlockSegmenter`：基于 MinerU 版面分析（或版面检测）切出 block + bbox + block_type + 阅读顺序；
- `BlockCropper`：按 bbox 从 page image 裁剪 block 图像，落存储；
- `BlockVisualIngestPipeline`：block 图像向量化 → 写 `block_visual`；
- `BlockClauseMapper`：用 `page_blocks.related_node_id` 把命中块映射到条款；
- `VisualReranker`（block 级，Qwen3-VL-Reranker-8B）。

## 5. 数据模型变更

- `page_blocks`（§5.5，M2 预留，M8 填充使用）：
  `block_id / standard_id / page_no / block_type(text|table|figure|formula|header|footer) / bbox / ocr_text / related_node_id / image_path / quality_score`。
- Milvus 新增 `block_visual` 集合（§8.3）：
  | 字段 | 类型 | 说明 |
  |---|---|---|
  | `block_id` | string | 主键 |
  | `standard_id` | string | 标准 ID |
  | `page_no` | int | 页码 |
  | `block_type` | string | text/table/figure/formula |
  | `related_node_id` | string | 关联条款（映射键） |
  | `bbox` | string | 坐标 |
  | `status` / `access_level` | string | §9.6 标量过滤，不可绕过 |
  | `image_path` | string | 裁剪图路径 |
  | `visual` | float_vector | 块视觉向量 |
- 存储：新增 block 裁剪图目录（§4.4）。

## 6. 受影响的接口契约

| 契约 | 影响 |
|---|---|
| ① Parser | 依赖 MinerU 输出的 block + bbox + 阅读顺序（§6.3）；`BlockSegmenter` 消费解析中间格式 |
| ② Embedding | 复用 M7 的 `VisualEmbeddingClient`（对块图像编码） |
| ③ Retriever | `VisualRetriever` 内部从 page_visual 切到 block_visual，**对外形状不变** |
| ④ Candidate | block 经 `related_node_id` 归一到 `standard_id+clause_id`；同条款多块合并证据（§10.2） |
| ⑤ ContextFragment | 不变；block 命中可把对应 table/figure 片段填入 `tables`（配合 M5 表格片段化） |

## 7. 关键流程

### 7.1 block 级入库（§8.3）
```text
页面图像(M7)
  → 版面 block 切分(MinerU bbox+阅读顺序)
  → 按 block_type 裁剪 text/table/figure/formula 块图像
  → 块图像向量化(Qwen3-VL-Embedding)
  → 写 Milvus block_visual(含 related_node_id + status/access_level)
  → page_blocks 落库(bbox/ocr_text/related_node_id/quality_score)
```

### 7.2 block 级检索（§17.4）
```text
query → 视觉 Embedding → block_visual 召回(过滤下推)
  → Qwen3-VL-Reranker block top20→top5
  → block → clause 映射(related_node_id)
  → clause_id 回查 PG → ContextFragment → DeepSeek
```

## 8. 验收标准（绑定 §15 评估）

对照 M7 page 级 baseline，在 §15.2 视觉/表格题上：
- **视觉路条款定位精度**：block→clause 的 top-1/top-3 条款命中率显著优于 page→clause（§17.4 目标）；
- **页/块命中后条款映射成功率**（§15.3）提升；
- block 级视觉 Reranker A/B：相对无 reranker 是否带来净增益；
- 同样遵守 §9.6 状态+密级过滤（块级标量 + PG 二次校验）。

## 9. 风险

| 风险 | 应对 |
|---|---|
| 版面 block 切分质量依赖 MinerU，切分差则定位差 | 用 `quality_score` 过滤低质块；切分失败回退 page 级（M7） |
| block 数量庞大致索引/显存成本上升 | 只对必要文档/必要 block_type 建索引（§16.3） |
| block→clause 映射缺失（related_node_id 空） | 回退到所在 page 的 page_clause_map（M7 能力兜底） |
| 多个视觉 8B 模型显存压力 | 按需加载；reranker 低置信度才触发 |
| 对图纸过度深挖偏离目标 | 明确非目标（§1.2）：不追求图纸深度理解 |

## 10. 开放问题

1. block 切分用 MinerU 版面分析，还是引入独立版面检测模型？质量与成本权衡。
2. 哪些 block_type 值得建视觉索引（如 formula/header/footer 是否纳入）？
3. block 裁剪图的分辨率与存储成本上限。
4. block→clause 映射缺失时的兜底优先级（page 级 vs 丢弃）。
