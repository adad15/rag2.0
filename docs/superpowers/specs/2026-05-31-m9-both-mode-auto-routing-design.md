# M9 both 模式 + auto 路由 —— 设计文档

- 日期：2026-05-31
- 类型：里程碑设计 spec（M9）
- 上游：[总览路线图](2026-05-31-rag-overview-roadmap-design.md)、[技术设计文档整合版](../../规范文档rag系统技术设计文档_整合版.md)
- 对应文档章节：§9.1、§9.4、§9.5、§10.1（跨模态融合）、§10.2（去重）、§17.5
- 前置里程碑：M5（文本路增强/reranker）+ M7（视觉路，至少 page 级）

---

## 1. 定位与目标

M9 是收尾里程碑：把文本路与视觉路**合并**为 both 模式，并实现 **auto 路由**让系统自动选路。核心纪律（§18.1 / §9.4）：**both 最全但最贵，必须经评估验证确实优于单路才设为默认**，否则 auto 默认仍以文本路为主。

目标（§17.5）：
1. auto 路由（§9.5 规则）；
2. text + visual 并行召回；
3. clause_id 级融合去重（§10.2）；
4. both 模式评估；
5. 依评估结果决定默认策略。

## 2. 范围

**做：** 四种模式统一入口（auto/text/visual/both，§9.1）、auto 路由判定、并行召回编排、跨模态融合（分数归一化 / 统一多模态 reranker，§10.1）、clause_id 去重合并、both 评估。

**不做：** 新召回能力（文本路属 M3/M5，视觉路属 M7/M8）；本里程碑只做"编排与融合"。

## 3. 前置依赖

- M3 的文本路三路召回 + RRF + 查询理解（auto 路由复用查询理解的解析结果）；
- M5 的文本 reranker（融合时可作统一打分器候选）；
- M7（及可选 M8）的视觉路 VisualRetriever；
- §9.6 统一过滤在两路都已生效；
- M6 的降级链（视觉服务不可用时 both/visual 自动退回 text，§16.4）。

## 4. 架构与组件

M9 不新增检索器，而是在多个 Retriever（契约③）之上加**路由器 + 融合器**。这正是从 M1 就钉死契约③④的目的：

```text
query → 查询理解(M3)
  → QueryRouter(auto) 判定: text / visual / both
  ├─ text:   文本三路召回(M3) → RRF → reranker(M5)
  └─ visual: VisualRetriever(M7/M8)
  → CrossModalFuser: clause_id 归一化 → 去重合并 → 分数归一化/统一 reranker
  → small-to-big + 引用扩展 + token 预算(M5)
  → ContextFragment(契约⑤) → DeepSeek
```

新增组件：
- `QueryRouter`：按 §9.5 规则与查询理解结果选路，含 fallback；
- `CrossModalFuser`：跨模态候选融合——分数归一化或交给统一多模态 Reranker（§10.1 注意项），按 `standard_id+clause_id` 去重合并证据；
- `ModeController`：统一四模式入口，串接降级（§16.4）。

## 5. 数据模型变更

无新增表。M9 是编排层，复用 M3/M5/M7/M8 的索引与映射。可能新增：
- 路由决策日志（query 特征 → 选路 → 各路命中 → 是否采纳），用于 §15.5 在线回流与 both 增益分析；
- 评估侧记录 both vs 单路的对照指标。

## 6. 受影响的接口契约

| 契约 | 影响 |
|---|---|
| ③ Retriever | 不新增实现；`QueryRouter`/`CrossModalFuser` 作用在多个 Retriever 输出之上 |
| ④ Candidate | **本里程碑的核心**：文本/视觉候选统一用 `standard_id+clause_id` 去重合并（§10.2 第4点：两路命中同条款合并证据）；`source` 区分来源用于融合权重/归因 |
| ⑤ ContextFragment | 不变；融合后的证据仍注入同一结构，归因标注命中来源 |

> 从 M1 起即采用契约④的归一化键，使 M9 能直接在 clause_id 层融合，无需回溯改造——这是 walking skeleton + 早钉契约的回报。

## 7. 关键流程

### 7.1 auto 路由规则（§9.5）
```text
含标准号/条款号                  → text
含"表/限值/指标/单位"            → text + table 子流程(§9.7)
含"图/示意图/流程图/第几页"      → visual 或 both
命中文档 OCR 质量低              → visual
text 召回置信度低                → fallback visual
visual 映射条款不清              → fallback text
```

### 7.2 both 模式融合（§9.4 / §10.1 / §10.2）
```text
text 候选条款 ┐
              ├ clause_id 归一化 → 去重合并证据 → 融合 → 重排 → 生成
visual 命中页/块→映射条款 ┘

跨模态融合: 文本向量与视觉向量分布差异大,
  仅 RRF 可能稀释某一路 → 增加分数归一化, 或统一交多模态 Reranker 打分(§10.1)
```

### 7.3 降级（§16.4，复用 M6）
```text
视觉服务不可用 → both/visual 自动退回 text
```

## 8. 验收标准（绑定 §15 评估）

建立 both 模式评估（§17.5 第4点）：
- **both vs text-only vs visual-only** 三者在 §15.2 全题型上的对照（recall@k、条款命中率、答案正确性、数值准确率）；
- **决策闸**：仅当 both 在目标题型上**净增益显著且成本可接受**时，才把 auto 的默认倾向调向 both；否则 auto 默认以 text 为主、visual 仅在 §9.5 触发条件下介入（§18.1：both 不一定提升，可能只是更贵）；
- **auto 路由准确率**：用带 gold 路由标注的样本评估路由判定正确率与 fallback 触发合理性；
- 融合后仍遵守 §9.6 过滤与 §13.4 PG 权威校验。

## 9. 风险

| 风险 | 应对 |
|---|---|
| both 不一定提升、只是更贵（§18.1） | 评估闸把关，不达标不设默认；auto 默认偏文本路 |
| 跨模态 RRF 稀释视觉线索（§10.1） | 分数归一化或统一多模态 reranker 打分 |
| auto 路由误判致召回路线错 | §9.5 规则 + 双向 fallback（text↔visual） |
| both 模式延迟翻倍 | 延迟预算（§16.4）+ 仅必要时触发 both |
| 同条款跨模态重复证据 | clause_id 去重合并（契约④） |

## 10. 开放问题

1. 跨模态融合最终选"分数归一化 + RRF"还是"统一多模态 Reranker 打分"？需评估对比。
2. auto 路由判定用规则即可，还是需要轻量分类器？先规则、按评估决定。
3. both 模式的触发门槛（哪些查询特征才值得双路并行）以控成本。
4. 默认模式最终取值（auto 偏 text vs 偏 both），由 both 评估结果定（§17.5 第5点）。
