# M2a 扫描件 OCR 解析（可插拔引擎 + 逐页路由）设计

- 日期：2026-06-02
- 类型：子项目 spec（M2 的第一刀：解析/OCR；结构化层为 M2 第二刀，另立 spec）
- 来源：[总览路线图](2026-05-31-rag-overview-roadmap-design.md) M2、旧 [M2 计划](../plans/2026-05-31-m2-ingest-normalization.md)（仅作素材参考，本 spec 重新划界）、主设计文档 §6
- 状态：已与项目负责人确认设计，待写实现计划
- 背景：M1 实测发现用户语料**多为扫描或图文混合 PDF**，poppler 抽不到文本层（扫描页每页约 6 字，文字页约 2000 字）。M1 的 poppler 文本路覆盖不了真实语料。本子项目为"扫描/混合 PDF → 文字 + 条款进现有入库管道"，并把解析层做成**可插拔多引擎**，默认 PP-Structure。

---

## 0. 为什么把 M2 拆开、先做这一刀

旧 M2 计划把"OCR 接入"和"结构化加厚"（条款树/三文本/表格 cell/质检门禁/schema）捆在一个 14 任务大计划里，且写于扫描语料发现之前。两者是可分离的子系统：
- **解析/OCR（本 spec）**：把扫描/混合 PDF 变成文字与富 IR——用户真正的卡点、风险最高。
- **结构化层（下一 spec）**：纯 C++ 逻辑，消费解析产物，低风险。

故先做解析/OCR，拿到"扫描件能进系统、能被问到"的端到端结果；结构化层下一轮在已缓存的富 IR 上做，不重复 OCR。

---

## 1. 本轮范围

**做（In scope）**
1. 解析层改造为**可插拔多引擎** + **逐页路由**（混合 PDF 每页对症下药）。
2. 实现 **PP-Structure** 引擎（PaddleOCR / PP-StructureV3）为默认 OCR 后端，走独立 Python HTTP 服务。
3. 契约① IR **加性扩展**：`ParsedDoc` 增 `elements`（标题/正文/表格 HTML/页码）。
4. 富 IR 持久化为**磁盘 JSON 缓存**（供下一轮结构化层消费，避免重复 OCR）。
5. 入库**沿用 M1 的 `clause_splitter` + ingest + PG + Milvus**——扫描件现在也能出条款、被 query 命中。
6. config 新增引擎选择/路由模式/服务地址/阈值。
7. 为 **MinerU / VL-API / Tesseract** 预留 OCR 后端接口（本轮不实现）。

**不做（Out of scope，留给"结构化层"那一轮）**
- 条款层级树（章/节/条）、atomic/retrieval/context 三文本分离、表格 cell 级结构化、page_clause_map、质检门禁、PG schema 扩展。
- MinerU / VL-API / Tesseract 的**具体实现**（仅预留接口）。
- 逐页路由中"同一引擎内的批量优化"等性能调优。

**契约稳定性**：契约 ①（加性扩展）②③④⑤ 均不破坏；M1 的 `query`/检索/生成路径不动，回归通过。

---

## 2. 架构：契约① 多后端 + 逐页路由

```
                 ┌──────────────────────────────────────────┐
   file_path ──> │ HybridParser（"auto" 引擎，逐页路由）        │ ──> ParsedDoc
                 │  1. poppler 抽全篇，得每页文本               │   (pages文本 + elements富IR)
                 │  2. 每页判：文字够→用poppler；稀疏→标记OCR    │
                 │  3. 稀疏页批量送 OCR 后端，得 elements        │
                 │  4. 合并为统一 ParsedDoc                     │
                 └──────────────┬───────────────────────────┘
                                │ OCR 后端（可插拔，本轮只实现 PP-Structure）
                    ┌───────────▼─────────────┐
                    │ OcrBackend 接口           │
                    │  - PpStructureBackend(本轮)│──HTTP──> PP-Structure FastAPI 服务(Python)
                    │  - [预留] MineruBackend    │
                    │  - [预留] VlApiBackend     │
                    │  - [预留] TesseractBackend │
                    └───────────────────────────┘
```

