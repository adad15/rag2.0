# 道路桥隧工程知识库扩展设计

- 日期：2026-06-06
- 类型：补充技术设计文档
- 关系：作为现有《规范文档 RAG 系统技术设计文档》的长期扩展说明
- 状态：草案，待确认后可拆分为实施计划

---

## 1. 定位变化

现有系统定位是“规范文档 RAG 系统”，核心目标是把工程规范解析成条文级知识库，并要求回答可溯源到标准号、规范名称、版本、条款号。

长期目标应扩展为：

```text
道路桥隧工程知识库系统
```

规范、标准、规程仍然是最高权威资料，但不再是唯一资料。系统需要同时支持：

1. 道路、桥梁、隧道相关规范、标准、规程；
2. 技术指南、设计手册、施工手册；
3. PPT、培训课件、会议材料；
4. 技术文档、施工方案、技术交底、专项方案；
5. 病害处治案例、检测报告、项目总结；
6. 图表、流程图、构造图、照片等多模态资料。

扩展后的系统不能退化为普通文档问答。它仍然需要保留规范库的严谨性，但要允许非规范资料作为解释、补充和经验参考。

---

## 2. 核心原则

### 2.1 规范是权威底座，其他资料是补充层

回答中必须区分资料权威等级：

| 权威等级 | 资料类型 | 回答中的定位 |
|---|---|---|
| `normative` | 规范、标准、规程 | 可作为强依据，必须引用标准号和条款号 |
| `guideline` | 指南、手册、技术导则 | 可作为技术解释依据，需引用章节/页码 |
| `project_doc` | 施工方案、技术交底、专项方案 | 只能作为项目做法参考 |
| `training` | PPT、培训课件 | 只能作为辅助说明，不替代规范 |
| `case` | 案例、检测报告、项目总结 | 只能作为经验参考，需提示适用条件 |
| `web_or_misc` | 网页、零散资料 | 默认低权重，需人工确认来源可靠性 |

当规范资料与其他资料冲突时，系统默认以现行规范为准。

### 2.2 不同文档类型使用不同入库 profile

不能把所有文档都按条文切分，也不能把规范按普通字符滑窗切分。每类文档应有独立 profile：

| Profile | 适用资料 | 主切片单位 |
|---|---|---|
| `standard_profile` | 规范、标准、规程 | 条、款、项、表格 cell |
| `guide_profile` | 指南、手册、技术导则 | 章、节、小节、技术主题 |
| `ppt_profile` | PPT、培训课件 | 页、标题块、图表块 |
| `project_doc_profile` | 施工方案、交底、专项方案 | 工序、检查项、风险点、措施段 |
| `case_profile` | 病害案例、检测报告、项目总结 | 问题、原因、检测结论、处治措施 |

Profile 决定解析器、切片规则、metadata 抽取、检索权重和回答约束。

### 2.3 PostgreSQL 仍然是结构化底座

Milvus 只负责召回，权威字段和资料状态仍以 PostgreSQL 为准。扩展后 PG 不应只保存 `clause_nodes`，而应抽象为通用文档节点体系。

规范条文是节点的一种，PPT 页、技术章节、施工步骤、案例问题也都是节点。

---

## 3. 目标架构

```text
文件入库
  ↓
文档类型识别
  ↓
选择 DocumentProfile
  ↓
解析服务 PDF / OCR / DOCX / PPTX / HTML / 图片
  ↓
统一 IR：DocumentElement
  ↓
Profile 专属结构化
  ↓
DocumentNode / RetrievalChunk / Table / Figure / Entity / Relation
  ↓
PostgreSQL 结构化底座
  ↓
Embedding + BM25 + metadata
  ↓
Milvus / PG 精确检索
  ↓
多路召回 + RRF + Reranker
  ↓
按权威等级组织上下文
  ↓
DeepSeek 生成 + 溯源校验
```

---

## 4. 通用数据模型

### 4.1 documents

保存每份资料的全局信息。

| 字段 | 说明 |
|---|---|
| `document_id` | 内部文档 ID |
| `document_type` | standard / guide / ppt / project_doc / case / misc |
| `authority_level` | normative / guideline / training / project_doc / case / web_or_misc |
| `title` | 文档标题 |
| `source_path` | 原始文件路径 |
| `source_org` | 发布单位、项目单位或资料来源 |
| `publish_date` | 发布日期 |
| `version` | 版本 |
| `status` | 现行、作废、草案、内部资料等 |
| `discipline` | road / bridge / tunnel / pavement / geotech / traffic / material |
| `project_scope` | 可选，项目名、标段、地区 |
| `profile` | standard_profile / ppt_profile 等 |
| `parse_version` | 解析版本 |
| `created_at` | 入库时间 |

