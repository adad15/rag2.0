# M2b 解析质量（OCR 输出清洗与可信化）设计

- 日期：2026-06-03
- 类型：子项目 spec（M2 的第二刀：解析质量；结构化层为 M2c，另立 spec）
- 来源：[总览路线图](2026-05-31-rag-overview-roadmap-design.md) M2、[M2a OCR 解析 spec](2026-06-02-m2a-ocr-parsing-design.md)、主设计文档 §6 / §14
- 状态：已与项目负责人确认设计，待写实现计划
- 背景：M2a 跑通了"可插拔 OCR 引擎 + 逐页路由 + PP-Structure 服务"。实测真实扫描件（JTC 5210-2018）发现：OCR **把中文条款和表格基本读对了**（Heading 112 / Table 26 / Formula 38），但下游"效果差"——根因不在 OCR 识别，而在**后处理没把读对的东西整理干净**：条款号没抠出来、图表题混进标题、置信度写死、英文糊未隔离。本子项目专治"让解析产物（parse_cache 的 elements）干净、可信"，为 M2c 结构化层提供干净的输入契约。

---

## 0. 在 M2 三刀里的位置

```
M2a  OCR 管道（可插拔引擎 / 逐页路由 / 服务跑通）        ✅ 已完成
M2b  解析质量（让 IR 干净可信）—— 本 spec                producer 侧
M2c  结构化层（条款树 / 三文本 / schema / Milvus）        consumer 侧，另立 spec
```

拆分理由：M2b 是**模型/OCR/Python 为主的"数据清洗"**，M2c 是**纯 C++ 的"数据建模"**——两类性质、风险、技能都不同的活，分开后各自可独立验收、风险隔离。接缝是 `data/parse_cache/<id>.json`：M2b 负责把缓存里的 `elements` 喂饱喂干净，M2c 负责把它吃成树。

---

## 1. 一句话现状与目标

- **现状**：OCR 服务（`services/ppstructure/app.py` 的 `_to_elements`）把 PP-Structure 的富标签**拍平丢失**：`doc_title/paragraph_title/table_title` 全压成 `Heading`；`clause_no` 从不填；`ocr_confidence` 写死 1.0。结果条款号躲在 `title` 里且与正文粘连无空格（`5.2.1龟裂...`），图表题（`表4.0.1...`）混进标题，下游 M1 式"空格正则"切分几乎切不出条款（56 页仅 2 页有行首条款号）。
- **目标**：经 M2b 后，`parse_cache/<id>.json` 的每个 `elements[i]` 满足：①条款号被抠进 `clause_no`；②图/表题被正确标为 caption、不冒充条款；③携带**真实** OCR 置信度；④带 `region` 标签（front_matter/toc/body/appendix/explanation）；⑤可疑项被标记。**M2b 不建树、不碰 PG/Milvus、不改检索。**

---

## 2. 设计原则：app.py 如实上报，C++ 做判断

> **app.py（Python）只负责"如实上报它看到了什么"，所有语义判断放 C++。**

理由：
1. **迭代成本**——clause_no 正则、图表题规则会拿真实文档反复调。放 C++ 可在**缓存 JSON 上重跑归一化**（秒级、不碰 GPU）；放 Python 改一次就得重跑 OCR（GPU、分钟级）。
2. **共享号码文法**——poppler 前端（行首空格分隔）与 OCR 前端（title 抠号）共用同一套条款号语法，放 C++ 一份实现两处复用。
3. **可测 + 合架构**——纯字符串逻辑用既有 doctest 覆盖；契合主设计 §2.1"确定性逻辑在 C++"。

唯一必须留在 Python 的：**真实 `ocr_confidence`**（C++ 事后无法恢复）、**渲染 DPI**、以及**停止丢弃** PP-Structure 的原始块标签。

---

## 3. 本轮范围

**做（In scope）**

A. **app.py 保真补丁**（唯一的 Python 改动）：
   - 透传 PP-Structure 每块的原始标签到新字段 `raw_label`（如 `table_title`/`paragraph_title`/`figure_title`/`doc_title`/`text`/`formula`...），不再拍平。
   - 透传**真实** `ocr_confidence`（按块），不再写死 1.0。
   - 渲染 DPI 200 → 300。

