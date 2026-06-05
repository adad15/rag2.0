# PP-StructureV3 精度修复一期设计

- 日期：2026-06-05
- 类型：子项目 spec（PP-StructureV3 继续使用前提下的精度增强；不引入 MinerU）
- 来源：用户确认“MinerU 目前先不考虑”；[M2b 解析质量 spec](2026-06-03-m2b-ocr-quality-design.md)、[M2b 进度与交接](../2026-06-04-m2b-progress-and-handoff.md) §4/§5.2、[M2c-1 结构化建树 spec](2026-06-05-m2c1-structural-tree-design.md)
- 状态：已与项目负责人确认前置到 M2c-1 之前，待写实现计划
- 背景：M2b 已把 PP-StructureV3 输出清洗成 `schema_version=2` 的富 IR，解决了条款号抽取、图表题泄漏、页眉页脚、水印等问题。但真实扫描规范仍暴露三类 PP-StructureV3 引擎级问题：块边界混乱、跨页接缝丢内容、局部乱码；且 `parsing_res_list` 不提供块级置信度，导致问题难以自动定位。本 spec 不换 OCR 引擎，而是在现有 PP-StructureV3 后面加一层“证据透传 + 保守边界检测 + 质量门禁”。

---

## 0. 在当前路线中的位置

```
M2a    OCR 管道：可插拔后端 + PP-Structure 服务 + 逐页缓存          已完成
M2b    OCR 输出清洗：raw_label / clause_no / region / suspect       已完成
M2b-2  PP-Structure 精度修复一期：证据透传 + 保守边界检测 + 质量门禁  本 spec
M2c-1  结构化建树：elements → clause tree                          已有 spec
M2c-2+ 三文本 / 落库 / 检索接线                                    后续
```

M2b-2 放在 M2c-1 之前：建树器应消费已经尽量可靠的元素流，而不是在树构建时同时处理 OCR 揉块、低置信、跨页缺列项等问题。接缝仍是 `data/parse_cache/<id>.json`，本轮把缓存 schema 加性升级到 v3；M2c-1 的输入契约同步改为 v3。

---

## 1. 一句话目标

在不更换 PP-StructureV3 的前提下，让 `parse_cache` 的 `elements` 从“干净但可能缺/乱/糊”升级为“带坐标证据、只在高确定性边界处拆分、可归属续文、能自动标红风险”的 IR，为 M2c 建树与后续检索提供更可靠输入。

验收时不追求 OCR 完美；目标是把已知三类失败从“靠人翻缓存才知道”变成“高确定性边界可拆、低确定性情况明确标红并体检量化”。

---

## 2. 设计原则

1. **不换引擎，不重写 OCR**：继续用 `services/ppstructure/app.py` 的 PPStructureV3 服务。
2. **Python 只透传证据，C++ 做确定性判断**：模型输出、坐标、顺序、行级分数留在 Python；条款拆分、续文归属、列项检查放 C++，方便在缓存上秒级回归。
3. **保守边界检测优先**：只自动做高置信结构性拆分，如块中部出现显式新条款号；不猜测尾巴归属，不自动重排阅读顺序，不自动改写乱码。
4. **标红比误修更重要**：跨页缺列项、疑似乱码、低置信块应进入 `quality_flags` 和 `ocrcheck`，供人工复核或后续二次 OCR。
5. **加性 schema 升级**：新增字段不破坏旧缓存读取；M1/M2b 既有逻辑继续可运行。

---

## 3. 本轮范围

**做（In scope）**

A. **PP-Structure 证据透传**
- 从 PP-StructureV3 结果中透传块坐标 `bbox`、阅读顺序 `reading_order`。
- 透传页面尺寸 `page_width`、`page_height`，支持页底/跨页接缝判断和未来局部重 OCR。
- 从 `overall_ocr_res` 的 OCR 行级结果聚合出近似块级置信度：`confidence_min`、`confidence_mean`，并继续填 `ocr_confidence` 为块内行均值。
- 失败时保留 `ocr_confidence=0.0`，并打 `quality_flags=["no_confidence"]`。
- 生成本次 OCR 配置指纹 `ocr_config_hash`（DPI、去水印阈值、PP-Structure 子模块开关、paddleocr 版本等），写入缓存，避免参数变化后误用旧缓存。

