# M1 之后进度与交接（截至 2026-06-02）

> 给接手的新会话：本文汇总 M1 收尾后到 M2a 的全部进展、现场调试踩的坑与修复、当前状态、如何运行、下一步。分支 **`V2.0`**（已推 origin）。

---

## 0. 一句话现状

- **M1（端到端走查骨架）**：已完成并在**文字版 PDF** 上跑通验收（ingest→query 带条款号溯源）。
- **M2a（扫描件 OCR 解析）**：**C++ 侧 6 个任务全部完成 + Python OCR 服务跑通（PP-StructureV3，GPU）**；正在做**真实扫描件的端到端 ingest 验证**——刚修完 embedding 偶发 SSL 失败的重试，待重跑确认。
- **M2 第二刀（结构化层）**：尚未开始。

相关文档：
- 总览路线图：`docs/superpowers/specs/2026-05-31-rag-overview-roadmap-design.md`
- M0+M1 计划：`docs/superpowers/plans/2026-05-31-m0-m1-walking-skeleton.md`
- M1 修订 spec/plan：`docs/superpowers/specs/2026-06-01-m1-scope-revision-design.md`、`plans/2026-06-01-m1-scope-revision.md`
- **M2a spec/plan**：`docs/superpowers/specs/2026-06-02-m2a-ocr-parsing-design.md`、`plans/2026-06-02-m2a-ocr-parsing.md`

---

## 1. 工程基线（务必先知道）