B. **C++ 归一化层**（M2b 主战场，落 `src/parse/ocr_normalize.{h,cpp}` 或并入 `ppstructure_backend`）：
   - **caption 过滤**：`raw_label ∈ {table_title, figure_title, chart_title}` 或标题以 `图/表/续表/附图/附表` 开头 → 标为 caption，不抠号。
   - **`parse_clause_no` 共享纯函数**：两档判定抠条款号（见 §5）。
   - **区域打标签**：顺序扫元素流，给每个元素打 `region`（见 §6）。
   - **英文糊判定**：独立糊块丢弃，附在条款尾的英文保留（见 §7）。
   - **异常标记**：连续性/正文过短 → `suspect_seq` / `suspect_short`（见 §8）。

C. **IR 加性扩展**：`ParseElement` 增 `raw_label`、`region`、`suspect`（标志位/原因）；`ParsedDoc` 增 `schema_version`。缓存 JSON 容忍式读取（旧缓存缺字段不报错）。

D. **指标脚本/命令**：对一份缓存报"OCR 干不干净"的体检表（见 §9），作为 M2b 验收依据。

**不做（Out of scope，留给 M2c 或其他轮）**
- 条款层级树（父子/path/node_type）、防撞号 node_id、三文本分离、PG schema 扩展、Milvus 标量字段与重嵌——**全是 M2c**。
- 表格 cell 结构化（spec_tables/spec_table_cells）——**M5**。
- "把英文 OCR 修对"、MinerU/其他引擎替换——超出"够用就停"。
- 同义词/分词词典——M3。

**契约稳定性**：缓存格式只做**加性**扩展（新增字段 + `schema_version`），M2c/未来读者容忍式读取；M1 的 `query`/检索/生成路径不动。

---

## 4. 架构与数据流

```
扫描页 ──HTTP──> app.py（PP-StructureV3）
                  │  如实上报：raw_label + 真实 confidence + DPI300
                  ▼
            ppstructure_backend（C++，HTTP 反序列化）
                  │
                  ▼
            ocr_normalize（C++，M2b 主体）
              ① caption 过滤（靠 raw_label）
              ② parse_clause_no（两档判定，与 poppler 前端共用）
              ③ region 打标签（顺序扫）
              ④ 英文糊判定
              ⑤ 异常标记
                  │
                  ▼
            HybridParser 合并 → ParsedDoc（elements 已清洗）
                  │
                  ▼
            write_parse_cache → data/parse_cache/<id>.json（干净 IR + schema_version）
                  │
                  ▼
            metrics 命令：对缓存出体检表（§9）
```

poppler（文字版）页同样调用 `parse_clause_no`（行首、要求空格、不认单级章号），但 M2b 的**主要目标是 OCR/扫描件质量**；文字版页本就基本可用，M2b 对其只做共享号码文法层面的统一。

---

## 5. clause_no 抠取：两档判定 + 共享纯函数

纯函数签名：`parse_clause_no(text, is_heading) -> {clause_no, rest}`。两个前端共用，doctest 覆盖全部真实样本。

**caption 优先**：进 `parse_clause_no` 前，先过 §3.B 的 caption 过滤；caption 不抠号（否则 `表7.4.5-1...` 会被误抠成条款 `7.4.5-1`）。

**两档判定**（关键，防 `200kN`/`0.5%` 误判）：

1. **多级号（≥2 级，宽松）**：`^\s*(\d+(?:\.\d+)+(?:-[0-9A-Za-z]+)?)(?=[^\d.]|$)(.*)$`，**外加两条防数值误判的守卫**：
   - **顶层段 ≥ 1**：章/条编号不以 0 开头 → 排除 `0.5%`（顶层段为 0）。
   - **rest 不得以单位/百分号开头**：抠号后剩余文本若紧跟 `%`、`‰`、`°`、`kN`、`mm` 等单位符号/词 → 判为数值，不抠（排除 `5.2%`、`3.5mm`）。剩余文本应以汉字/字母/空格起（标题或正文续）。
   - 必须含至少一个点 → `200kN`（无点）天然不匹配。
   - 覆盖：`5.2`、`5.2.1`、`2.0.1`（带 `.0.`）、`5.2.10`/`6.3.10`（多位）、`4.2.1-1`/`7.4.5-1`（后缀）。