### 4.2 document_nodes

替代单一的 `clause_nodes`，作为统一节点表。规范条文仍可兼容映射到此表。

| 字段 | 说明 |
|---|---|
| `node_id` | 节点 ID |
| `document_id` | 所属文档 |
| `parent_id` | 父节点 |
| `node_type` | clause / section / slide / table / figure / procedure_step / risk / case_problem / case_solution |
| `node_no` | 条款号、章节号、页码或步骤号 |
| `title` | 标题 |
| `path` | 层级路径 |
| `text` | 节点原文 |
| `page_start` | 起始页 |
| `page_end` | 结束页 |
| `bbox` | 页面坐标，可选 |
| `authority_level` | 从 documents 继承，也可节点级覆盖 |
| `status` | 从 documents 继承，也可节点级覆盖 |
| `metadata_json` | 扩展元数据 |

### 4.3 retrieval_chunks

仍采用三文本分离，但适配多文档类型：

| 字段 | 说明 |
|---|---|
| `chunk_id` | 检索块 ID |
| `node_id` | 对应节点 |
| `document_id` | 所属文档 |
| `atomic_text` | 原始最小知识单元 |
| `retrieval_text` | 用于召回的扩展文本 |
| `context_text` | 用于生成的上下文 |
| `chunk_type` | clause / section / slide / table / figure / procedure / case |
| `authority_level` | 权威等级 |
| `metadata_json` | 主题、实体、工序、指标、病害等 |

`retrieval_text` 应按 profile 生成：

```text
规范：标准号 + 标准名称 + 章节路径 + 条款号 + 条文 + 关键词 + 单位
指南：文档名 + 章节路径 + 技术主题 + 正文 + 适用条件
PPT：课程名 + 页标题 + 页码 + 图表说明 + OCR 文本
施工方案：项目类型 + 工序 + 质量控制点 + 风险 + 措施
案例：病害类型 + 原因 + 检测结论 + 处治措施 + 适用条件
```

### 4.4 engineering_entities

用于道路桥隧领域实体抽取。

| 实体类型 | 示例 |
|---|---|
| `structure` | 路基、路面、桥梁、隧道、涵洞、边坡 |
| `component` | 桥面铺装、伸缩缝、衬砌、防水层、基层 |
| `material` | 沥青混合料、水泥稳定碎石、钢筋、混凝土 |
| `disease` | 车辙、裂缝、沉陷、渗漏、空洞、剥落 |
| `process` | 摊铺、碾压、注浆、喷射混凝土、张拉 |
| `test_method` | 压实度试验、弯沉检测、回弹法、钻芯法 |
| `indicator` | 压实度、厚度、强度、渗水系数、平整度 |
| `measure` | 铣刨重铺、灌缝、注浆加固、换填 |

### 4.5 engineering_relations

用于轻量知识图谱，不要求一开始引入图数据库。

| 关系 | 示例 |
|---|---|
| `requires` | 工序需要材料/设备 |
| `has_indicator` | 结构或材料有检测指标 |
| `uses_test_method` | 指标使用某试验方法 |
| `causes` | 原因导致病害 |
| `treats` | 措施处治病害 |
| `cites` | 文档引用规范或条款 |
| `supersedes` | 新规范替代旧规范 |
| `applies_to` | 条款或措施适用于对象/条件 |

---

## 5. Profile 设计

### 5.1 standard_profile

沿用当前规范库路线：

1. 解析 PDF/OCR 得到 elements；
2. 建条款层级树；
3. 保留标准号、条款号、版本、现行状态、强制性标记；
4. 生成 atomic / retrieval / context 三文本；
5. 表格进入 `spec_tables` / `spec_table_cells`；
6. 默认作为最高权威来源。

当前 M2c 仍应优先完成，不因扩展而推倒。

### 5.2 guide_profile

适合技术指南、设计手册、施工手册。

切片规则：

1. 以章、节、小节为主；
2. 小节过长时按主题段落拆分；
3. 表格、图、公式挂到最近章节节点；
4. 保留页码和标题路径。

metadata：

1. 适用对象：道路、桥梁、隧道、路面、路基等；
2. 技术主题：设计、施工、检测、养护、病害处治；
3. 指标和单位；
4. 适用条件；
5. 引用规范。

回答中定位为“技术解释依据”，不得覆盖规范条文。

### 5.3 ppt_profile

适合 PPT、培训课件、会议材料。

切片规则：