- 构建：**VS2022/MSBuild + vcpkg 清单**，**显式源文件清单**（非通配符）——新增 `.cpp/.h` 必须登记两个 `.vcxproj` + `.filters`。C++20、强制 `/utf-8`。
- 构建命令（PowerShell）：`& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`
- 测试：`rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest，当前 **40 用例 / 130 断言全绿**）。
- 应用：`rag2.0\x64\Debug\rag2.0.exe`，**从项目根目录运行**（`config.json`、`src/db/schema.sql` 为相对路径）。
- **配置走根目录 `config.json`**（已 gitignore；模板 `config.example.json`）。**不再用环境变量**。
- **Windows + UTF-8**：已加 `rag2.0/app.manifest`（进程 ACP=UTF-8），解决中文路径/中文命令行参数；另有 `src/util/path_utf8.h` 按字节切路径。控制台看中文先 `chcp 65001`。

---

## 2. M2a 做了什么

### 2.1 C++ 侧（Tasks 1–6，全部完成）
可插拔多引擎解析层 + 逐页路由：
- `src/parse/parser.h`：契约① IR 加性扩展——`ParseElement`(type/page_no/level/clause_no/title/text/table_html/caption/source/ocr_confidence) + `ParsedDoc.elements` + `standard_no` + 枚举↔字符串。
- `src/parse/parse_cache.{h,cpp}`：`ParsedDoc ↔ JSON` + `write_parse_cache` 写盘（富 IR 存 `data/parse_cache/<standard_id>.json`，供下一轮结构化层消费，不重复 OCR）。
- `src/parse/ocr_backend.h`：`OcrBackend` 抽象接口（`ocr_pages(file,pages)`）。
- `src/parse/ppstructure_backend.{h,cpp}`：`PpStructureBackend`（HTTP 调服务）+ 纯函数 `parse_ppstructure_json`。**已加分批（16页/次）+ 600s 读超时**。
- `src/parse/hybrid_parser.{h,cpp}`：`HybridParser`（契约①实现）——`pick_ocr_pages`(按每页**字节**阈值，auto/poppler/ocr) + `merge_doc`(poppler 文字页 + OCR 扫描页合并)。
- `src/parse/parser_factory.{h,cpp}`：`parse_mode_from_string` + `make_ocr_backend`（未实现引擎抛可读错误）。
- `src/ingest/ingest_pipeline.cpp`：`ingest_file` 加 `cache_dir` 参数；parse 后回填 `doc.standard_no` 并 `write_parse_cache`；其余复用 M1（split→PG→embed→Milvus）。
- `src/main.cpp`：`cmd_ingest` 与 `cmd_dump` 都经 `make_parser` 用 config 选定的 HybridParser。
- config 新增：`RAG_PARSE_MODE`(auto/poppler/ocr,默认auto)、`RAG_OCR_ENGINE`(默认ppstructure)、`RAG_PPSTRUCT_BASE_URL`(默认 http://localhost:8001)、`RAG_SCAN_CHARS_THRESHOLD`(默认100,**字节**)。
- 测试：test_parse_cache / test_ppstructure_normalize / test_hybrid_router / test_parser_factory + config 用例。

### 2.2 Python OCR 服务（`services/ppstructure/`，已跑通）
- **uv 项目**：`pyproject.toml`(含 `paddleocr>=3.0`、`paddlex[ocr]>=3.0`) + `uv.lock` + `app.py`(FastAPI) + `README.md`。
- 引擎：**PaddleOCR 3.6.0 / PP-StructureV3**，跑在 **GTX 1660 Ti(GPU)**；paddlepaddle-gpu 3.3.1（手动装，不在 pyproject）。
- 端点：`POST /parse_pages {file_path,pages[]}`、`POST /parse {file_path}`、`GET /health`→`{"status":"ok","engine":"v3"}`。
- **验证过**：第 91 页返回 21 个元素，条款号(5.3/6.1/6.2/7.1-7.8/3.2/4.4)全、公式还原 LaTeX；HTTP 字段名与 C++ `parse_ppstructure_json` 对齐。

---

## 3. 现场调试踩的坑与修复（都已提交到 V2.0）

| 坑 | 修复 |
|----|------|
| 中文路径/中文命令行被当 GBK → 崩 | UTF-8 manifest + `path_utf8.h`（M1 修订期） |
| 密钥管理 | 改 `config.json`（gitignore） |
| `uv run` 每次重新同步，把手动装的包退回 | 用 **`uv run --no-sync`** 跑服务（保住手动装的 GPU paddle） |
| paddleocr 装成 2.x（无 PPStructureV3） | pyproject 锁 `paddleocr>=3.0` + `uv pip install -U` |
| `PPStructureV3()` 构造报 DependencyError | 装 **`paddlex[ocr]`**（已写进 pyproject） |
| v3 返回结构与 app.py 假设不符 | `_to_elements` 改用真实字段 **`block_label`/`block_content`**，取 `result[0].json['parsing_res_list']` |
| 305 页一次性 OCR 超 C++ 120s 读超时 | OcrBackend **分批 16 页/次** + post_json 加超时参数(600s) |
| embedding 偶发 `SSL connection failed` 中止整批 | **embedding 重试**（连接层/5xx 退避重试 4 次，`cloud_embedding.cpp`） |

---

## 4. 怎么运行（当前状态下）

**① 起 OCR 服务**（一个窗口，保持开着）：
```powershell
cd "D:\vs2022 code\rag2.0\services\ppstructure"
uv run --no-sync uvicorn app:app --host 0.0.0.0 --port 8001
```
（务必 `--no-sync`；模型已缓存到 `C:\Users\<你>\.paddlex\`，启动十几秒。`Ctrl+C` 关闭。）

**② ingest / query**（另一窗口，项目根目录）：
```powershell
cd "D:\vs2022 code\rag2.0"
chcp 65001
.\rag2.0\x64\Debug\rag2.0.exe ingest
.\rag2.0\x64\Debug\rag2.0.exe query "<某条款问题>"
```
- `auto` 模式：原生文字页用 poppler，扫描页转 PP-Structure。
- **OCR 是"先整篇 OCR 完再入库"**——305 页扫描件要等 ~20-40 分钟（进度看**服务窗口**的 PaddleOCR 日志；C++ 窗口此间静默）。中途 Ctrl+C 则未写库。
- 建议先用小文件（`data/sample.pdf`，改 config.json 的 `RAG_DOC_PATH`）跑通整条链，再上大的。

---

## 5. 当前未决 / 下一步

1. **正在验证**：真实扫描件端到端 ingest（刚加 embedding 重试，待重跑确认 `clauses>0` + query 命中）。若 SSL 仍频繁失败（非偶发），改 embedding **复用连接**（现在每条新建 TLS）或查网络/限流。
2. **M2a 收尾后**：本轮的"富 IR"已落 `data/parse_cache/`，供下一轮消费。
3. **检索质量差是预期内的**（M2a 只解决"读进来"，没碰检索）。实测 ingest 成功（query 能返回条款上下文），但：表格类查询(如方法号 `T0327-1`)答不了——**表格未结构化**；关键词查询(如 `干筛法`)召回错——**单路 dense + 只嵌条款正文**太弱。**这正是下面两步要解决的，别期待现在检索好。**
4. **M2 第二刀「结构化层」**（独立子项目，走 brainstorm→spec→plan）：条款层级树(章/节/条) + atomic/retrieval/context 三文本分离(**富化 retrieval_text=标准号+名称+章节路径+正文+关键词+单位**) + 表格 cell 级结构化(spec_tables/spec_table_cells，让表格可查) + page_clause_map + 解析质检门禁 + 对应 PG schema 扩展。消费 parse_cache 的 `elements`(含 table_html)。
5. **M3 三路召回**：dense + BM25 + PG 精确匹配 + RRF 融合 + 查询理解（标准号/条款号归一化、同义词/中文分词词典）。检索质量主要靠这步。
4. 之后按路线图：M3 三路召回 → M4 评估闭环。

---

## 6. 关键提醒（容易忘）

- 跑服务**永远带 `--no-sync`**，否则卸掉 GPU paddle。
- 新增 `.cpp/.h` 要登记两个 `.vcxproj` + `.filters`。
- `RAG_SCAN_CHARS_THRESHOLD` 单位是**字节**（中文≈3字节/字），默认 100，区分扫描(每页~几字节)与文字(每页~千字节)绰绰有余。
- 用户语料**多为扫描件**，且**文字层可能与图像页不对齐**（劣质 OCR 版）——以 OCR 结果为准，别拿旧 poppler 文字层比对完整性。
- `/parse_pages` JSON 契约：`{"elements":[{type∈{Heading,Text,Table,Formula,Figure}区分大小写, page_no, text, table_html, caption, ocr_confidence, ...}]}`，错误 `{"error":"..."}`。
