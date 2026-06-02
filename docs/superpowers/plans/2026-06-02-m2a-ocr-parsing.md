# M2a 扫描件 OCR 解析（可插拔引擎 + 逐页路由）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让扫描/图文混合 PDF 经 OCR 进入现有入库管道——逐页路由（文字页 poppler、扫描页 PP-Structure），产出条款并可被 query 命中；解析层做成可插拔多引擎，富 IR 落磁盘缓存备下一轮。

**Architecture:** 契约①`Parser` 不变；新增 `HybridParser`（逐页路由 + 合并），它持有一个可插拔 `OcrBackend`（本轮实现 `PpStructureBackend`，HTTP 调 Python 服务；MinerU/VL-API/Tesseract 预留接口）。路由/合并/JSON 归一化/缓存读写全是纯函数走 TDD；OCR 走独立 Python FastAPI 服务。入库沿用 M1 的 `split_clauses`→PG→embed→Milvus，仅多一步写富 IR 磁盘缓存。

**Tech Stack:** 延续 M1——C++17/20、VS2022/MSBuild、vcpkg、libpqxx、cpp-httplib、nlohmann/json、spdlog、doctest、poppler-cpp。新增 Python 侧：FastAPI/uvicorn + PaddleOCR(PP-StructureV3) + PyMuPDF。

来源 spec：[2026-06-02-m2a-ocr-parsing-design.md](../specs/2026-06-02-m2a-ocr-parsing-design.md)

> **构建/测试/工程登记约定（贯穿全计划）：**
> - 构建（PowerShell，勿用 git-bash，`/p:` 会被转义）：`& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`
> - 跑测试：运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest，全绿即过）。
> - 跑应用：从**项目根目录**运行 `rag2.0\x64\Debug\rag2.0.exe`（`config.json`、`src/db/schema.sql` 是相对路径）。
> - **本仓库用显式源文件清单（非通配符）：新增 `.cpp`/`.h` 必须同时登记到 `rag2.0\rag2.0.vcxproj` 与 `rag2.0.tests\rag2.0.tests.vcxproj` 的 `<ClCompile>`/`<ClInclude>`，以及各自 `.filters`。** 测试工程需把被测 src `.cpp` 也加进去。仅修改已有文件的任务无需改工程文件。
> - 配置走根目录 `config.json`（已 gitignore），模板 `config.example.json`。源码强制 `/utf-8`，文件存 UTF-8。

---

## 文件结构（本计划新建/修改）

```
修改:
  src/parse/parser.h                  契约①加性扩展：ElementType / ParseElement / ParsedDoc.elements+standard_no + 枚举↔字符串
  src/rag_config.h, src/config.cpp    新增 4 个配置项 + config.example.json
  config.example.json                 同步新增 4 键
  src/ingest/ingest_pipeline.h/.cpp   parse 后写富 IR 缓存（其余沿用 M1）
  src/main.cpp                        cmd_ingest 按 config 构建解析器（HybridParser）
新建:
  src/parse/parse_cache.h/.cpp        ParsedDoc ↔ JSON + 磁盘读写（纯逻辑 TDD）
  src/parse/ocr_backend.h             OcrBackend 抽象接口（仅头）
  src/parse/ppstructure_backend.h/.cpp PP-Structure HTTP 后端 + JSON 归一化（纯逻辑 TDD）
  src/parse/hybrid_parser.h/.cpp      逐页路由(pick_ocr_pages)+合并(merge_doc)（纯逻辑 TDD）+ parse 编排
  src/parse/parser_factory.h/.cpp     按 config 造 OcrBackend / ParseMode（纯逻辑 TDD）
  tests/test_parse_cache.cpp
  tests/test_ppstructure_normalize.cpp
  tests/test_hybrid_router.cpp
  tests/test_parser_factory.cpp
  services/ppstructure/app.py         PP-Structure FastAPI 服务
  services/ppstructure/requirements.txt
```

---

### Task 1: 契约① IR 加性扩展（parser.h）

**Files:**
- Modify: `src/parse/parser.h`

仅加字段/类型，保留 `ParsedPage`/`pages`，不破坏 M1 既有调用。

- [ ] **Step 1: 整文件替换 `src/parse/parser.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 归一化元素类型：poppler（降级，仅 Text）与 OCR 后端（完整）都产出。
enum class ElementType { Heading, Text, Table, Formula, Figure };

inline std::string element_type_to_string(ElementType t) {
    switch (t) {
        case ElementType::Heading: return "Heading";
        case ElementType::Table:   return "Table";
        case ElementType::Formula: return "Formula";
        case ElementType::Figure:  return "Figure";
        default:                   return "Text";
    }
}
inline ElementType element_type_from_string(const std::string& s) {
    if (s == "Heading") return ElementType::Heading;
    if (s == "Table")   return ElementType::Table;
    if (s == "Formula") return ElementType::Formula;
    if (s == "Figure")  return ElementType::Figure;
    return ElementType::Text;
}

// 归一化元素：按阅读顺序排列。poppler 页产出 Text；OCR 页产出完整结构。
struct ParseElement {
    ElementType type = ElementType::Text;
    int page_no = 0;             // 从 1 开始
    int level = 0;               // Heading 层级，非标题为 0
    std::string clause_no;       // 元素自带条款号（可空）
    std::string title;           // 标题文本（Heading）
    std::string text;            // 正文 / OCR 文本
    std::string table_html;      // 表格 HTML（Table 类型）
    std::string caption;         // 表/图题
    std::string source;          // "poppler" | "ppstructure" | ...
    float ocr_confidence = 1.0f;
};

// 统一中间格式（IR）。M1 仅用 pages；M2a 加 elements（富 IR）与 standard_no。
struct ParsedPage {
    int page_no = 0;       // 从 1 开始
    std::string text;      // 该页全文
};

struct ParsedDoc {
    std::string source_path;
    std::string title;                       // 可为文件名
    std::string standard_no;                 // 可空，由 extract_standard_no 回填
    std::vector<ParsedPage> pages;           // 每页全文
    std::vector<ParseElement> elements;      // 归一化元素流（富 IR，本轮仅缓存）
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual ParsedDoc parse(const std::string& file_path) = 0;
};
```

