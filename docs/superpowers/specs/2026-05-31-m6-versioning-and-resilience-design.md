# M6 版本管理与运维健壮性 —— 设计文档

- 日期：2026-05-31
- 类型：里程碑设计 spec（M6）
- 上游：[总览路线图](2026-05-31-rag-overview-roadmap-design.md)、[技术设计文档整合版](../../规范文档rag系统技术设计文档_整合版.md)
- 对应文档章节：§13（版本管理）、§16.4（服务韧性/延迟预算）、§16.5（embedding 迁移）、§9.6（统一过滤一致性）
- 前置里程碑：M3（已落地"默认只召回现行"的状态过滤与密级过滤）

---

## 1. 定位与目标

M6 把"规范 RAG 的底线能力"补全。规范 RAG 最大风险是**误用废止标准**（§13.1）与**密级越权**（§9.6），M3 已落地最基础的"默认现行过滤"，M6 补全完整的版本生命周期、条款级修订、PG↔Milvus 一致性，以及多服务串联下的降级与延迟健壮性。

目标：
1. 完整版本状态流程：新增 / 改版 / 废止（§13.3）；
2. 条款级版本管理：局部修订 / 修改单（§13.3）；
3. PG↔Milvus 一致性保障（§13.4）；
4. 分级降级链 + 延迟预算 + 缓存（§16.4）；
5. embedding 模型升级与索引灰度迁移机制固化（§16.5，与 M5 共享）。

## 2. 范围

**做：** 版本生命周期的状态机与流程、条款级修订、一致性对齐任务、查询时以 PG 为准的二次校验、降级链、延迟预算埋点、缓存层。

**不做：** 视觉路相关版本（视觉索引一致性在 M7/M8 各自处理，但复用本里程碑的对齐框架）；新检索能力（属 M3/M5）。

## 3. 前置依赖

- M3 的 §9.6 统一过滤（status + access_level 已下推 Milvus 标量 + PG 条件）；
- M2 的 `standards`（版本字段）与 `clause_nodes`（需支持条款级 status 与修订指向）；
- M0 的外部服务（Milvus/embedding/DeepSeek）已可被超时/重试封装。

## 4. 架构与组件

新增组件，多为后台任务与查询链路上的校验钩子，不改检索/生成主形状：

- `VersionManager`：执行新增/改版/废止/修改单四类状态变更流程，维护 `replaces/replaced_by/supersedes`；
- `ConsistencyReconciler`（后台任务）：定期校验 PG 与 Milvus 标量字段（status/access_level）一致性，异步对齐/补偿（§13.4）；
- `AuthorityGuard`（查询链路钩子）：召回候选后用 `clause_id` 回查 PG 最新 status/access_level 二次校验，任一不通过即剔除（§9.6 要点3、§13.4）；
- `ResilienceLayer`：每个外部服务的超时/重试封装 + 分级降级链 + 延迟预算埋点；
- `CacheLayer`：query embedding 缓存、最终上下文缓存、标准号/条款号精确结果短期缓存。

## 5. 数据模型变更

- `standards` 启用并强约束版本字段（§13.2）：`publish_date / effective_date / abolish_date / replaces / replaced_by / status_source / status_checked_at`；状态枚举：现行 / 作废 / 即将实施。
- `clause_nodes` 增条款级版本能力（§13.3 局部修订要点1）：
  | 字段 | 说明 |
  |---|---|
  | `status`（条款级） | 现行 / 被修订 / 作废，默认随标准继承 |
  | `supersedes` | 指向被本条款替换的旧条款 node_id |
  | `revision_source` | 修订来源（修改单编号） |
  | `revision_effective_date` | 修订生效日期 |
- Milvus `clause_text` 标量字段 `status` / `access_level` 由 `ConsistencyReconciler` 与 PG 对齐；缓存键须含 `access_level` 与 `index_version`（§16.4 要点4，避免越权命中/读旧索引）。

## 6. 受影响的接口契约