B. **IR schema v3**
- `ParseElement` 加 `element_id`、`bbox`、`reading_order`、`page_width`、`page_height`、`quality_flags`、`attached_to_element_id`、`attached_to_clause_no`。
- `parse_cache` 读写新字段；旧缓存缺字段时默认空值。
- `schema_version` 升到 3。

C. **新增 `ocr_repair` 修复层**
- 块内显式条款边界检测：仅当块中部出现高置信新条款号时拆分；不判断前半段究竟属于上一条还是本条。
- 续文归属：无号正文块、列项块、短续文块绑定到最近的正文条款元素，写 `attached_to_element_id`；`attached_to_clause_no` 只作可读辅助，不作为唯一键。
- 质量标记：不确定块混排、条款跳号、列项缺失、疑似乱码、低置信、跨页接缝风险进入 `quality_flags`。

D. **增强 `ocrcheck`**
- 输出 v3 新指标：拆分次数、续文归属次数、低置信块数、乱码风险数、列项缺失风险数、跨页接缝风险数。
- 支持列出前 N 条风险元素，便于人工复核。

**不做（Out of scope）**
- 不引入 MinerU/VL API/Tesseract。
- 不自动补回缺失文字；跨页漏掉的内容只标红。
- 不做二次局部重 OCR；可在后续版本用 `bbox` 支撑。
- 不做表格 cell 结构化；表格 HTML 仍留给 M5。
- 不改 PG/Milvus/检索接线；M2c 后续消费 v3 缓存。

---

## 4. 数据流

```
PDF 页面
  ↓ render_page(dpi=250, 去水印, page_width/page_height)
PPStructureV3.predict
  ↓ app.py 透传 parsing_res_list + overall_ocr_res + OCR 配置指纹
HTTP elements(JSON)
  ↓ ppstructure_backend 反序列化
ParsedDoc elements(schema v3 原始证据)
  ↓ normalize_parsed_doc(M2b 既有清洗)
caption / clause_no / region / suspect
  ↓ repair_ocr_elements(新增)
显式边界拆分 / 续文归属 / 质量 flags
  ↓ write_parse_cache
data/parse_cache/<id>.json
  ↓ ocrcheck
体检表 + 风险清单
```

调用顺序建议：

```cpp
ParsedDoc doc = merge_doc(base, ocr_pages, ocr_els);
normalize_parsed_doc(doc);
repair_ocr_elements(doc);
evaluate_ocr_quality(doc);
doc.schema_version = 3;
return doc;
```

`evaluate_ocr_quality` 可以先并入 `repair_ocr_elements`，但接口上建议拆开：repair 负责改元素流，quality 负责只读标记和指标。

---

## 5. Python 服务证据透传

当前 `services/ppstructure/app.py::_to_elements` 只消费 `parsing_res_list` 的 `block_label` 和 `block_content`。本轮改为同时读取：

- `block_bbox` / `bbox`：块坐标，统一成 `[x0, y0, x1, y1]`。
- `block_order` / `order`：阅读顺序；缺失时用数组下标。
- `page_width`、`page_height`：渲染后图像尺寸。
- `overall_ocr_res.rec_texts`、`rec_scores`、`rec_boxes`：行级 OCR 文本、分数、坐标。

行级分数聚合策略：

1. 对每个 OCR 行框，计算其中心点。
2. 若中心点落在 block bbox 内，则归属到该 block。
3. block 的 `confidence_mean` = 归属行 `rec_scores` 均值。
4. block 的 `confidence_min` = 归属行 `rec_scores` 最小值。
5. 无归属行但有 `block_score` 时使用 `block_score`；两者都没有时为 0.0。

输出元素新增字段：

```json
{
  "type": "Text",
  "page_no": 15,
  "raw_label": "text",
  "reading_order": 12,
  "bbox": [126.0, 522.5, 1480.0, 611.0],
  "page_width": 1654,
  "page_height": 2339,
  "text": "5.3.4错台应为接缝两边出现的高差...",
  "ocr_confidence": 0.91,
  "confidence_min": 0.84,
  "confidence_mean": 0.91
}
```

如果 PP-StructureV3 当前版本字段名与上述不同，`app.py` 采用“多键候选 + 缺失容忍”的读取方式，并在服务启动或首个请求打印真实顶层 key 与 block key 集合一次，方便定位版本差异。