- [ ] **Step 2: 构建验证（确保 M1 未被破坏）**

Run: MSBuild 构建 + 跑测试 exe。
Expected: 成功；既有 28 测试仍全绿（仅新增字段/类型，未删旧字段）。

- [ ] **Step 3: Commit**

```
git add src/parse/parser.h
git commit -m "feat(m2a): extend Parser IR with element stream (additive, contract 1)"
```

---

### Task 2: 富 IR 磁盘缓存（ParsedDoc ↔ JSON，纯逻辑 TDD）

**Files:**
- Create: `src/parse/parse_cache.h`, `src/parse/parse_cache.cpp`
- Test: `tests/test_parse_cache.cpp`
- Modify（登记）: 两个 `.vcxproj` + 两个 `.filters`

- [ ] **Step 1: 写失败测试 `tests/test_parse_cache.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/parse_cache.h"

TEST_CASE("parsed_doc JSON round-trip preserves pages and elements") {
    ParsedDoc d;
    d.source_path = "C:/a.pdf";
    d.title = "a.pdf";
    d.standard_no = "JTG 3432-2024";
    { ParsedPage p; p.page_no = 1; p.text = "第一页文本"; d.pages.push_back(p); }
    { ParseElement e; e.type = ElementType::Table; e.page_no = 1;
      e.table_html = "<table><tr><td>x</td></tr></table>"; e.caption = "表1";
      e.source = "ppstructure"; e.ocr_confidence = 0.9f; d.elements.push_back(e); }

    std::string json = parsed_doc_to_json(d);
    ParsedDoc r = parsed_doc_from_json(json);

    CHECK(r.source_path == "C:/a.pdf");
    CHECK(r.standard_no == "JTG 3432-2024");
    REQUIRE(r.pages.size() == 1);
    CHECK(r.pages[0].text == "第一页文本");
    REQUIRE(r.elements.size() == 1);
    CHECK(r.elements[0].type == ElementType::Table);
    CHECK(r.elements[0].table_html.find("<table>") != std::string::npos);
    CHECK(r.elements[0].caption == "表1");
    CHECK(r.elements[0].source == "ppstructure");
}
```

- [ ] **Step 2: 登记测试文件并构建，确认失败**

在 `rag2.0.tests\rag2.0.tests.vcxproj` 测试 `<ClCompile>` 组（`test_standard_meta.cpp` 之后）加：
```xml
    <ClCompile Include="..\tests\test_parse_cache.cpp" />
```
在 `rag2.0.tests\rag2.0.tests.vcxproj.filters` 的 `测试` 组加：
```xml
    <ClCompile Include="..\tests\test_parse_cache.cpp"><Filter>测试</Filter></ClCompile>
```
Run: MSBuild。Expected: 编译 FAIL（`parse/parse_cache.h` 不存在）。

- [ ] **Step 3: 写 `src/parse/parse_cache.h`**

```cpp
#pragma once
#include <string>
#include "parse/parser.h"

// ParsedDoc ↔ JSON（含 pages 与 elements）。纯函数，可单测。
std::string parsed_doc_to_json(const ParsedDoc& doc);
ParsedDoc   parsed_doc_from_json(const std::string& json_text);

// 磁盘缓存：写/读 data/parse_cache/<标准>.json（目录不存在则创建）。
void        write_parse_cache(const std::string& cache_path, const ParsedDoc& doc);
```

- [ ] **Step 4: 写 `src/parse/parse_cache.cpp`**

```cpp
#include "parse/parse_cache.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using nlohmann::json;

std::string parsed_doc_to_json(const ParsedDoc& doc) {
    json j;
    j["source_path"] = doc.source_path;
    j["title"] = doc.title;
    j["standard_no"] = doc.standard_no;
    j["pages"] = json::array();
    for (const auto& p : doc.pages)
        j["pages"].push_back({ {"page_no", p.page_no}, {"text", p.text} });
    j["elements"] = json::array();
    for (const auto& e : doc.elements) {
        j["elements"].push_back({
            {"type", element_type_to_string(e.type)},
            {"page_no", e.page_no}, {"level", e.level},
            {"clause_no", e.clause_no}, {"title", e.title}, {"text", e.text},
            {"table_html", e.table_html}, {"caption", e.caption},
            {"source", e.source}, {"ocr_confidence", e.ocr_confidence}
        });
    }
    return j.dump(2);
}

ParsedDoc parsed_doc_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    ParsedDoc d;
    if (!j.is_object()) return d;
    d.source_path = j.value("source_path", "");
    d.title = j.value("title", "");
    d.standard_no = j.value("standard_no", "");
    if (j.contains("pages"))
        for (auto& p : j["pages"]) {
            ParsedPage pp;
            pp.page_no = p.value("page_no", 0);
            pp.text = p.value("text", "");
            d.pages.push_back(std::move(pp));
        }
    if (j.contains("elements"))
        for (auto& e : j["elements"]) {
            ParseElement pe;
            pe.type = element_type_from_string(e.value("type", "Text"));
            pe.page_no = e.value("page_no", 0);
            pe.level = e.value("level", 0);
            pe.clause_no = e.value("clause_no", "");
            pe.title = e.value("title", "");
            pe.text = e.value("text", "");
            pe.table_html = e.value("table_html", "");
            pe.caption = e.value("caption", "");
            pe.source = e.value("source", "");
            pe.ocr_confidence = e.value("ocr_confidence", 1.0f);
            d.elements.push_back(std::move(pe));
        }
    return d;
}

void write_parse_cache(const std::string& cache_path, const ParsedDoc& doc) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary);
    f << parsed_doc_to_json(doc);
}
```

- [ ] **Step 5: 登记实现文件到两个工程**

