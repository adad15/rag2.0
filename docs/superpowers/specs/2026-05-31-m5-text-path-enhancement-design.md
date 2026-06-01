# M5 文本路增强 —— 设计文档

- 日期：2026-05-31
- 类型：里程碑设计 spec（M5）
- 上游：[总览路线图](2026-05-31-rag-overview-roadmap-design.md)、[技术设计文档整合版](../../规范文档rag系统技术设计文档_整合版.md)
- 对应文档章节：§9.7、§10.3、§10.5、§11.5、§12.4、§16.5
- 前置里程碑：M4（评估闭环必须存在——M5 每项增强都要用评估集验证增益）

---

## 1. 定位与目标

M5 在 M0–M4 跑通的文本路 MVP 之上，把"召回到 → 答得准"这段加厚。核心命题：**不靠主观判断，每项增强都要在 M4 评估集上证明对"条款命中率 / 数值准确率 / 答案完整性"有正增益且不引入噪声**（呼应 §15.1）。

五项增强：
1. 文本 Reranker（§10.3）
2. 表格 cell 级结构化定位子流程（§9.7）
3. 一跳引用图扩展回填（§10.5）
4. LLM 离线元数据增强（§12.4）
5. 上下文 token 预算（§11.5）

并入一项基础设施收尾：把 M1 的临时云 embedding 切换到正式 **Qwen3-Embedding-8B**（§16.5 灰度迁移，复用接口②）。

## 2. 范围

**做：** 上述 5 项增强 + embedding 正式化（灰度）。每项独立可开关（feature flag），便于评估对照（A/B）。

**不做：** 视觉路（M7+）、both/auto（M9）、版本生命周期完整流程（M6，本里程碑只依赖 M3 已落地的"默认现行过滤"）。Reranker 的"低置信度才触发"成本优化（§10.3 末行）作为可选项，非本里程碑硬目标。

## 3. 前置依赖

- M4 评估集（≥100 条，含表格/限值/引用类题与"应拒答"难例）；
- M2 已建好 `spec_tables` / `spec_table_cells`（cell 级定位的数据基础）；
- M2/M3 已落地 `clause_nodes.refs`、`standards.replaces/replaced_by`（引用扩展的数据基础）；
- GPU 资源到位（部署 Qwen3-Reranker-8B、Qwen3-Embedding-8B via vLLM）。

## 4. 架构与组件

在 M3 的检索链（三路召回 → RRF）与生成端（ContextFragment → DeepSeek）之间插入新组件，**不改既有接口契约的形状，只新增实现与一个新接口**：

```text
查询理解(M3) → 三路召回(M3) → RRF(M3)
   → [新] Reranker（top20→top5）
   → [新] 引用图扩展（一跳，拉 refs 被引条款作辅助证据）
   → small-to-big 回填(M3)
   → [新] 表格 cell 级定位（命中表→定位单元格→注入紧凑片段）
   → [新] token 预算分配/裁剪
   → ContextFragment[] (契约⑤) → DeepSeek
```

新增组件：
- `Reranker`（新接口，见 §6）+ `text_reranker` 实现（调 Qwen3-Reranker-8B 服务）；
- `ReferenceExpander`：解析命中条款 `refs`，沿引用边一跳扩展；
- `TableCellLocator`：在 `spec_table_cells` 上按 行/列表头/单位 结构化过滤，定位 `number_value`+`unit`；
- `ContextBudgeter`：按优先级分配/裁剪上下文 token；
- `MetadataEnricher`（离线后台任务，非查询链路）：调 DeepSeek 产条款摘要/适用条件/关键词/常见问法，写回 PG。

## 5. 数据模型变更

- 新增 `clause_metadata`（LLM 离线增强结果，与条款一对一/一对多）：
  | 字段 | 说明 |
  |---|---|
  | `node_id` | 关联条款 |
  | `summary` | 条款摘要 |
  | `applicable_condition` | 适用条件 |
  | `keywords` | 关键词（用于 BM25 扩展/检索文本增强） |
  | `common_questions` | 常见问法（可回写入 retrieval_text） |
  | `enrich_model_version` | 生成模型版本（§14.3 追溯） |
  | `enriched_at` | 生成时间 |
