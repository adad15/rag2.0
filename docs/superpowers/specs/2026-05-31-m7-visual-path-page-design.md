# M7 视觉路试点（page 级） —— 设计文档

- 日期：2026-05-31
- 类型：里程碑设计 spec（M7）
- 上游：[总览路线图](2026-05-31-rag-overview-roadmap-design.md)、[技术设计文档整合版](../../规范文档rag系统技术设计文档_整合版.md)
- 对应文档章节：§2.2、§8.3、§9.3、§17.3、§5.4（page_clause_map）
- 前置里程碑：M4（评估集，用于判断视觉路是否值得——§15.1 第5项）

---

## 1. 定位与目标

M7 上线**视觉路作为文本路的补充召回能力**（§2.2：视觉路不替代文本路）。第一版采用 **page 级**索引（§8.3：初始版本 page 级，block 级留待 M8）。

适用场景（§2.2）：扫描件 OCR 质量差、图纸/示意图/流程图多、表格结构复杂、版式对理解重要、用户明确问图/表/页面布局/扫描内容。

核心约束（§9.3 注）：**视觉路检索的是页面，但生成时仍回到条款文本与结构化表格**。

## 2. 范围

**做：** poppler 页面渲染、page 图像存储、Qwen3-VL-Embedding 服务、Milvus `page_visual` 集合、page→clause 映射、`visual` 模式检索与生成、视觉路评估。视觉 Reranker 可选（§16.3 建议视觉 reranker 先不用）。

**不做：** block 级（M8）、both/auto（M9）、对所有文档建视觉索引（§16.3：只对必要文档建索引——优先扫描件/图纸密集/复杂表格/OCR 差的文档，§17.3）。

## 3. 前置依赖

- M2 已建 `page_clause_map`（§5.4，页面→条款映射的数据基础）；
- M6 的一致性对齐框架（视觉索引同样需与 PG 状态对齐，复用）；
- GPU 资源：Qwen3-VL-Embedding 服务（vLLM，§16.2）；按需加载（§16.3 第3点：视觉 embedding 低频时按需加载）；
- poppler 渲染能力（M0 已引入 poppler，本里程碑用其 page→image）。

## 4. 架构与组件

视觉路作为**另一个 Retriever 实现**接入既有架构（契约③），输出仍归一到 `standard_id+clause_id`（契约④），生成端复用 ContextFragment（契约⑤）：

```text
用户问题 → 视觉 Embedding 编码 query
  → Milvus page_visual 召回(top-k 页面)
  → [可选] 视觉 Reranker
  → page → clause 映射(page_clause_map)
  → 回查 PG 条款文本与结构化表格(底座原则)
  → ContextFragment[] (契约⑤) → DeepSeek
```

新增组件：
- `PageRenderer`：poppler 把 PDF 页渲染为图像，落对象存储/文件系统；
- `VisualEmbeddingClient`：文本 query 与页面图像 → 同空间视觉向量（调 Qwen3-VL-Embedding 服务）；
- `VisualRetriever`（实现契约③）：query→`page_visual` 召回→page→clause 映射→Candidate；
- `PageClauseMapper`：依据 `page_clause_map` 把命中页面解析为所属条款集合；
- 入库侧 `VisualIngestPipeline`：渲染→视觉向量→写 `page_visual`。

## 5. 数据模型变更

- Milvus 新增 `page_visual` 集合（§8.3）：
  | 字段 | 类型 | 说明 |
  |---|---|---|
  | `page_id` | string | 页面 ID（主键） |
  | `standard_id` | string | 标准 ID |
  | `standard_no` | string | 标准号 |
  | `page_no` | int | 页码 |
  | `status` | string | 版本状态（标量过滤，§9.6 同样适用） |
  | `access_level` | string | 密级（标量过滤，§9.6 不可绕过） |
  | `image_path` | string | 页面图像路径 |
  | `visual` | float_vector | 视觉向量 |
- 文件系统/对象存储：新增 page image 存储目录（§4.4）。
- `page_clause_map`（§5.4，M2 已建）在 M7 被真正消费：`coverage_type` full/partial/table/figure 用于映射优先级。

## 6. 受影响的接口契约

| 契约 | 影响 |
|---|---|
| ① Parser | 不直接改；poppler 渲染能力新增（PageRenderer 独立于文本 Parser） |
| ② Embedding | 视觉 embedding 是**另一个实现维度**：新增 `VisualEmbeddingClient`（与文本 EmbeddingClient 并列，不混用同一向量空间） |
| ③ Retriever | 新增 `VisualRetriever` 实现，**形状不变**——这是文本路与视觉路能在 M9 融合的关键 |
| ④ Candidate | 视觉命中经 page→clause 映射后归一到 `standard_id+clause_id`；`source="visual"`；多页映射到同条款时合并证据（§10.2 第4点） |
| ⑤ ContextFragment | 不变：视觉路生成仍回到条款文本/表格，注入同一结构 |

## 7. 关键流程

### 7.1 视觉路入库（§8.3 page 级）
```text
选定必要文档(扫描件/图纸密集/复杂表/OCR差)
  → poppler 渲染页面图像
  → Qwen3-VL-Embedding 编码页面 → visual 向量
  → 写 Milvus page_visual (含 status/access_level 标量)
  → page_clause_map 已由 M2 提供
```

### 7.2 visual 模式检索生成（§9.3）
```text
query → 视觉 Embedding 编码
  → page_visual 召回(status+access_level 过滤下推)
  → [可选]视觉 Reranker
  → page → clause 映射
  → clause_id 回查 PG(权威源, status 二次校验)
  → 构造 ContextFragment → DeepSeek(仍引用条款号/表号)
```

## 8. 验收标准（绑定 §15 评估）

用 §15.2 中"图纸/扫描页/视觉问题"题型（5–10 条）+ 表格题：
- **视觉页命中率**（§15.3）：query 命中正确页面的比例达标；
- **页命中后条款映射成功率**（§15.3）：命中页面能正确映射到 gold_clause_no 的比例；
- **是否值得**（§15.1 第5项）：视觉路相比纯文本路，在视觉密集题上是否带来净增益——不达标则维持"仅对必要文档建视觉索引"或暂缓推广；
- 视觉路同样遵守 §9.6 状态+密级过滤（构造越权/废止样本验证不绕过）。

## 9. 风险

| 风险 | 应对 |
|---|---|
| 视觉页级过粗，命中页但无法准确定位条款（§18.1） | page_clause_map 提供映射；精度不足则推进 M8 block 级 |
| 多个 8B 模型常驻显存压力 | 视觉 embedding 低频按需加载，视觉 reranker 先不用（§16.3） |
| 视觉路绕过密级/状态过滤致泄露或误用 | status/access_level 写入 page_visual 标量并下推 + PG 二次校验（§9.6） |
| 对全量文档建视觉索引成本高 | 只对必要文档建索引（§16.3 第6点 / §17.3） |
| query 与页面图像不在同一向量空间致召回差 | Qwen3-VL-Embedding 统一编码 query 与页面 |

## 10. 开放问题

1. 页面渲染 DPI / 图像尺寸与视觉向量质量、存储成本的权衡？
2. "必要文档"的判定规则（按 OCR 置信度阈值自动挑 vs 人工标注）？
3. 视觉 Reranker 是否在 M7 就引入，还是按 §16.3 先不用、M8 再加？
4. page→clause 映射在 partial/table/figure 覆盖类型下的优先级与去重策略。