`rag2.0\rag2.0.vcxproj`：`<ClCompile>` 组（`standard_meta.cpp` 后）加 `    <ClCompile Include="..\src\parse\parse_cache.cpp" />`；`<ClInclude>` 组加 `    <ClInclude Include="..\src\parse\parse_cache.h" />`。
`rag2.0\rag2.0.vcxproj.filters`：源加 `    <ClCompile Include="..\src\parse\parse_cache.cpp"><Filter>源文件\parse</Filter></ClCompile>`；头加 `    <ClInclude Include="..\src\parse\parse_cache.h"><Filter>头文件\parse</Filter></ClInclude>`。
`rag2.0.tests\rag2.0.tests.vcxproj`：被测 `<ClCompile>` 加 `    <ClCompile Include="..\src\parse\parse_cache.cpp" />`；`<ClInclude>` 加 `    <ClInclude Include="..\src\parse\parse_cache.h" />`。
`rag2.0.tests\rag2.0.tests.vcxproj.filters`：`被测源码` 加 `    <ClCompile Include="..\src\parse\parse_cache.cpp"><Filter>被测源码</Filter></ClCompile>`；`头文件` 加 `    <ClInclude Include="..\src\parse\parse_cache.h"><Filter>头文件</Filter></ClInclude>`。

- [ ] **Step 6: 构建并跑测试，确认全绿**

Run: MSBuild + 测试 exe。Expected: 全部 PASS（含新 round-trip 用例）。

- [ ] **Step 7: Commit**

```
git add src/parse/parse_cache.h src/parse/parse_cache.cpp tests/test_parse_cache.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2a): ParsedDoc<->JSON parse cache (TDD)"
```

---

### Task 3: OcrBackend 接口 + PP-Structure 后端（JSON 归一化 TDD + HTTP）

**Files:**
- Create: `src/parse/ocr_backend.h`, `src/parse/ppstructure_backend.h`, `src/parse/ppstructure_backend.cpp`
- Test: `tests/test_ppstructure_normalize.cpp`
- Modify（登记）: 两个 `.vcxproj` + 两个 `.filters`

- [ ] **Step 1: 写失败测试 `tests/test_ppstructure_normalize.cpp`（纯解析，不触网）**

```cpp
#include <doctest/doctest.h>
#include "parse/ppstructure_backend.h"

TEST_CASE("parse_ppstructure_json maps service JSON to elements") {
    std::string json = R"({
      "elements": [
        {"type":"Heading","page_no":91,"level":1,"title":"6 允许误差","text":"6 允许误差",
         "clause_no":"","table_html":"","caption":"","ocr_confidence":0.97},
        {"type":"Text","page_no":91,"text":"6.2 允许误差为平均值的10%。","ocr_confidence":0.95},
        {"type":"Table","page_no":50,"table_html":"<table><tr><td>96</td></tr></table>",
         "caption":"表5","ocr_confidence":1.0}
      ]
    })";
    auto els = parse_ppstructure_json(json);
    REQUIRE(els.size() == 3);
    CHECK(els[0].type == ElementType::Heading);
    CHECK(els[0].page_no == 91);
    CHECK(els[1].text.find("允许误差为平均值") != std::string::npos);
    CHECK(els[2].type == ElementType::Table);
    CHECK(els[2].table_html.find("<table>") != std::string::npos);
    CHECK(els[2].caption == "表5");
    CHECK(els[0].source == "ppstructure");   // 后端统一打标
}

TEST_CASE("parse_ppstructure_json on error payload throws") {
    CHECK_THROWS(parse_ppstructure_json(R"({"error":"boom"})"));
}
```

- [ ] **Step 2: 登记测试并构建，确认失败**

`rag2.0.tests\rag2.0.tests.vcxproj`（测试组）加 `    <ClCompile Include="..\tests\test_ppstructure_normalize.cpp" />`；`.filters` 的 `测试` 组加对应行。
Run: MSBuild。Expected: 编译 FAIL（头不存在）。

- [ ] **Step 3: 写 `src/parse/ocr_backend.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "parse/parser.h"

// 可插拔 OCR 后端：对指定页做 OCR，返回这些页的归一化元素。
// 本轮实现 PpStructureBackend；MinerU/VL-API/Tesseract 实现此接口即可接入，HybridParser 不改。
class OcrBackend {
public:
    virtual ~OcrBackend() = default;
    virtual std::vector<ParseElement> ocr_pages(const std::string& file_path,
                                                const std::vector<int>& pages) = 0;
};
```

- [ ] **Step 4: 写 `src/parse/ppstructure_backend.h`**

```cpp
#pragma once
#include "parse/ocr_backend.h"
#include <string>

// 纯函数：把 PP-Structure 服务返回 JSON 解析为元素流（含 error 检测）。可单测。
std::vector<ParseElement> parse_ppstructure_json(const std::string& json_body);

class PpStructureBackend : public OcrBackend {
public:
    explicit PpStructureBackend(std::string base_url);
    std::vector<ParseElement> ocr_pages(const std::string& file_path,
                                        const std::vector<int>& pages) override;
private:
    std::string base_url_;
};
```

- [ ] **Step 5: 写 `src/parse/ppstructure_backend.cpp`**