2. **单级号（章，严格）**：仅当**同时**满足才认：
   - 元素是 `Heading`（非正文 Text）；且
   - 数字后紧跟**汉字**（`1总则` 的 `总`），非字母/单位（排除 `200kN`）；且
   - 数字在合理章号范围（如 1–99）。
   - 覆盖：`1总则`、`5公路损坏分类`、`7公路技术状况评定`。

**附录**：`附录A` / `表 A-1` 的字母编号在 M2b 仅做 caption/region 识别；附录条款的字母层级编号（A/A.1）规范化留 M2c。

**真实样本走查**（doctest 断言）：

| 输入 | caption? | parse 结果 |
|------|---------|-----------|
| `5.2.1龟裂应按面积计算...` | 否 | clause_no=`5.2.1`, rest=`龟裂应按面积计算...` |
| `2.0.1公路技术状况指数...` | 否 | clause_no=`2.0.1` |
| `5.2.10泛油...` | 否 | clause_no=`5.2.10` |
| `1总则` | 否 | clause_no=`1`(单级严格), rest=`总则` |
| `表4.0.1公路技术状况...` | **是** | 不抠号 |
| `图3.0.3...` | **是** | 不抠号 |
| `续表7.5.1` | **是** | 不抠号 |
| `200kN加到` | 否 | 不命中（无点）→ 普通正文 |
| `0.5%。` | 否 | 不命中（顶层段为 0 被守卫挡）|
| `5.2%。` | 否 | 不命中（rest 以 `%` 起，判数值）|
| `7.33路基技术状况...` | 否 | clause_no=`7.33` → §8 标 `suspect_seq` |
| `7.4.9算：` | 否 | clause_no=`7.4.9`, rest=`算：` → §8 标 `suspect_short` |

---

## 6. 区域打标签（per-element，顺序扫一遍）

维护"当前区域"状态机，给每个元素打 `region ∈ {front_matter, toc, body, appendix, explanation}`：

- 起始 `front_matter`；
- 遇 `目次` 标题（或行尾页码点引导）→ 进 `toc`；
- 遇**第一个合法章标题**（单级号 `1` + 后跟汉字，如 `1总则`）→ 进 `body`；
- 遇 `附录X` 标题 → 进 `appendix`；
- 遇 `条文说明` 标题 → 进 `explanation`。

用途：
- `toc`/`front_matter` 元素**不计入条款**（否则目次假条款号污染指标与下游）。
- `explanation` 标签给 M2c 防撞号用（条文说明的 `3.2` 不能覆盖正文 `3.2`）。
- `appendix` 保留（真内容）。

**接缝**：M2b 只**打标签**（局部、顺序扫）；把 body 的号**组装成父子树、生成 path、防撞号 node_id** 是 M2c。region 之所以在 M2b，是因为不打 toc/front_matter 标签就无法诚实度量 §9 的填充率。

---

## 7. 英文糊判定

```
某块文本 ASCII 占比 > 80% 且不含成段汉字：
  - 独立成块（如 'sessmentSta'）         → 丢弃
  - 紧跟某条款中文正文之后的尾巴
    （如 '...指数highwamintenanc'）        → 保留（噪声小，留在条款里）
```

不修英文（OCR 硬伤，超出"够用就停"边界）。阈值（80%）属可调，第一版量出后定。

---

## 8. 异常标记（度量 OCR 干不干净，只标不修）

抠号后做两项轻量检查，**只打标记、不改数据**：
- **连续性 `suspect_seq`**：同父级下条款号应递增连续（`7.1,7.2,7.3...`）。出现跳变（`7.2→7.33`）或缺号 → 标记。
- **正文过短 `suspect_short`**：抠号后 `rest` 字节数 < 阈值（如 6）→ 标记（如 `7.4.9算：`）。