- **契约① `Parser`（文件→`ParsedDoc`）不变**，仍是顶层接口。实现只两个：
  - `PopplerParser`（已有，纯原生文字版直接用）。
  - `HybridParser`（**本轮新增**）：持有一个 `OcrBackend` + 一个路由模式，按模式决定每页用 poppler 还是 OCR。
- **`OcrBackend` 接口**：输入"文件 + 页号列表"，输出这些页的 `ParseElement`。本轮实现 `PpStructureBackend`；`Mineru/VlApi/Tesseract` 预留（声明接口、不实现）。
- **路由模式**（`RAG_PARSE_MODE`，决定 `HybridParser` 行为）：
  - `auto`（默认）：逐页判定，文字页用 poppler、稀疏页用 OCR 后端。
  - `poppler`：所有页用 poppler（等价于直接用 `PopplerParser`）。
  - `ocr`：所有页强制走 OCR 后端（阈值视为无穷大）。
  - 即三种模式都是 `HybridParser` 的特例，无需额外解析器类。

---

## 3. 逐页路由机制（解决混合 PDF）

`HybridParser::parse(file_path)`：
1. `PopplerParser` 抽全篇 → 每页 `text`。
2. 对每页算字符数；`< 阈值`（默认 100 字/页，config 可调）判为**稀疏页（疑似扫描）**，收集其页号。
3. 把稀疏页号列表交给 `OcrBackend`（默认 PP-Structure）批量 OCR，得到这些页的 `elements`（标题/正文/表格 HTML）。
4. **合并**：
   - 文字页：用 poppler 的 `text`（并生成 Text 元素入 `elements`）。
   - 稀疏页：用 OCR 的 `elements`，并把其文本拼接回该页 `text`。
   - 按页号顺序产出统一 `ParsedDoc`。

**效果**：纯文字 / 纯扫描 / 任意混合都正确；只 OCR 真正需要的页（文字页零 OCR 开销）。

**边界**：阈值用真实文档校准（实测扫描 ~6 字/页、文字 ~2000 字/页，差距巨大，50~500 均稳）。

---

## 4. IR：契约① 加性扩展

`src/parse/parser.h` 的 `ParsedDoc` 增 `elements`（保留 `pages`）：

```cpp
enum class ElementType { Heading, Text, Table, Formula, Figure };

struct ParseElement {
    ElementType type = ElementType::Text;
    int page_no = 0;            // 从 1 开始
    int level = 0;              // Heading 层级，非标题为 0
    std::string clause_no;      // 元素自带条款号（可空）
    std::string title;          // 标题文本
    std::string text;           // 正文 / OCR 文本
    std::string table_html;     // 表格 HTML（Table 类型）
    std::string caption;        // 表/图题
    std::string source;         // "poppler" | "ppstructure" | ...（本页由谁解析）
    float ocr_confidence = 1.0f;
};
// ParsedDoc 增： std::vector<ParseElement> elements;
//               std::string standard_no;  // 可空，沿用 M1 的 extract_standard_no 回填
```

下游（本轮）只消费 `pages` 文本走切分器；`elements` 仅持久化备用。

---

## 5. PP-Structure 服务（Python / FastAPI，独立部署）

`services/ppstructure/app.py`：
- `POST /parse_pages {file_path, pages:[int]}` → `{elements:[...]}`（仅这些页）。
- `POST /parse {file_path}` → 全篇 elements（`ocr` 强制模式用）。
- `GET /health` → `{"status":"ok"}`。
- 内部：PP-StructureV3 对指定页做**版面分析 + 中文 OCR + 表格识别(HTML)**，按阅读顺序输出 elements（含 page_no、表格 HTML、置信度）。
- 运行：GTX 1660 Ti(6GB) 或 CPU；**逐页处理防 OOM**，单页失败跳过并在响应里标注。
- 文件路径由 C++ 传入，服务与主程序同机、可读同一路径（沿用 M1/旧计划约定）。

`services/ppstructure/requirements.txt`：`fastapi`、`uvicorn[standard]`、`paddlepaddle-gpu`（或 CPU 版 `paddlepaddle`）、`paddleocr`。

---

## 6. 富 IR 持久化（磁盘 JSON 缓存）