`ocr_config_hash` 不需要每个元素重复计算；服务响应顶层携带一次，C++ 写入 parse cache 顶层 manifest。第一期可同时把 hash 冗余到每个元素，便于旧读取路径调试，但权威位置是缓存顶层。

---

## 6. C++ IR schema v3

在 `src/parse/parser.h` 中加性扩展：

```cpp
struct BBox {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

struct ParseElement {
    // 保留 M2b 已有字段：type/page_no/level/clause_no/title/text/table_html/
    // caption/source/ocr_confidence/raw_label/region/is_caption/suspect。
    std::string element_id;
    BBox bbox;
    int reading_order = 0;
    int page_width = 0;
    int page_height = 0;
    float confidence_min = 0.0f;
    float confidence_mean = 0.0f;
    std::vector<std::string> quality_flags;
    std::string attached_to_element_id;
    std::string attached_to_clause_no;
};
```

兼容策略：

- `element_id` 在 C++ 反序列化后生成，格式为 `p<page_no>:o<reading_order>:<short_hash>`；拆分产生的新元素追加 `:s<index>`，保证一次 ingest 内稳定且不依赖条款号。
- `parse_ppstructure_json` 缺 `bbox` 时保留 0 坐标。
- `parse_cache.cpp` 缺 `quality_flags` 时读成空数组。
- `suspect` 字段保留；新增规则同时写入 `quality_flags`，旧 UI/测试仍可看 `suspect`。
- `ParsedDoc.schema_version = 3` 只在执行 repair 后设置；读旧 v2 缓存不自动伪装成 v3。
- 缓存顶层新增 `ocr_config_hash`；哈希不匹配时后续 `ocrcheck` 应提示“缓存来自不同 OCR 配置”，但不阻止读取。

---

## 7. `ocr_repair` 组件

新增文件：

| 文件 | 职责 |
|---|---|
| `src/parse/ocr_repair.h` | 暴露 `repair_ocr_elements(ParsedDoc&)`、辅助类型 |
| `src/parse/ocr_repair.cpp` | 块拆分、续文归属、质量标记 |
| `tests/test_ocr_repair.cpp` | 纯 C++ 单测 |

### 7.1 块内显式条款边界检测

目标不是猜测“揉块”里每段文字的真实归属，而是只识别一种高确定性情况：同一个块中部出现新的显式条款号，且该条款号可以独立通过 `parse_clause_no`。例如：

```text
度（1.0m）换算成损坏面积。损坏程度应按下列标准判断。5.3.4错台应为接缝两边出现的高差...
```

拆为：

```text
[Text] 度（1.0m）换算成损坏面积。损坏程度应按下列标准判断。
[Text/Heading] 5.3.4 错台应为接缝两边出现的高差...
```

识别规则：

- 只处理 `source=="ppstructure"`、非 `Table/Formula/Figure`、非 caption、非明确页眉页脚页码的元素。
- 不强依赖 `region==Body`。M2c-1 文档已判断 M2b 的 region 对汇编型文档只是弱提示；因此只要元素本身含可信条款候选、T 方法号候选或列项候选，就允许参与修复。
- 在块首之外扫描可信条款号：
  - 十进制条款号：`N.M`、`N.M.K`、`N.M.K-S`。
  - T 方法号：`T 0301—2024` / `T 0301-2024`。
- 命中点前必须是句号、分号、冒号、换行、中文右括号、空白之一。
- 命中点后必须跟中文、字母标题或正文；如果后面是 `%`、`mm`、`kN`、数字单位，则拒绝。
- 拆分后的后半段必须能独立通过 `parse_clause_no`；否则不拆。
- 若同一块中命中多个可信条款号，按顺序拆成多段。

不做的事：

- 不判断前半段究竟属于上一条、本条，还是当前块自己的尾巴。
- 不自动重排块顺序。
- 不自动把前半段接回上一条。
- 不自动补缺失文字。
- 如果块内疑似混有多段内容但没有高置信新条款边界，只添加 `quality_flags += "ambiguous_block_mix"`，保留原文。

拆分后的字段继承：