```cpp
#include "parse/ppstructure_backend.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

std::vector<ParseElement> parse_ppstructure_json(const std::string& json_body) {
    auto j = json::parse(json_body);   // 解析失败抛异常（交由上层 try/catch）
    if (j.contains("error"))
        throw std::runtime_error("ppstructure service error: " +
                                 j["error"].get<std::string>());
    std::vector<ParseElement> out;
    if (!j.contains("elements")) return out;
    for (auto& e : j["elements"]) {
        ParseElement pe;
        pe.type = element_type_from_string(e.value("type", "Text"));
        pe.page_no = e.value("page_no", 0);
        pe.level = e.value("level", 0);
        pe.clause_no = e.value("clause_no", "");
        pe.title = e.value("title", "");
        pe.text = e.value("text", "");
        pe.table_html = e.value("table_html", "");
        pe.caption = e.value("caption", "");
        pe.ocr_confidence = e.value("ocr_confidence", 1.0f);
        pe.source = "ppstructure";
        out.push_back(std::move(pe));
    }
    return out;
}

PpStructureBackend::PpStructureBackend(std::string base_url)
    : base_url_(std::move(base_url)) {}

std::vector<ParseElement> PpStructureBackend::ocr_pages(const std::string& file_path,
                                                        const std::vector<int>& pages) {
    json body;
    body["file_path"] = file_path;
    body["pages"] = pages;
    auto res = http::post_json(base_url_, "/parse_pages", body.dump(), {});
    if (!res.ok())
        throw std::runtime_error("ppstructure /parse_pages failed: " + res.body + res.error);
    return parse_ppstructure_json(res.body);
}
```

- [ ] **Step 6: 登记实现到两个工程**

`rag2.0\rag2.0.vcxproj`：`<ClCompile>` 加 `..\src\parse\ppstructure_backend.cpp`；`<ClInclude>` 加 `..\src\parse\ocr_backend.h` 与 `..\src\parse\ppstructure_backend.h`。
`rag2.0\rag2.0.vcxproj.filters`：源 `ppstructure_backend.cpp`→`源文件\parse`；头 `ocr_backend.h`/`ppstructure_backend.h`→`头文件\parse`。
`rag2.0.tests\rag2.0.tests.vcxproj` + `.filters`：同样加 `ppstructure_backend.cpp`（被测源码）与两个头（头文件）。

- [ ] **Step 7: 构建 + 测试全绿**

Run: MSBuild + 测试 exe。Expected: PASS（含 2 个 ppstructure 用例）。

- [ ] **Step 8: Commit**

```
git add src/parse/ocr_backend.h src/parse/ppstructure_backend.h src/parse/ppstructure_backend.cpp tests/test_ppstructure_normalize.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2a): OcrBackend interface + PP-Structure HTTP backend with testable JSON normalize"
```

---

### Task 4: HybridParser —— 逐页路由 + 合并（纯逻辑 TDD + parse 编排）

**Files:**
- Create: `src/parse/hybrid_parser.h`, `src/parse/hybrid_parser.cpp`
- Test: `tests/test_hybrid_router.cpp`
- Modify（登记）: 两个 `.vcxproj` + 两个 `.filters`

- [ ] **Step 1: 写失败测试 `tests/test_hybrid_router.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/hybrid_parser.h"

TEST_CASE("pick_ocr_pages: auto mode picks pages below threshold") {
    // 每页字节数（1-based 页号）：第2页稀疏
    std::vector<int> chars = {2000, 6, 1500};
    auto pages = pick_ocr_pages(chars, ParseMode::Auto, 100);
    REQUIRE(pages.size() == 1);
    CHECK(pages[0] == 2);
}
TEST_CASE("pick_ocr_pages: poppler mode picks none, ocr mode picks all") {
    std::vector<int> chars = {2000, 6, 1500};
    CHECK(pick_ocr_pages(chars, ParseMode::Poppler, 100).empty());
    CHECK(pick_ocr_pages(chars, ParseMode::Ocr, 100).size() == 3);
}

TEST_CASE("merge_doc: ocr pages replaced, poppler pages kept, elements merged") {
    ParsedDoc base;
    base.source_path = "C:/a.pdf";
    { ParsedPage p; p.page_no=1; p.text="文字页一"; base.pages.push_back(p); }
    { ParsedPage p; p.page_no=2; p.text="";        base.pages.push_back(p); } // 扫描页(空)
    std::vector<int> ocr_pages = {2};
    std::vector<ParseElement> ocr_els;
    { ParseElement e; e.type=ElementType::Text; e.page_no=2; e.text="OCR出来的二页"; ocr_els.push_back(e); }
    { ParseElement e; e.type=ElementType::Table; e.page_no=2; e.table_html="<table></table>"; ocr_els.push_back(e); }

    ParsedDoc out = merge_doc(base, ocr_pages, ocr_els);

    REQUIRE(out.pages.size() == 2);
    CHECK(out.pages[0].text == "文字页一");               // poppler 页保留
    CHECK(out.pages[1].text.find("OCR出来的二页") != std::string::npos); // 扫描页用 OCR 文本
    // elements：第1页 1 个 Text(poppler) + 第2页 2 个(ocr)
    int p1=0,p2=0; for (auto& e: out.elements){ if(e.page_no==1)++p1; if(e.page_no==2)++p2; }
    CHECK(p1 == 1);
    CHECK(p2 == 2);
}
```

- [ ] **Step 2: 登记测试并构建，确认失败**

`rag2.0.tests` vcxproj+filters 加 `tests\test_hybrid_router.cpp`。
Run: MSBuild。Expected: 编译 FAIL（头不存在）。

- [ ] **Step 3: 写 `src/parse/hybrid_parser.h`**

```cpp
#pragma once
#include <vector>
#include <string>
#include "parse/parser.h"
#include "parse/ocr_backend.h"

enum class ParseMode { Auto, Poppler, Ocr };

// 纯函数：给定每页字节数，按模式与阈值算出需 OCR 的页号（1-based）。
std::vector<int> pick_ocr_pages(const std::vector<int>& bytes_per_page,
                                ParseMode mode, int threshold);

// 纯函数：合并 poppler 基础文档与 OCR 页元素。
// - OCR 页：text = 该页 OCR 元素文本拼接；elements = 该页 OCR 元素。
// - 非 OCR 页：保留 poppler text；并为该页生成一个 Text 元素(source=poppler)。
// - elements 按页号升序排列。
ParsedDoc merge_doc(const ParsedDoc& poppler_doc,
                    const std::vector<int>& ocr_pages,
                    const std::vector<ParseElement>& ocr_elements);

// 契约① 实现：逐页路由（poppler 文字页 + OcrBackend 扫描页），合并为 ParsedDoc。
class HybridParser : public Parser {
public:
    HybridParser(Parser& poppler, OcrBackend& ocr, ParseMode mode, int threshold);
    ParsedDoc parse(const std::string& file_path) override;
private:
    Parser& poppler_;
    OcrBackend& ocr_;
    ParseMode mode_;
    int threshold_;
};
```