标记写进缓存元素（`suspect` 字段），既供人眼复核，也供 M2c 正式质检门禁消费。**M2b 不试图自动修复错号**（猜错更危险）。

---

## 9. 验收指标（先量后定线）

M2b 交付一个**指标命令**（如 `ocrcheck <cache.json>` 或并入现有 CLI），对每份缓存报：

| 指标 | 含义 | 建议目标线\* |
|------|------|------------|
| 正文 clause_no 填充率 | `region=body` 中"应是条款"的元素里成功抠号占比 | ≥ 95% |
| 图表题泄漏数 | 被误当条款的 图/表 题数 | 0 |
| TOC 泄漏数 | 目次假条款号混进 body 条款数 | 0 |
| 可疑条款占比 | (suspect_seq + suspect_short) 占比 | ≤ 5% |
| 置信度真实率 | 携带真实（非写死 1.0）置信度的元素占比 | 100% |

\* **目标线先量后定**：M2b 第一步先跑指标量出基线，再据实定"够用线"；原则是**够 M2c 干净建树即停，不追 OCR 完美**。

**验收判据**：
1. app.py 透传 `raw_label` + 真实置信度，DPI=300；旧 `clause_no='' / confidence=1.0` 写死问题消除。
2. C++ 归一化对 JTC 5210-2018（扫描版）产出的缓存：clause_no 填充率达标、图表题/ TOC 泄漏为 0、置信度真实率 100%。
3. `parse_clause_no` doctest 覆盖 §5 全部真实样本（含 `200kN`/`0.5%` 负例）通过。
4. 指标命令可对任意缓存出体检表。
5. 缓存新增 `raw_label/region/suspect/schema_version` 字段；M2c/旧读者容忍式读取，M1 `query` 回归不劣化。

---

## 10. 测试策略

**纯逻辑 TDD（不触网/不依赖服务）**：
- `parse_clause_no`：§5 真实样本表全量断言（正例 + `200kN`/`0.5%`/`5.2%` 负例 + 单级严格档）。
- caption 过滤：`raw_label` 各取值 + `图/表/续表` 前缀。
- region 状态机：给定有序元素标签序列 → 正确切换 front_matter/toc/body/appendix/explanation。
- 英文糊判定：ASCII 占比阈值上下边界。
- 异常标记：跳号序列、过短正文。
- 容忍式读取：缺 `raw_label/region` 的旧缓存能正常反序列化。

**端到端（手动，需服务 + 真实 PDF）**：
- 对 JTC 5210-2018 重跑 OCR ingest → 看缓存 elements 的 clause_no/region/confidence；跑指标命令看体检表达标。
- 对 JTG 3432-2024（文字版）回归：poppler 路 `parse_clause_no` 不破坏既有切分。

---

## 11. 为 M2c 预留的契约

- 缓存元素新增 `clause_no`（已抠）、`region`、`raw_label`、`suspect`、`caption`(图表题)、真实 `ocr_confidence`，`table_html`（OCR 表格，留 M5）。
- M2c 的"双前端"中 OCR 前端简化为"读已清洗 elements"；poppler 前端复用同一 `parse_clause_no`。
- region=explanation 是 M2c 防撞号的依据；region=toc/front_matter 是"不入树"的依据。

---

## 12. 风险

| 风险 | 应对 |
|------|------|
| PP-Structure 的 `block_label` 取值集合与假设不符 | 实现首步**先打印真实 label 全集**再定 caption 映射；映射表配置化 |
| 单级章号严格档仍误判（如正文里"5 个试样"） | 限定仅 Heading 元素 + 后跟汉字 + 章号范围；误判进 §8 可疑标记兜底 |
| 目标线拍脑袋 | 先量基线后定线（§9） |
| DPI→300 增大渲染/显存开销 | 1660Ti 上单页渲染可接受；必要时 DPI 配置化 |