| 契约 | 影响 |
|---|---|
| ② Embedding | 升级走 §16.5 灰度（与 M5 共享 `index_version`）；调用方不动 |
| ③ Retriever | 不变；`AuthorityGuard` 作为检索后置过滤钩子，输入输出仍是候选列表 |
| ④ Candidate | 增 `effective_status`（PG 二次校验后的权威状态）用于剔除/提示 |
| ⑤ ContextFragment | `status` 字段以 PG 二次校验结果为准；废止条款进入上下文时附版本风险提示 |

`ResilienceLayer` 不改契约，是对所有外部调用的横切包装。

## 7. 关键流程

### 7.1 版本状态流程（§13.3）
```text
新增: 新文件→解析→质检→入 PG→建索引→标记 现行/即将实施
改版: 新版入库→旧版 status=作废→建 replaces/replaced_by→重建相关索引
废止: PG status=作废→Milvus 标量同步→默认召回过滤
```

### 7.2 局部修订 / 修改单（条款级，§13.3）
```text
修改单入库→定位受影响 clause_id
  → 旧条款节点 status=作废/被修订, 记 revision_source+生效日期
  → 写新条款节点, 建 supersedes 指向旧条款
  → 仅对受影响条款重建 retrieval_chunks 与向量索引(最小化)
  → 标准整体 status 保持 现行
  → 默认只召回最新现行条款; 查历史条款显式放开, 回答提示"该条款已被 X 年修改单替换"
```

### 7.3 一致性保障（§13.4）
```text
异步对齐: 后台定期校验 PG↔Milvus 状态一致性, 失败补偿
查询时以 PG 为准: Milvus 召回候选后, 经 clause_id 回查 PG 最新 status,
  已作废/删除即使 Milvus 未清理也必须过滤
密级硬底线: access_level 禁止依赖可能滞后的 Milvus 字段单独裁决, 必须 PG 二次校验
```

### 7.4 分级降级链（§16.4）
```text
Reranker 不可用      → 退回仅 RRF 排序
视觉服务不可用       → both/visual 自动退回 text
Embedding 不可用     → 退回 BM25 + PG 精确匹配, 提示语义召回降级
DeepSeek 生成失败    → 返回"检索到的规范原文 + 无法生成"降级回答, 不整体报错
```

## 8. 验收标准（绑定 §15 评估）

- **废止标准误用率**（§15.4）：默认查询下趋近 0；改版后旧版自动不被召回；
- **标准版本准确率**：回答标注的版本与 PG 权威状态一致；
- **修改单场景**：被修订条款默认召回最新现行条款，旧条款仅在显式放开时出现并带提示；
- **密级过滤**：构造越权样本，确认任何路径（含 fallback）都被 PG 二次校验拦截；
- **韧性**：分别注入各外部服务故障，确认走对应降级而非整体不可用；P95 延迟在预算内或超预算阶段触发降级；
- **回归保护**：升级前后跑同一评估集做回归对比（§15.5 / §17.2）。

## 9. 风险

| 风险 | 应对 |
|---|---|
| 修改单漏更新致现行标准里残留旧条款被召回 | 条款级 status + supersedes + 最小化重建（§18.1 对应行） |
| PG 已更新 Milvus 未更新致召回脏数据 | 异步对齐 + 查询时 PG 为准（§13.4） |
| 密级依赖滞后 Milvus 字段致越权 | 禁止单独裁决，强制 PG 二次校验（安全底线） |
| 服务级联失败 | 分级降级 + 延迟预算（§16.4） |
| 缓存读到旧索引/越权命中 | 缓存键含 access_level + index_version |

## 10. 开放问题

1. `status_source` / `status_checked_at` 的维护方式（人工核验 vs 外部数据源）？
2. `ConsistencyReconciler` 的对齐周期与失败补偿策略（实时触发 vs 定时批量）？
3. 缓存 TTL：精确查询结果短期缓存的具体时长？
4. 延迟预算 P95 目标与各阶段（embedding/召回/rerank/生成）预算切分的具体数值，需上线后实测标定。