- [ ] **Step 4: 写 `src/parse/hybrid_parser.cpp`**

```cpp
#include "parse/hybrid_parser.h"
#include <algorithm>
#include <set>

std::vector<int> pick_ocr_pages(const std::vector<int>& bytes_per_page,
                                ParseMode mode, int threshold) {
    std::vector<int> out;
    for (size_t i = 0; i < bytes_per_page.size(); ++i) {
        int page = (int)i + 1;
        if (mode == ParseMode::Poppler) continue;
        if (mode == ParseMode::Ocr) { out.push_back(page); continue; }
        if (bytes_per_page[i] < threshold) out.push_back(page);   // Auto
    }
    return out;
}

ParsedDoc merge_doc(const ParsedDoc& base,
                    const std::vector<int>& ocr_pages,
                    const std::vector<ParseElement>& ocr_elements) {
    std::set<int> ocr_set(ocr_pages.begin(), ocr_pages.end());
    ParsedDoc out;
    out.source_path = base.source_path;
    out.title = base.title;
    out.standard_no = base.standard_no;

    for (const auto& p : base.pages) {
        ParsedPage np;
        np.page_no = p.page_no;
        if (ocr_set.count(p.page_no)) {
            std::string merged;
            for (const auto& e : ocr_elements)
                if (e.page_no == p.page_no && !e.text.empty()) {
                    if (!merged.empty()) merged += "\n";
                    merged += e.text;
                }
            np.text = merged;
        } else {
            np.text = p.text;
        }
        out.pages.push_back(std::move(np));
    }

    // elements 按页号升序：非 OCR 页贡献一个 poppler Text 元素；OCR 页贡献其 OCR 元素
    for (const auto& p : base.pages) {
        if (ocr_set.count(p.page_no)) {
            for (const auto& e : ocr_elements)
                if (e.page_no == p.page_no) out.elements.push_back(e);
        } else if (!p.text.empty()) {
            ParseElement e;
            e.type = ElementType::Text;
            e.page_no = p.page_no;
            e.text = p.text;
            e.source = "poppler";
            out.elements.push_back(std::move(e));
        }
    }
    return out;
}

HybridParser::HybridParser(Parser& poppler, OcrBackend& ocr, ParseMode mode, int threshold)
    : poppler_(poppler), ocr_(ocr), mode_(mode), threshold_(threshold) {}

ParsedDoc HybridParser::parse(const std::string& file_path) {
    ParsedDoc base = poppler_.parse(file_path);   // 每页文本（poppler 很快，即使抽不出）
    std::vector<int> bytes;
    bytes.reserve(base.pages.size());
    for (const auto& p : base.pages) bytes.push_back((int)p.text.size());

    auto ocr_pages = pick_ocr_pages(bytes, mode_, threshold_);
    std::vector<ParseElement> ocr_els;
    if (!ocr_pages.empty()) ocr_els = ocr_.ocr_pages(file_path, ocr_pages);

    return merge_doc(base, ocr_pages, ocr_els);
}
```

- [ ] **Step 5: 登记到两个工程**

`rag2.0\rag2.0.vcxproj`(+filters)：源 `..\src\parse\hybrid_parser.cpp`(`源文件\parse`)；头 `..\src\parse\hybrid_parser.h`(`头文件\parse`)。
`rag2.0.tests\rag2.0.tests.vcxproj`(+filters)：被测源 `hybrid_parser.cpp`；头 `hybrid_parser.h`。

- [ ] **Step 6: 构建 + 测试全绿**

Run: MSBuild + 测试 exe。Expected: PASS（pick_ocr_pages 2 例 + merge_doc 1 例）。

- [ ] **Step 7: Commit**

```
git add src/parse/hybrid_parser.h src/parse/hybrid_parser.cpp tests/test_hybrid_router.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2a): HybridParser per-page routing + merge (TDD)"
```

---

### Task 5: 配置新增 4 项（TDD）

**Files:**
- Modify: `src/rag_config.h`, `src/config.cpp`, `config.example.json`
- Test: `tests/test_config.cpp`

- [ ] **Step 1: 在 `tests/test_config.cpp` 末尾追加失败测试**

```cpp
TEST_CASE("Config parses M2a parser keys with defaults") {
    Config c = Config::from_json_string("{}");
    CHECK(c.parse_mode == "auto");
    CHECK(c.ocr_engine == "ppstructure");
    CHECK(c.ppstruct_base_url == "http://localhost:8001");
    CHECK(c.scan_chars_threshold == 100);
}
TEST_CASE("Config M2a parser keys can be overridden") {
    Config c = Config::from_json_string(
        R"({"RAG_PARSE_MODE":"ocr","RAG_OCR_ENGINE":"ppstructure",
            "RAG_PPSTRUCT_BASE_URL":"http://x:9","RAG_SCAN_CHARS_THRESHOLD":"50"})");
    CHECK(c.parse_mode == "ocr");
    CHECK(c.ppstruct_base_url == "http://x:9");
    CHECK(c.scan_chars_threshold == 50);
}
```

- [ ] **Step 2: 构建确认失败**

Run: MSBuild。Expected: 编译 FAIL（Config 无这些字段）。

- [ ] **Step 3: 在 `src/rag_config.h` 的 `Config` 加字段**

在 `std::string doc_path;` 之后追加：
```cpp
    // M2a 解析层
    std::string parse_mode;          // auto | poppler | ocr
    std::string ocr_engine;          // ppstructure（预留 mineru/vlapi/tesseract）
    std::string ppstruct_base_url;
    int         scan_chars_threshold = 100;  // 每页字节数低于此判为扫描页
```