- `page_no/source/raw_label/region/bbox/reading_order/confidence_*` 继承原块。
- 第一段保留原 `clause_no` 或空。
- 后续段重新调用 `parse_clause_no` 填 `clause_no`。
- 原块及新块都加 `quality_flags += "explicit_clause_boundary_split"`。

### 7.2 续文归属

目标不是合并文本，而是为建树提供归属提示。

规则：

- 顺序扫描正文候选元素。
- 遇到带 `clause_no` 的正文元素，更新 `current_element_id` 与 `current_clause_no`。
- 遇到无 `clause_no`、非 caption、非表格、非前置区域的文本元素：
  - 若文本像列项（`1 `、`1）`、`（1）`、`1 重度...`）或普通续文，则 `attached_to_element_id=current_element_id`，同时填 `attached_to_clause_no=current_clause_no` 方便人读。
  - 添加 `quality_flags += "attached_continuation"`。
- 如果无当前条款可归属，添加 `quality_flags += "orphan_text"`。

第一期不把文本硬拼进上一条，避免改变原始证据；M2c 建树时可按 `attached_to_clause_no` 合并到叶子节点正文。

### 7.3 列项缺失与跨页接缝风险

工程规范常见结构：

```text
损坏程度应按下列标准判断：
1 轻度...
2 中度...
3 重度...
```

风险检测：

- 如果某条款或其续文中出现“按下列标准判断”“分为下列”“应符合下列规定”等引导语，期望后续列项从 1 开始。
- 在同一条款归属范围内收集列项编号。
- 若首个列项不是 1，标 `missing_item_start`。
- 若列项序列跳号，如 `1, 3`，标 `missing_item_gap`。
- 若引导语位于页底附近（bbox y1 接近页面底部）且下一页首个归属列项不是 1，额外标 `cross_page_gap`。

页面底部判断采用相对坐标：如果 bbox 有效且 `y1 / page_height >= 0.85`，视作页底风险。`page_height` 由 Python 渲染层透传；缺失时只做列项序列检查，并给该元素加 `quality_flags += "missing_page_size"`。

### 7.4 疑似乱码

现有 `is_english_garble` 只丢独立英文糊块。新增中文 OCR 乱码风险：

- 文本包含高比例 ASCII 或孤立字母夹杂中文，如 `2EM 添司`。
- 文本长度较短且包含明显异常 token：`EM`、`司ab`、连续不可解释拉丁片段。
- 行级 `confidence_min < 0.60` 或 `confidence_mean < 0.75`。
- 以上任一命中时添加 `quality_flags += "garble_risk"` 或 `low_confidence`。

乱码只标记，不删除、不改写。

---

## 8. `ocrcheck` v3 指标

增强 `src/parse/ocr_metrics.{h,cpp}`：

```text
==== OCR 体检表 ====
schema_version          : 3
正文条款候选            : 45
  其中抠到号            : 45  (填充率 100.0%)
显式边界拆分            : 8
不确定块混排            : 6
续文归属                : 64
孤立正文                : 3
图表题泄漏              : 0
低置信块                : 12
疑似乱码                : 5
缺列项风险              : 2
跨页接缝风险            : 1
可疑条款(seq/short)     : 4
置信度 min/mean/max     : 0.512 / 0.903 / 0.992
```

风险清单输出前 N 条，默认 20：

```text
[p13 order=1 flags=cross_page_gap,missing_item_start] 判断：3重度应为...
[p15 order=7 flags=garble_risk,low_confidence] 2EM 添司
```

如果置信度仍不可用，保持当前“不可用”展示，并把 `no_confidence` 计入质量 flags，避免误以为精度已可量化。

---

## 9. 测试策略

**单测优先，不依赖 OCR 服务**

- `test_ppstructure_normalize.cpp`：补 bbox/order/page_size/confidence/quality_flags 反序列化。
- `test_parse_cache.cpp`：schema v3 字段、`element_id`、`attached_to_element_id`、`ocr_config_hash` 往返。
- `test_ocr_repair.cpp`：
  - 块中部 `5.3.4` 在高置信边界条件下被拆出新元素，并打 `explicit_clause_boundary_split`。
  - 块内疑似混排但没有高置信新条款边界时不拆，只标 `ambiguous_block_mix`。
  - `3.5mm`、`0.5%`、`200kN` 不被误拆。
  - 无号续文绑定最近 `element_id`，并保留 `attached_to_clause_no` 作为可读辅助。
  - 无当前条款时标 `orphan_text`。
  - 列项 `1,3` 标 `missing_item_gap`。
  - `判断：` 后首项为 `3` 标 `missing_item_start`。
  - `2EM 添司` 标 `garble_risk`。