1. 每页至少一个 slide node；
2. 页标题作为主索引字段；
3. 图表、流程图、构造图可作为 block node；
4. OCR 文本与备注文本分别保存；
5. 图片路径和 bbox 保留给视觉路。

metadata：

1. 课程名或材料名；
2. 页标题；
3. 主题标签；
4. 图表说明；
5. 涉及对象、病害、工艺、指标。

回答中定位为“辅助理解材料”。如果 PPT 内容与规范冲突，必须以规范为准。

### 5.4 project_doc_profile

适合施工方案、技术交底、专项方案、作业指导书。

切片规则：

1. 按工程部位、工序、步骤拆分；
2. 质量控制、风险、安全、验收要求单独成节点；
3. 表格中的检查项和参数结构化；
4. 保留项目、标段、地区、时间等上下文。

metadata：

1. 项目类型；
2. 工程部位；
3. 工艺流程；
4. 质量控制点；
5. 风险点；
6. 检查指标；
7. 适用条件。

回答中定位为“项目做法参考”，不能当通用规范依据。

### 5.5 case_profile

适合病害报告、检测报告、养护处治案例、项目总结。

切片规则：

1. 问题现象；
2. 检测结论；
3. 原因分析；
4. 处治措施；
5. 效果评价。

metadata：

1. 病害类型；
2. 道路/桥梁/隧道对象；
3. 材料和结构层；
4. 环境条件；
5. 检测方法；
6. 处治方法；
7. 效果。

回答中定位为“经验参考”，必须提示案例条件差异。

---

## 6. 检索与回答策略

### 6.1 查询理解

查询理解层需要识别：

1. 是否要求规范依据；
2. 是否是技术解释；
3. 是否是施工做法；
4. 是否是病害处治；
5. 是否涉及表格、限值、单位；
6. 是否需要图片、构造图、流程图；
7. 是否限定道路/桥梁/隧道专业。

示例：

```text
“沥青路面车辙怎么处理？”
→ case + guide + standard

“桥梁伸缩缝安装有什么规范要求？”
→ standard 优先，guide 补充

“隧道二衬渗漏有哪些处治办法？”
→ guide + case + project_doc，若有规范则补充规范依据

“压实度检测频率是多少？”
→ standard + table cell 优先
```

### 6.2 多路召回

建议召回顺序：

1. PG 精确召回：标准号、条款号、文档名、页码；
2. BM25：关键词、专业术语、指标、病害；
3. Dense：语义相似；
4. 表格 cell：限值、单位、指标；
5. 视觉路：图片、图表、PPT 页、构造图；
6. 实体关系扩展：病害、材料、工序、指标关联。

### 6.3 权威等级排序

候选融合时加入权威权重：

```text
normative > guideline > project_doc > case > training > web_or_misc
```

但这不是绝对排序。对“经验做法”类问题，case 和 project_doc 可以排前；对“规范要求”类问题，normative 必须排前。

### 6.4 生成上下文分组

回答上下文不应只是一组 chunk，而应按来源分组：

```json
{
  "normative_evidence": [],
  "technical_reference": [],
  "project_reference": [],
  "case_reference": [],
  "training_reference": []
}
```

生成回答时按以下结构输出：

```text
1. 规范依据
2. 技术解释
3. 可参考做法
4. 适用条件与风险
5. 来源
```

当没有规范依据时，应明确说明：

```text
未检索到可直接作为规范依据的条文。以下内容仅来自技术资料/案例，不能替代现行规范。
```

---

## 7. 对当前项目的影响

### 7.1 当前 M2c 不需要推翻

当前 M2c 的结构化建树仍然是优先级最高的工作。规范资料是整个知识库的权威层，必须先做扎实。

M2c 后续应从“只生成 clause tree”扩展成：

```text
standard_profile 的专属结构化实现
```

这样后续新增 PPT、技术文档、案例时，不会和规范切片规则混在一起。

### 7.2 Parser 层需要扩展为多格式

当前已有 `Parser` 抽象，后续可加：

1. `PdfParser` / `HybridParser`；
2. `PptParser`；
3. `DocxParser`；
4. `HtmlParser`；
5. `ImageParser`。

所有解析器输出统一 `ParsedDoc / ParseElement`，但需要补充字段：

| 字段 | 用途 |
|---|---|
| `bbox` | 页面或 slide 内定位 |
| `block_id` | block 级节点 |
| `reading_order` | 阅读顺序 |
| `image_path` | 图表或页面截图 |
| `notes_text` | PPT 备注 |
| `element_metadata_json` | 扩展字段 |