- 解析完成后，把整个 `ParsedDoc`（含 `elements`）序列化为 JSON，写 `data/parse_cache/<standard_id>.json`（`data/` 已 gitignore）。
- 用途：下一轮"结构化层"读此缓存建层级树/表格，**不重复 OCR**。
- 幂等：同 `standard_id` 覆盖；存在缓存且 PDF 未变可跳过 OCR（本轮可选优化，先简单覆盖）。
- **不进 Milvus**（仅向量）、**不进 PG**（schema 改动属下一轮）。

---

## 7. 入库（本轮保持最小，复用 M1）

`ingest`：
1. 按 config 选解析器（`auto`→HybridParser）→ `ParsedDoc`。
2. 写富 IR 缓存（§6）。
3. **沿用 M1**：`split_clauses(每页 text)` → 条款 → 写 PG `clause_nodes`；`embed(条款正文)` → 写 Milvus（同 M1 修订①，只嵌正文）。
4. 标准号沿用 M1 `extract_standard_no`（首页文本/缓存回填）。
- 表格：本轮其文本已随页文本进检索（可被搜到）；**cell 级结构化留下一轮**消费缓存里的 `table_html`。

---

## 8. 配置（config.json 新增）

| 键 | 默认 | 说明 |
|----|------|------|
| `RAG_PARSE_MODE` | `auto` | `auto`(逐页混合) / `poppler` / `ocr` |
| `RAG_OCR_ENGINE` | `ppstructure` | OCR 后端：`ppstructure`；预留 `mineru`/`vlapi`/`tesseract` |
| `RAG_PPSTRUCT_BASE_URL` | `http://localhost:8001` | PP-Structure 服务地址 |
| `RAG_SCAN_CHARS_THRESHOLD` | `100` | 每页字符数低于此判为稀疏页（送 OCR） |

`config.example.json` 同步加这些键（值为默认，不含密钥）。

---

## 9. 错误处理

- 服务不可达 / 解析失败 → C++ 抛**可读异常**（复用现有 ingest try/catch），不崩、不 abort。
- 单页 OCR 失败 / OOM → Python 端跳过该页 + 标注，C++ 合并时该页文本为空，不中断全篇。
- 选了未实现的引擎（如 `mineru`）→ 启动即报"该引擎尚未实现，请用 ppstructure"。

---

## 10. 测试策略

**纯逻辑 TDD（不触网/不依赖服务）**：
- IR 归一化：PP-Structure 服务 JSON → `ParsedDoc.elements`（含 Table/Heading/页号）。
- 逐页路由判定：给定每页字符数数组 + 阈值 → 正确算出"哪些页该 OCR"。
- 合并逻辑：poppler 页 + OCR 页 → 正确按页号合并、文本回填。
- 缓存读写：`ParsedDoc` ↔ JSON 往返一致。
- config 解析：新增 4 键的默认与覆盖。

**端到端（手动，需服务 + 你的真实 PDF）**：
- 扫描 PDF：`dump`/`ingest` 抽取字节从 ~2KB 跃升、`clauses>0`、`query` 命中并引用真实条款号。
- 混合 PDF：日志显示"第 X 页走 poppler、第 Y 页走 OCR"，结果完整。
- `data/parse_cache/<id>.json` 生成且含 elements。

---

## 11. 验收判据

1. 一份**扫描或混合**标准 PDF 经 `ingest` 写入 `clauses>0`（M1 上为 0 或仅数条）。
2. `query "<该文档某条款相关问题>"` 返回回答并**引用文档中真实存在的条款号**。
3. 逐页路由可观察：混合 PDF 中文字页用 poppler、扫描页用 PP-Structure。
4. 富 IR 缓存落盘 `data/parse_cache/<id>.json`（含 `elements`）。
5. `RAG_PARSE_MODE` / `RAG_OCR_ENGINE` 可在 config 切换并生效；选未实现引擎给出清晰报错。
6. 全部单测 PASS；M1 `query` 回归不劣化。

---

## 12. 为后续预留的接口（不实现，仅占位）

- `OcrBackend` 抽象：`MineruBackend` / `VlApiBackend` / `TesseractBackend` 实现它即可接入，HybridParser 与 router 不改。
- `ParseElement` 已含 `table_html`/`bbox(可后加)`/`ocr_confidence`，够下一轮结构化层与质检消费。
- 磁盘缓存格式即"结构化层"的输入契约。