- `test_ocr_metrics.cpp`：v3 指标计数与格式化输出。

**端到端手动验证**

1. 重启 PP-Structure 服务。
2. 清目标文档 `data/ocr_cache/<id>` 与 `data/parse_cache/<id>.json`。
3. 对 JTC 5210-2018 扫描件重跑 `ingest`。
4. 运行 `ocrcheck`，确认 v3 指标出现。
5. 抽查 p7、p12-13、p15：
   - p7/p15 若存在显式新条款边界，应出现 `explicit_clause_boundary_split`；若无法确定边界，应只出现 `ambiguous_block_mix`。
   - p12-13 缺列项应出现 `missing_item_start` 或 `cross_page_gap`。
   - p15 乱码应出现 `garble_risk` 或 `low_confidence`。

---

## 10. 验收标准

1. `app.py` 返回元素包含 bbox/order/page_size/confidence 字段；字段缺失时服务不崩。
2. `parse_cache` schema v3 能往返新字段、稳定 `element_id`、顶层 `ocr_config_hash`，旧 v2 缓存仍可读取。
3. 块内显式条款边界检测对真实样本有效：高置信新条款号可拆，低确定性块只标 `ambiguous_block_mix`，数值单位负例不误拆。
4. 无号续文能绑定到最近条款元素，但原始文本不被硬改写。
5. `ocrcheck` 能量化低置信、乱码、缺列项、跨页接缝风险。
6. 现有 M2b 指标不回退：图表题泄漏仍为 0，正文条款填充率不下降。

---

## 11. 风险与应对

| 风险 | 应对 |
|---|---|
| PP-StructureV3 字段名随版本变化 | Python 多键候选读取；首批日志打印真实 key 集合 |
| bbox 与 OCR 行框坐标系不一致 | 若聚合结果异常，全量标 `no_confidence` 并保留旧逻辑，不阻塞主流程 |
| 显式边界拆分误拆数值 | 复用 `parse_clause_no` 守卫；新增单位负例测试 |
| 不确定揉块被过度修复 | 默认不猜尾巴归属；不满足高置信边界条件只标 `ambiguous_block_mix` |
| 自动续文归属误绑 | 只写 `attached_to_element_id` / `attached_to_clause_no`，不直接合并文本；M2c 可选择使用或忽略 |
| 质量 flags 过多影响判断 | `ocrcheck` 分类型统计并输出样例，先让数据说话再调阈值 |
| OCR 参数变化复用旧缓存 | 顶层 `ocr_config_hash` 和 `ocrcheck` mismatch 提示；后续可把 hash 纳入 `data/ocr_cache` 目录 key |

---

## 12. 影响文件

**Python**
- 修改：`services/ppstructure/app.py`

**C++**
- 修改：`src/parse/parser.h`
- 修改：`src/parse/ppstructure_backend.cpp`
- 修改：`src/parse/parse_cache.cpp`
- 修改：`src/parse/hybrid_parser.cpp`
- 修改：`src/parse/ocr_metrics.{h,cpp}`
- 新增：`src/parse/ocr_repair.{h,cpp}`
- 修改：`rag2.0/*.vcxproj*`
- 修改：`rag2.0.tests/*.vcxproj*`

**Tests**
- 新增：`tests/test_ocr_repair.cpp`
- 修改：`tests/test_ppstructure_normalize.cpp`
- 修改：`tests/test_parse_cache.cpp`
- 修改：`tests/test_ocr_metrics.cpp`

---

## 13. 后续可选项

本期完成后，bbox 与 quality flags 可支撑两条后续路线：

1. **局部二次 OCR**：只对 `low_confidence/garble_risk/cross_page_gap` 附近区域提高 DPI 或裁剪重识别。
2. **人工复核界面**：按风险清单导出页面、bbox 和文本，快速定位问题页。

这两项不进入本期实现，避免把“精度修复一期”拖成完整质检平台。