- `retrieval_chunks.retrieval_text` 可在离线增强后重生成（关键词/常见问法并入），触发受影响条款的向量重建（与 M6 局部重建机制对齐）。
- embedding 迁移：新 collection（或 `index_version` 区分），不破坏旧索引（§16.5）。

## 6. 受影响的接口契约

| 契约 | 影响 |
|---|---|
| ② Embedding | 新增 `qwen_embedding` 实现替换云实现；调用方不动；维度变化走新 collection 灰度 |
| ③ Retriever | 不变。Reranker 作用在 Retriever 输出的候选列表之后，是**候选列表→候选列表**的新接口 |
| ④ Candidate | 引用扩展进来的候选用同一 `standard_id+clause_id` 键去重；新增 `is_aux_evidence` 标记（辅助证据，引用归属仍以原命中条款为主） |
| ⑤ ContextFragment | `tables` 字段开始被填充（紧凑 Markdown 片段 / cell 命中行列），不改 schema 形状 |

新增接口 `Reranker`：
```text
Reranker::rerank(query, candidates[], top_k) -> candidates[]   // 重打分并截断
```

## 7. 关键流程

### 7.1 表格 cell 级定位（§9.7）
```text
查询理解抽取(材料/等级/指标/单位条件)
  → dense+BM25 召回 table caption/table_summary → 命中 table_id
  → 在 spec_table_cells 按 row_header/col_header/unit 过滤
  → 命中唯一单元格: 取 number_value+unit+merged_info 注入上下文
  → 命中不唯一: 回退注入紧凑 Markdown 表片段(不塞完整 HTML)
  → 命中的数值/单位显式带入，供 §11.4 数值后置校验硬匹配
```

### 7.2 一跳引用图扩展（§10.5）
```text
命中条款 → 解析 refs → 沿引用边一跳拉被引条款/表格(辅助证据)
  → 跨标准引用同样过 §9.6 status+access_level 过滤
  → 受 token 预算约束(深度=1)
  → 被引标准已废止而原条款现行: 按 §13 提示版本风险, 不静默采用
```

### 7.3 token 预算（§11.5）
优先级（高→低，超预算自低向高裁剪，命中条款原文不可裁剪）：
```text
命中条款原文 > 表格相关行列 > 父级节上下文 > 一跳引用条款
```

### 7.4 embedding 灰度迁移（§16.5）
新模型并行建索引 → 评估集对比新旧 → 达标按 `index_version` 灰度放量 → 保留回滚 → 迁移期双写/排队 → 完成清理旧索引。

## 8. 验收标准（绑定 §15 评估）

每项增强做 A/B（开关对照），在 M4 评估集上：
- **Reranker**：top-1/top-3 条款命中率较 RRF-only 提升，且无明显误拒上升；
- **表格 cell 定位**：限值/数值/单位题的"数值和单位准确率"显著提升；
- **引用扩展**：答案完整性提升，且对噪声/误拒无明显负作用（§15.1 专列项）；
- **LLM 元数据**：开启关键词/常见问法后语义召回 recall@k 提升；
- **token 预算**：在不降命中率前提下，上下文长度可控、无 Context 溢出；
- **embedding 升级**：Qwen3-Embedding-8B 索引在评估集上不劣于临时索引方可切流。
- 所有阈值/模型版本变化记入 §14.3 追溯信息；拒答阈值随之重标定（§11.4）。

## 9. 风险

| 风险 | 应对 |
|---|---|
| Reranker/引用扩展引入噪声反降指标 | 强制 A/B 评估，达不到增益就不上线（YAGNI） |
| 多个 8B 模型常驻显存压力（§16.3） | embedding 常驻、reranker 按需触发、低置信度才 rerank |
| LLM 离线增强成本/慢 | 后台任务、不阻塞主入库（§12.4）；分批限速 |
| cell 定位因合并单元格取错限值 | 依据 `merged_info` 还原表头归属（§9.7 要点3） |
| embedding 迁移期数据不一致 | 双写/排队 + index_version 灰度 + 回滚 |

## 10. 开放问题

1. Qwen3-Reranker-8B / Qwen3-Embedding-8B 的部署形态与 GPU 配额？
2. embedding 维度是否截短到 1024/2048（§16.3）以控显存？需评估对比。
3. LLM 元数据增强的触发范围（全量 vs 高频/失败 case 优先）与预算上限。
4. Reranker 是否一上线就用"低置信度才触发"的成本优化，还是先全量再优化。