- [ ] **Step 4: 在 `src/config.cpp` 的 `from_map` 里 `c.doc_path` 之后追加**

```cpp
    c.parse_mode          = get(e, "RAG_PARSE_MODE", "auto");
    c.ocr_engine          = get(e, "RAG_OCR_ENGINE", "ppstructure");
    c.ppstruct_base_url   = get(e, "RAG_PPSTRUCT_BASE_URL", "http://localhost:8001");
    c.scan_chars_threshold= std::stoi(get(e, "RAG_SCAN_CHARS_THRESHOLD", "100"));
```

- [ ] **Step 5: 在 `config.example.json` 追加 4 键**

在 `"RAG_DOC_PATH": ""` 之后（注意前一行补逗号）加：
```json
  "RAG_PARSE_MODE": "auto",
  "RAG_OCR_ENGINE": "ppstructure",
  "RAG_PPSTRUCT_BASE_URL": "http://localhost:8001",
  "RAG_SCAN_CHARS_THRESHOLD": "100"
```

- [ ] **Step 6: 构建 + 测试全绿**

Run: MSBuild + 测试 exe。Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/rag_config.h src/config.cpp config.example.json tests/test_config.cpp
git commit -m "feat(m2a): config keys for parse mode / ocr engine / ppstruct url / threshold (TDD)"
```

---

### Task 6: 工厂 + 接入 ingest（按 config 构建 HybridParser，parse 后写缓存）

**Files:**
- Create: `src/parse/parser_factory.h`, `src/parse/parser_factory.cpp`
- Test: `tests/test_parser_factory.cpp`
- Modify: `src/ingest/ingest_pipeline.h`, `src/ingest/ingest_pipeline.cpp`, `src/main.cpp`
- Modify（登记）: 两个 `.vcxproj` + 两个 `.filters`

- [ ] **Step 1: 写失败测试 `tests/test_parser_factory.cpp`（纯逻辑）**

```cpp
#include <doctest/doctest.h>
#include "parse/parser_factory.h"

TEST_CASE("parse_mode_from_string maps strings") {
    CHECK(parse_mode_from_string("auto") == ParseMode::Auto);
    CHECK(parse_mode_from_string("poppler") == ParseMode::Poppler);
    CHECK(parse_mode_from_string("ocr") == ParseMode::Ocr);
    CHECK(parse_mode_from_string("???") == ParseMode::Auto);  // 兜底 auto
}

TEST_CASE("make_ocr_backend builds ppstructure, throws for unimplemented") {
    auto b = make_ocr_backend("ppstructure", "http://localhost:8001");
    CHECK(b != nullptr);
    CHECK_THROWS(make_ocr_backend("mineru", "x"));
    CHECK_THROWS(make_ocr_backend("vlapi", "x"));
    CHECK_THROWS(make_ocr_backend("tesseract", "x"));
}
```

- [ ] **Step 2: 登记测试并构建确认失败**

`rag2.0.tests` vcxproj+filters 加 `tests\test_parser_factory.cpp`。Run MSBuild → FAIL（头不存在）。

- [ ] **Step 3: 写 `src/parse/parser_factory.h`**

```cpp
#pragma once
#include <memory>
#include <string>
#include "parse/hybrid_parser.h"
#include "parse/ocr_backend.h"

ParseMode parse_mode_from_string(const std::string& s);

// 按引擎名造 OCR 后端：本轮仅 "ppstructure"；其余抛 std::runtime_error。
std::unique_ptr<OcrBackend> make_ocr_backend(const std::string& engine,
                                             const std::string& base_url);
```

- [ ] **Step 4: 写 `src/parse/parser_factory.cpp`**

```cpp
#include "parse/parser_factory.h"
#include "parse/ppstructure_backend.h"
#include <stdexcept>

ParseMode parse_mode_from_string(const std::string& s) {
    if (s == "poppler") return ParseMode::Poppler;
    if (s == "ocr")     return ParseMode::Ocr;
    return ParseMode::Auto;
}

std::unique_ptr<OcrBackend> make_ocr_backend(const std::string& engine,
                                             const std::string& base_url) {
    if (engine == "ppstructure")
        return std::make_unique<PpStructureBackend>(base_url);
    throw std::runtime_error("OCR 引擎 '" + engine +
        "' 尚未实现（本轮仅支持 ppstructure；mineru/vlapi/tesseract 为预留）");
}
```

- [ ] **Step 5: 修改 `src/ingest/ingest_pipeline.h`，给 `ingest_file` 加缓存目录参数**

把 `ingest_file` 声明改为（末尾加一个带默认值的 `cache_dir`）：
```cpp
IngestResult ingest_file(const std::string& file_path,
                         Parser& parser,
                         PgClient& pg,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         const std::string& collection,
                         const std::string& cache_dir = "data/parse_cache");
```

- [ ] **Step 6: 修改 `src/ingest/ingest_pipeline.cpp`：parse 后写富 IR 缓存**

在顶部 include 区加：
```cpp
#include "parse/parse_cache.h"
```
把函数签名同步为带 `cache_dir`。然后找到现有这行（M1 已存在）：
```cpp
    std::string standard_id = make_id(file_path);
```
在它的**正下方**插入一行（此处 `standard_id` 与 `doc` 均已就绪）：
```cpp
    // M2a：把富 IR（含 OCR 页元素/表格）写磁盘缓存，供下一轮结构化层消费（不重复 OCR）
    write_parse_cache(cache_dir + "/" + standard_id + ".json", doc);
```
其余逻辑一律不变（仍 `split_clauses(page.text)`→PG→`embed(c.text)`→Milvus）。

- [ ] **Step 7: 修改 `src/main.cpp` 的 `cmd_ingest`：按 config 构建 HybridParser**

在 include 区追加：
```cpp
#include "parse/hybrid_parser.h"
#include "parse/parser_factory.h"
```
把 `cmd_ingest` 里构建解析器并调用 `ingest_file` 的部分改为：
```cpp
        PopplerParser poppler;
        auto backend = make_ocr_backend(cfg.ocr_engine, cfg.ppstruct_base_url);
        HybridParser parser(poppler, *backend,
                            parse_mode_from_string(cfg.parse_mode),
                            cfg.scan_chars_threshold);

        auto r = ingest_file(cfg.doc_path, parser, pg, mv, embed, cfg.milvus_collection);