### 7.3 Ingest 层需要 profile router

新增：

```text
DocumentProfileRouter
```

根据文件类型、文件名、首页信息、用户选择判断 profile。

Profile router 不应完全自动。工程资料来源复杂，应允许用户在入库时手动指定：

```text
--profile standard
--profile guide
--profile ppt
--profile project_doc
--profile case
```

自动判断只作为默认建议。

---

## 8. 分阶段路线

### 阶段 A：完成规范底座

目标：

1. 完成 M2c-1 条款树；
2. 完成 M2c-2 三文本；
3. 完成 M2c-3 落库和替换 M1 `split_clauses`；
4. 补齐 `retrieval_chunks`、`page_clause_map`、表格基础结构；
5. M3 上线 dense + BM25 + PG 精确 + RRF。

这是所有扩展的前置基础。

### 阶段 B：技术文档 profile

先扩展 guide / project_doc，不急于做 PPT。

理由：

1. 技术文档与规范同为 PDF/DOCX，解析链路复用度高；
2. 章节级切片比 PPT 视觉块简单；
3. 对道路桥隧问答提升明显；
4. metadata 可以直接服务后续知识图谱。

新增能力：

1. 文档类型识别；
2. 章节级树；
3. 技术主题 metadata；
4. authority_level 分层回答；
5. 技术资料作为补充来源。

### 阶段 C：案例与病害知识

新增 case_profile：

1. 病害现象；
2. 原因分析；
3. 检测方法；
4. 处治措施；
5. 适用条件。

这一阶段开始形成道路桥隧领域知识图谱雏形。

### 阶段 D：PPT 与视觉资料

新增 ppt_profile：

1. slide node；
2. 图表 block；
3. OCR + 备注；
4. page image / block image；
5. 视觉召回。

PPT 适合作为教学和解释资料，不应作为早期核心。

### 阶段 E：轻量知识图谱

在 PG 中先做实体关系表，不急于上 Neo4j。

当实体和关系数据稳定后，再评估是否需要独立图数据库。

---

## 9. 借鉴开源项目的落点

### 9.1 RAGFlow

适合借鉴：

1. 深度文档理解；
2. 多格式解析；
3. 模板化 chunk；
4. chunk 可视化和人工干预；
5. grounded citation；
6. 多路召回 + rerank。

不建议直接替换当前底座。RAGFlow 是通用平台，你的系统需要道路桥隧领域 metadata 和规范条文级约束。

### 9.2 QAnything

适合借鉴：

1. 本地知识库体验；
2. 文件上传与解析进度展示；
3. 两阶段检索；
4. chunk 预览和编辑；
5. metadata 同时进入检索和生成；
6. PPT/PDF/Word/图片统一接入。

不建议直接拷代码，尤其要注意 AGPL-3.0 许可证风险。

### 9.3 KG-RAG-FOR-ARCH / KG-RAG

适合借鉴：

1. 实体关系抽取；
2. 检索增强词；
3. 知识图谱辅助召回；
4. 多跳关系扩展。

建议放到阶段 E，不应早于规范和技术文档 profile。

---

## 10. 风险

| 风险 | 说明 | 对策 |
|---|---|---|
| 资料权威混乱 | PPT 或案例被误当规范依据 | 引入 `authority_level`，生成端强制分层 |
| 切片规则混乱 | 所有资料套同一 chunker | 建 DocumentProfile |
| metadata 爆炸 | 一开始抽太多字段 | 先抽专业、对象、主题、指标、病害、工序 |
| 检索噪声增加 | 非规范资料太多，淹没规范 | 查询理解 + 权威权重 + profile filter |
| 视觉资料成本高 | PPT 和图片处理慢 | 后置到阶段 D |
| 知识图谱过早复杂化 | 图谱质量不稳定 | 先用 PG 实体关系表，后评估 Neo4j |

---

## 11. 推荐结论

短期不要改变当前 M2c 主线。先把规范库做成可靠的权威底座。

长期扩展时，系统应从“规范文档 RAG”升级为“道路桥隧工程知识库”，但技术路线不是推翻重建，而是在现有架构上增加：

1. `documents` 通用文档表；
2. `document_nodes` 通用节点表；
3. `DocumentProfile` 入库策略；
4. `authority_level` 权威等级；
5. 多资料来源分组回答；
6. 道路桥隧领域实体和关系。

建议下一步实施优先级：

```text
规范 profile 完成 → 技术文档 profile → 案例 profile → PPT profile → 轻量知识图谱
```

这样既保留规范问答的严谨性，又能逐步扩展成真正的道路桥隧工程知识库。