```
（原先的 `PopplerParser parser;` 与对应 `ingest_file(... parser ...)` 调用整体替换为上面这段；`cmd_ingest` 的 try/catch 外框保留，`make_ocr_backend` 抛出的"引擎未实现"会被现有 catch 打印为可读错误。）

- [ ] **Step 8: 登记 `parser_factory` 到两个工程**

`rag2.0\rag2.0.vcxproj`(+filters)：源 `..\src\parse\parser_factory.cpp`(`源文件\parse`)；头 `..\src\parse\parser_factory.h`(`头文件\parse`)。
`rag2.0.tests\rag2.0.tests.vcxproj`(+filters)：被测源 `parser_factory.cpp`；头 `parser_factory.h`。

- [ ] **Step 9: 构建 + 全部测试绿**

Run: MSBuild（app+tests 均过）+ 测试 exe 全绿。
Expected: 成功；`rag2.0.exe` 默认 `auto` 模式：原生 PDF 仍 poppler、扫描页转 PP-Structure（需服务，见 Task 7）。

- [ ] **Step 10: Commit**

```
git add src/parse/parser_factory.h src/parse/parser_factory.cpp tests/test_parser_factory.cpp src/ingest/ingest_pipeline.h src/ingest/ingest_pipeline.cpp src/main.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2a): wire ingest to config-selected HybridParser + write parse cache"
```

---

### Task 7: PP-Structure FastAPI 服务（Python）

**Files:**
- Create: `services/ppstructure/app.py`, `services/ppstructure/requirements.txt`

> PaddleOCR/PP-StructureV3 的返回结构随版本略有差异；本任务给出可用形态，部署时按实际版本微调 `_to_elements` 的字段映射（这是本任务的主要开放项）。用 PyMuPDF 渲染指定页为图，避免依赖系统 poppler。

- [ ] **Step 1: 写 `services/ppstructure/requirements.txt`**

```
fastapi
uvicorn[standard]
paddlepaddle-gpu
paddleocr
pymupdf
pillow
numpy
```
（无 GPU 时把 `paddlepaddle-gpu` 换成 `paddlepaddle`。）

- [ ] **Step 2: 写 `services/ppstructure/app.py`**

```python
import os
from fastapi import FastAPI
from pydantic import BaseModel
import fitz                      # PyMuPDF
import numpy as np
from PIL import Image
from paddleocr import PPStructureV3

app = FastAPI()
_pipeline = PPStructureV3()      # 首次加载模型较慢

class PagesReq(BaseModel):
    file_path: str
    pages: list[int] = []        # 1-based；空表示全篇

class FileReq(BaseModel):
    file_path: str

def _render_page(doc, page_index_0based, dpi=200):
    page = doc[page_index_0based]
    pix = page.get_pixmap(dpi=dpi)
    img = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
    return np.array(img)

def _to_elements(result, page_no):
    """把 PP-StructureV3 单页结果映射为 IR 元素。字段名随版本调整。"""
    elements = []
    # PP-StructureV3 结果通常含版面块列表，每块有 type 与内容。
    blocks = []
    if isinstance(result, dict):
        blocks = result.get("parsing_res_list") or result.get("layout") or []
    elif isinstance(result, list):
        blocks = result
    for b in blocks:
        btype = (b.get("type") or b.get("label") or "text").lower()
        if "table" in btype:
            elements.append({"type": "Table", "page_no": page_no,
                             "table_html": b.get("html", b.get("res", "")) or "",
                             "caption": "", "text": "", "ocr_confidence": 1.0})
        elif "title" in btype or "header" in btype:
            txt = b.get("text", "") or ""
            elements.append({"type": "Heading", "page_no": page_no, "level": 1,
                             "title": txt, "text": txt, "ocr_confidence": 1.0})
        else:
            txt = b.get("text", "") or ""
            if txt.strip():
                elements.append({"type": "Text", "page_no": page_no,
                                 "text": txt, "ocr_confidence": 1.0})
    return elements

def _parse(file_path, pages):
    if not os.path.exists(file_path):
        return {"error": f"file not found: {file_path}"}
    doc = fitz.open(file_path)
    page_nums = pages if pages else list(range(1, doc.page_count + 1))
    all_elems = []
    for pno in page_nums:
        if pno < 1 or pno > doc.page_count:
            continue
        try:
            img = _render_page(doc, pno - 1)
            res = _pipeline.predict(input=img)
            res0 = res[0] if isinstance(res, list) and res else res
            all_elems.extend(_to_elements(res0, pno))
        except Exception as ex:    # 单页失败不终止全篇
            all_elems.append({"type": "Text", "page_no": pno, "text": "",
                              "caption": f"[page {pno} ocr failed: {ex}]",
                              "ocr_confidence": 0.0})
    return {"elements": all_elems}

@app.post("/parse_pages")
def parse_pages(req: PagesReq):
    return _parse(req.file_path, req.pages)

@app.post("/parse")
def parse(req: FileReq):
    return _parse(req.file_path, [])

@app.get("/health")
def health():
    return {"status": "ok"}
```

- [ ] **Step 3: 安装并启动服务**

Run（PowerShell，独立 venv）:
```
python -m venv services\ppstructure\.venv
services\ppstructure\.venv\Scripts\pip install -r services\ppstructure\requirements.txt
services\ppstructure\.venv\Scripts\python -m uvicorn services.ppstructure.app:app --host 0.0.0.0 --port 8001
```
Expected: uvicorn 启动；`curl http://localhost:8001/health` 返回 `{"status":"ok"}`。
（PaddleOCR 首次会下载模型；GPU 用 `paddlepaddle-gpu`，1660Ti 6GB 用 pipeline 默认即可。）

- [ ] **Step 4: 单页联调验证字段映射**

Run（用你的扫描 PDF 第 91 页之类有内容的页）:
```
curl -s -X POST http://localhost:8001/parse_pages -H "Content-Type: application/json" -d "{\"file_path\":\"D:/.../xxx.pdf\",\"pages\":[91]}"
```
Expected: 返回 `{"elements":[...]}` 含该页文本/表格。**若 elements 为空或字段不对，按实际返回结构修 `_to_elements`**（PP-StructureV3 版本差异点）。

- [ ] **Step 5: Commit**

```
git add services/ppstructure/app.py services/ppstructure/requirements.txt
git commit -m "feat(m2a): PP-Structure FastAPI service (per-page OCR -> normalized elements)"
```

---

### Task 8: 端到端验收（需服务 + 真实 PDF，用户环境）

**Files:** 无（运行验证）

**前置：** PP-Structure 服务已在 8001 运行；config.json 已设好（PG/Milvus/embedding/DeepSeek key + `RAG_DOC_PATH` 指向**扫描或混合** PDF）。从项目根目录运行；控制台先 `chcp 65001`。

- [ ] **Step 1: dump 看抽取量跃升**

Run: `.\rag2.0\x64\Debug\rag2.0.exe dump`
Expected: 对扫描 PDF，抽取字节从 M1 的 ~2KB 跃升到几十/几百 KB（OCR 生效）。

- [ ] **Step 2: ingest 出条款**

Run: `.\rag2.0\x64\Debug\rag2.0.exe ingest`
Expected: `clauses>0`（M1 上为 0 或仅数条）；`data/parse_cache/<id>.json` 生成且含 `elements`。

- [ ] **Step 3: 逐页路由可观察（混合 PDF）**

用一份混合 PDF（部分文字页 + 部分扫描页）跑 `ingest`，确认只对扫描页调了 OCR（可在 Python 服务日志看到收到的 `pages` 列表只含扫描页）。

- [ ] **Step 4: query 回归 + 命中**

Run: `.\rag2.0\x64\Debug\rag2.0.exe query "<该文档某条款相关问题>"`
Expected: 返回回答并引用文档中真实存在的条款号。

- [ ] **Step 5: 引擎切换验证**

把 config.json 的 `RAG_OCR_ENGINE` 改为 `mineru` 跑 `ingest` → 应打印"引擎 'mineru' 尚未实现…"的可读错误（不崩）。改回 `ppstructure`。

- [ ] **Step 6: 全部单测无回归**

Run: `rag2.0.tests\x64\Debug\rag2.0.tests.exe`
Expected: 全绿（M1 既有 + M2a 新增 parse_cache / ppstructure_normalize / hybrid_router / parser_factory / config）。

> **M2a 完成判据：** 扫描/混合 PDF 经 `ingest` 写入 `clauses>0`；`query` 引用真实条款号；逐页路由可观察（扫描页才 OCR）；富 IR 落 `data/parse_cache/`；`RAG_PARSE_MODE`/`RAG_OCR_ENGINE` 可切换、选未实现引擎给清晰报错；全部单测 PASS、M1 query 回归不劣化。

---

## 自检结果（Spec 覆盖核对）

对照 spec §1 范围与各节：

- **可插拔多引擎 + 逐页路由（§2/§3）** → Task 3（OcrBackend/PpStructureBackend）+ Task 4（HybridParser/pick_ocr_pages/merge_doc）+ Task 6（factory 选择）✅
- **PP-Structure 默认引擎、HTTP 服务（§5）** → Task 3 客户端 + Task 7 Python 服务 ✅
- **契约① IR 加性扩展（§4）** → Task 1 ✅
- **富 IR 磁盘缓存（§6）** → Task 2（序列化/写）+ Task 6 Step 6（ingest 调用 write_parse_cache）✅
- **入库复用 M1（§7）** → Task 6（ingest_file 仍 split→PG→embed→Milvus，仅加缓存）✅
- **config 4 键（§8）** → Task 5 ✅
- **错误处理（§9）** → Task 6（make_ocr_backend 抛可读错误，复用 cmd_ingest try/catch）+ Task 7（单页失败跳过）✅
- **MinerU/VL/Tesseract 预留接口（§12）** → Task 3 `OcrBackend` 抽象 + Task 6 `make_ocr_backend` 抛未实现 ✅
- **测试策略（§10）** → Task 2/3/4/5/6 纯逻辑 TDD；Task 8 端到端 ✅
- **不做结构化/不碰 PG schema（§1 Out of scope）** → 全程未建结构化表、未改 schema.sql ✅

类型/签名一致性核对：`ParseMode`（Task4 定义，Task5 配置字符串、Task6 `parse_mode_from_string` 映射、Task4 `pick_ocr_pages` 使用）一致；`OcrBackend`（Task3 定义，Task4 HybridParser 依赖，Task6 工厂构造）一致；`ParseElement`/`ParsedDoc.elements`（Task1）被 Task2/3/4 一致使用；`parse_ppstructure_json`（Task3 定义+测试）；`ingest_file` 新增 `cache_dir` 默认参数（Task5→6，Task6 main 调用沿用默认）一致。无占位符。

## 开放项（执行时确认）

1. **PP-StructureV3 返回结构**随版本不同，`_to_elements` 的字段映射需按 Task 7 Step 4 实测结果微调——这是本计划唯一不确定点。
2. `RAG_SCAN_CHARS_THRESHOLD` 默认 100（**字节**，非字符）；用真实文档校准（实测扫描 ~6、文字 ~2000 字节/页，区分度极大）。
3. 1660Ti 6GB：PP-Structure 用 pipeline 默认；若 OOM，降 `dpi`（Task7 `_render_page`）或逐页已是最小粒度。
4. PaddleOCR `predict` 接口名/参数随版本（3.x 用 `predict(input=...)`）；若不符按所装版本调整。
