# M2b 解析质量（OCR 输出清洗与可信化）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `data/parse_cache/<id>.json` 的 OCR 元素干净可信——抠出 `clause_no`、过滤图表题、携带真实置信度、打区域标签、标记可疑项——为 M2c 结构化层提供干净输入契约。

**Architecture:** app.py 只如实上报（透传 `raw_label` + 真实置信度 + DPI300），所有语义判断放 C++ 归一化层（caption 过滤 → `parse_clause_no` 共享纯函数 → region 状态机 → 英文糊 → 异常标记），在 `HybridParser::parse` 末尾对 `ParsedDoc.elements` 做一遍 `normalize_parsed_doc`，再落缓存。不建树、不碰 PG/Milvus、不改检索。

**Tech Stack:** C++20 / VS2022 MSBuild + vcpkg；doctest；nlohmann/json；spdlog；Python FastAPI + PaddleOCR(PP-StructureV3)。

**Spec:** `docs/superpowers/specs/2026-06-03-m2b-ocr-quality-design.md`

---

## 工程约定（务必先读）

- **构建**（PowerShell，不用 git-bash）：
  `& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`
- **测试 exe**：`.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest）。跑单个用例：`... -tc="用例名"`。
- **应用 exe**：`.\rag2.0\x64\Debug\rag2.0.exe`，从项目根目录运行。
- **C++20、强制 `/utf-8`**（中文源码注释必须）。
- **新增 `.cpp/.h` 必须登记 4+2 处**（显式文件清单，非通配符）：
  - 应用工程：`rag2.0\rag2.0.vcxproj`（`<ClCompile>`/`<ClInclude>`）+ `rag2.0\rag2.0.vcxproj.filters`
  - 测试工程：`rag2.0.tests\rag2.0.tests.vcxproj` + `rag2.0.tests\rag2.0.tests.vcxproj.filters`（测试工程需把**被测 src .cpp** 也加进 `<ClCompile>`，以及 test 文件）
  - 已有 `ppstructure_backend.cpp/.h` 的登记行可作为锚点参考其格式。

**本计划新增的源文件（共 3 对 + 4 个测试）：**
| 文件 | 职责 |
|------|------|
| `src/parse/clause_no.{h,cpp}` | `parse_clause_no` 共享条款号文法（两档判定） |
| `src/parse/ocr_normalize.{h,cpp}` | caption 过滤 / region 状态机 / 英文糊 / 异常标记 / `normalize_parsed_doc` |
| `src/parse/ocr_metrics.{h,cpp}` | 从 `ParsedDoc` 算"OCR 干不干净"体检表 |
| `tests/test_clause_no.cpp` | parse_clause_no 单测 |
| `tests/test_ocr_normalize.cpp` | caption / region / garble / anomaly 单测 |
| `tests/test_ocr_metrics.cpp` | 指标计算单测 |
| `tests/test_parse_cache_m2b.cpp` | 新字段往返序列化单测 |

**修改的文件：** `src/parse/parser.h`、`src/parse/ppstructure_backend.cpp`、`src/parse/parse_cache.cpp`、`src/parse/hybrid_parser.cpp`、`src/main.cpp`、`services/ppstructure/app.py`。

---

## Task 1: IR 加字段（ParseElement / ParsedDoc + Region 枚举）

**Files:**
- Modify: `src/parse/parser.h`

仅改头文件（结构体 + inline 转换函数），不新增 .cpp，无需登记工程文件。

- [ ] **Step 1: 在 `parser.h` 加 Region 枚举与转换函数**

在 `ElementType` 的转换函数之后、`struct ParseElement` 之前插入：

```cpp
// 区域标签：M2b 顺序扫元素流打标，供 M2c 决定哪些入树/防撞号。
enum class Region { FrontMatter, Toc, Body, Appendix, Explanation };

inline std::string region_to_string(Region r) {
    switch (r) {
        case Region::FrontMatter: return "front_matter";
        case Region::Toc:         return "toc";
        case Region::Appendix:    return "appendix";
        case Region::Explanation: return "explanation";
        default:                  return "body";
    }
}
inline Region region_from_string(const std::string& s) {
    if (s == "front_matter") return Region::FrontMatter;
    if (s == "toc")          return Region::Toc;
    if (s == "appendix")     return Region::Appendix;
    if (s == "explanation")  return Region::Explanation;
    return Region::Body;
}
```

- [ ] **Step 2: 给 `ParseElement` 加 4 个字段**

在 `struct ParseElement` 末尾（`float ocr_confidence = 1.0f;` 之后）追加：

```cpp
    std::string raw_label;          // PP-Structure 原始块标签（如 table_title/paragraph_title），app.py 透传
    Region region = Region::Body;   // M2b 区域标签
    bool is_caption = false;        // 图/表题，非条款
    std::string suspect;            // "" | "seq" | "short"（异常标记，只标不修）
```

- [ ] **Step 3: 给 `ParsedDoc` 加 schema_version**

在 `struct ParsedDoc` 的 `std::vector<ParseElement> elements;` 之后追加：

```cpp
    int schema_version = 2;   // M1/M2a=1（隐式）；M2b 起为 2
```

- [ ] **Step 4: 构建验证（确保头文件改动可编译）**

Run: `& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: 编译成功（既有用例不受影响，新字段有默认值）。

- [ ] **Step 5: Commit**

```bash
git add src/parse/parser.h
git commit -m "feat(m2b): add raw_label/region/is_caption/suspect to ParseElement + schema_version"
```

---

## Task 2: `parse_clause_no` 共享条款号文法（核心）

**Files:**
- Create: `src/parse/clause_no.h`, `src/parse/clause_no.cpp`
- Test: `tests/test_clause_no.cpp`
- Register: `rag2.0.vcxproj`(+filters), `rag2.0.tests.vcxproj`(+filters)

- [ ] **Step 1: 写失败测试 `tests/test_clause_no.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/clause_no.h"

TEST_CASE("parse_clause_no: 多级号（宽松）") {
    auto r = parse_clause_no("5.2.1龟裂应按面积计算", false);
    CHECK(r.matched);
    CHECK(r.clause_no == "5.2.1");
    CHECK(r.rest == "龟裂应按面积计算");

    CHECK(parse_clause_no("2.0.1公路技术状况指数", false).clause_no == "2.0.1");
    CHECK(parse_clause_no("5.2.10泛油", false).clause_no == "5.2.10");
    CHECK(parse_clause_no("6.3.10路面结构强度", false).clause_no == "6.3.10");
    CHECK(parse_clause_no("4.2.1-1", false).clause_no == "4.2.1-1");
    CHECK(parse_clause_no("5.2沥青路面", false).clause_no == "5.2");
}

TEST_CASE("parse_clause_no: 单级号（章，仅 Heading + 后跟汉字）") {
    auto r = parse_clause_no("1总则", true);
    CHECK(r.matched);
    CHECK(r.clause_no == "1");
    CHECK(r.rest == "总则");

    CHECK(parse_clause_no("7公路技术状况评定", true).clause_no == "7");
    // 非 Heading 的单级号不认（避免正文里"5 个试样"误判）
    CHECK_FALSE(parse_clause_no("5 个试样", false).matched);
}

TEST_CASE("parse_clause_no: 数值负例不误判") {
    CHECK_FALSE(parse_clause_no("200kN加到", false).matched);   // 无点
    CHECK_FALSE(parse_clause_no("0.5%。", false).matched);      // 顶层段为 0
    CHECK_FALSE(parse_clause_no("5.2%。", false).matched);      // rest 以 % 起，判数值
    CHECK_FALSE(parse_clause_no("3.5mm 厚", false).matched);    // rest 以单位起
}
```

- [ ] **Step 2: 写头文件 `src/parse/clause_no.h`**

```cpp
#pragma once
#include <string>

// 条款号抠取结果。matched=false 表示该文本不以条款号起。
struct ClauseNoResult {
    bool matched = false;
    std::string clause_no;   // 如 "5.2.1" / "1" / "4.2.1-1"
    std::string rest;        // 抠号后剩余文本（已去首部空白）
};

// 共享条款号文法（poppler 前端与 OCR 前端共用）。纯函数，可单测。
// is_heading=true 时才允许"单级章号"（如 "1总则"），否则只认多级号（≥2 级）。
ClauseNoResult parse_clause_no(const std::string& text, bool is_heading);
```

- [ ] **Step 3: 写实现 `src/parse/clause_no.cpp`**

```cpp
#include "parse/clause_no.h"
#include <regex>

// 单位词/符号前缀：rest 以这些开头则判为数值，不当条款号。
static bool rest_starts_like_unit(const std::string& rest) {
    if (rest.empty()) return false;
    // 单字节单位符号
    static const std::string unit_bytes = "%";
    if (unit_bytes.find(rest[0]) != std::string::npos) return true;
    // 常见 ASCII 单位词
    static const char* units[] = {"mm", "cm", "km", "kN", "kg", "MPa", "kPa", "mL", "ml"};
    for (auto u : units) {
        size_t n = std::char_traits<char>::length(u);
        if (rest.compare(0, n, u) == 0) return true;
    }
    // 多字节单位（‰ U+2030 = E2 80 B0；° U+00B0 = C2 B0）
    if (rest.size() >= 3 && (unsigned char)rest[0]==0xE2 && (unsigned char)rest[1]==0x80 && (unsigned char)rest[2]==0xB0) return true;
    if (rest.size() >= 2 && (unsigned char)rest[0]==0xC2 && (unsigned char)rest[1]==0xB0) return true;
    return false;
}

// 第一个字符是否汉字（UTF-8 多字节、且非 ASCII）——单级章号要求号后跟汉字。
static bool starts_with_cjk(const std::string& s) {
    return !s.empty() && (unsigned char)s[0] >= 0x80;
}

static std::string ltrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a);
}

ClauseNoResult parse_clause_no(const std::string& text, bool is_heading) {
    ClauseNoResult out;
    std::string s = ltrim(text);

    // 档1：多级号 ≥2 级（含至少一个点）。后瞻要求号后是非数字非点或结尾。
    static const std::regex multi(R"(^(\d+(?:\.\d+)+(?:-[0-9A-Za-z]+)?)(?=[^\d.]|$)(.*)$)");
    std::smatch m;
    if (std::regex_match(s, m, multi)) {
        std::string no = m[1].str();
        std::string rest = ltrim(m[2].str());   // 先去前导空白再判守卫，避免 "5.2 mm" 漏判
        // 守卫：顶层段 ≥ 1（排除 0.5）；rest 不以单位起（排除 5.2%）
        if (no[0] != '0' && !rest_starts_like_unit(rest)) {
            out.matched = true; out.clause_no = no; out.rest = rest;
            return out;
        }
    }

    // 档2：单级章号，仅 Heading + 号后紧跟汉字 + 号 1~99。
    if (is_heading) {
        static const std::regex single(R"(^(\d{1,2})([^\d.].*)$)");
        if (std::regex_match(s, m, single)) {
            std::string no = m[1].str();
            std::string rest = ltrim(m[2].str());
            if (no != "0" && starts_with_cjk(rest)) {
                out.matched = true; out.clause_no = no; out.rest = rest;
                return out;
            }
        }
    }
    return out;
}
```

- [ ] **Step 4: 登记到工程文件**

在 `rag2.0\rag2.0.vcxproj` 的 `..\src\parse\ppstructure_backend.cpp` 行旁加：
```xml
    <ClCompile Include="..\src\parse\clause_no.cpp" />
```
其 `<ClInclude>` 段加：
```xml
    <ClInclude Include="..\src\parse\clause_no.h" />
```
在 `rag2.0\rag2.0.vcxproj.filters` 加：
```xml
    <ClCompile Include="..\src\parse\clause_no.cpp"><Filter>源文件\parse</Filter></ClCompile>
    <ClInclude Include="..\src\parse\clause_no.h"><Filter>头文件\parse</Filter></ClInclude>
```
在 `rag2.0.tests\rag2.0.tests.vcxproj`：测试段加 `<ClCompile Include="..\tests\test_clause_no.cpp" />`；被测源码段加 `<ClCompile Include="..\src\parse\clause_no.cpp" />`；头文件段加 `<ClInclude Include="..\src\parse\clause_no.h" />`。
在 `rag2.0.tests\rag2.0.tests.vcxproj.filters`：
```xml
    <ClCompile Include="..\tests\test_clause_no.cpp"><Filter>测试</Filter></ClCompile>
    <ClCompile Include="..\src\parse\clause_no.cpp"><Filter>被测源码</Filter></ClCompile>
    <ClInclude Include="..\src\parse\clause_no.h"><Filter>头文件</Filter></ClInclude>
```

- [ ] **Step 5: 构建并跑测试**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="parse_clause_no*"
```
Expected: 3 个 parse_clause_no 用例全 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/parse/clause_no.h src/parse/clause_no.cpp tests/test_clause_no.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2b): add parse_clause_no shared two-tier clause-number grammar"
```

---

## Task 3: caption 过滤 + clause_no 应用（ocr_normalize 第一部分）

**Files:**
- Create: `src/parse/ocr_normalize.h`, `src/parse/ocr_normalize.cpp`
- Test: `tests/test_ocr_normalize.cpp`
- Register: 同 Task 2 的 4+2 处（文件名换 `ocr_normalize`、test 换 `test_ocr_normalize`）

- [ ] **Step 1: 写失败测试 `tests/test_ocr_normalize.cpp`（先只测 caption + clause_no）**

```cpp
#include <doctest/doctest.h>
#include "parse/ocr_normalize.h"

TEST_CASE("is_caption_label: 靠 raw_label 或前缀判图表题") {
    CHECK(is_caption_label("table_title", "表4.0.1公路技术状况等级划分标准"));
    CHECK(is_caption_label("figure_title", "图3.0.3指标体系"));
    CHECK(is_caption_label("", "表 A-1路基损坏调查表"));     // 前缀兜底
    CHECK(is_caption_label("", "续表7.5.1"));                 // 前缀兜底
    CHECK_FALSE(is_caption_label("paragraph_title", "5.2.1龟裂应按面积计算"));
}

TEST_CASE("apply_clause_extraction: 给非 caption 元素抠号、给 caption 打标") {
    std::vector<ParseElement> els(3);
    els[0].type = ElementType::Heading; els[0].raw_label = "paragraph_title";
    els[0].title = "5.2.1龟裂应按面积计算"; els[0].source = "ppstructure";
    els[1].type = ElementType::Heading; els[1].raw_label = "table_title";
    els[1].title = "表4.0.1公路技术状况等级划分标准"; els[1].source = "ppstructure";
    els[2].type = ElementType::Heading; els[2].raw_label = "paragraph_title";
    els[2].title = "1总则"; els[2].source = "ppstructure";

    apply_clause_extraction(els);

    CHECK(els[0].clause_no == "5.2.1");
    CHECK(els[0].is_caption == false);
    CHECK(els[1].is_caption == true);
    CHECK(els[1].clause_no == "");      // caption 不抠号
    CHECK(els[2].clause_no == "1");     // 单级章号
}
```

- [ ] **Step 2: 写头文件 `src/parse/ocr_normalize.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "parse/parser.h"

// 是否图/表题：raw_label 命中 table_title/figure_title/chart_title，或标题以 图/表/续表/附图/附表 起。
bool is_caption_label(const std::string& raw_label, const std::string& title);

// 整篇几乎全英文且不成词（OCR 糊块）。
bool is_english_garble(const std::string& text);

// 对元素流做：caption 标记 + clause_no 抠取（caption 不抠）。就地修改。
void apply_clause_extraction(std::vector<ParseElement>& els);

// 顺序扫元素流，按 目次/附录/条文说明 标题与首章检测打 region 标签。就地修改。
void tag_regions(std::vector<ParseElement>& els);

// 抠号后：连续性(seq) 与正文过短(short) 检查，写 .suspect。就地修改。
void flag_anomalies(std::vector<ParseElement>& els);

// M2b 总入口：对 doc.elements 依次跑 上面四步 + 丢弃独立英文糊块。就地修改 doc。
void normalize_parsed_doc(ParsedDoc& doc);
```

- [ ] **Step 3: 写实现 `src/parse/ocr_normalize.cpp`（本任务只实现 is_caption_label + apply_clause_extraction，其余留桩）**

```cpp
#include "parse/ocr_normalize.h"
#include "parse/clause_no.h"

// UTF-8 字节前缀匹配（汉字 图/表/续/附 的 UTF-8 编码）。
static bool starts_with(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

bool is_caption_label(const std::string& raw_label, const std::string& title) {
    if (raw_label == "table_title" || raw_label == "figure_title" || raw_label == "chart_title")
        return true;
    // 前缀兜底：图 / 表 / 续表 / 附图 / 附表（允许前导空白）
    size_t a = title.find_first_not_of(" \t\r");
    std::string t = (a == std::string::npos) ? std::string() : title.substr(a);
    static const char* prefixes[] = {"图", "表", "续表", "附图", "附表"};
    for (auto p : prefixes) if (starts_with(t, p)) return true;
    return false;
}

bool is_english_garble(const std::string& text) {
    if (text.empty()) return false;
    size_t ascii = 0, total = 0;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        ++total;
        if (c < 0x80) ++ascii;
    }
    return total > 0 && (double)ascii / total > 0.80;
}

void apply_clause_extraction(std::vector<ParseElement>& els) {
    for (auto& e : els) {
        // 取这个元素的"号源文本"：Heading 用 title，否则用 text。
        std::string& src = e.title.empty() ? e.text : e.title;
        if (is_caption_label(e.raw_label, src)) {
            e.is_caption = true;
            continue;                 // caption 不抠号
        }
        bool is_heading = (e.type == ElementType::Heading);
        auto r = parse_clause_no(src, is_heading);
        if (r.matched) {
            e.clause_no = r.clause_no;
            // 把剩余文本回填到 text（保留原 title 作展示），便于下游统一读 text。
            if (e.text.empty() || e.type == ElementType::Heading) e.text = r.rest;
        }
    }
}

// 以下三个本任务留桩，后续任务实现。
void tag_regions(std::vector<ParseElement>&) {}
void flag_anomalies(std::vector<ParseElement>&) {}
void normalize_parsed_doc(ParsedDoc& doc) { apply_clause_extraction(doc.elements); }
```

- [ ] **Step 4: 登记工程文件**（同 Task 2 格式，文件名 `ocr_normalize`/`test_ocr_normalize`；filters 用同样 `源文件\parse`/`头文件\parse`/`测试`/`被测源码`/`头文件`）。注意测试工程还需把 `..\src\parse\clause_no.cpp` 之外再加 `..\src\parse\ocr_normalize.cpp`。

- [ ] **Step 5: 构建并跑测试**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="is_caption_label*,apply_clause_extraction*"
```
Expected: 2 个用例 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/parse/ocr_normalize.h src/parse/ocr_normalize.cpp tests/test_ocr_normalize.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2b): caption filtering + clause_no application (ocr_normalize part 1)"
```

---

## Task 4: region 状态机

**Files:**
- Modify: `src/parse/ocr_normalize.cpp`（实现 `tag_regions`）
- Test: `tests/test_ocr_normalize.cpp`（追加用例）

- [ ] **Step 1: 追加失败测试**

在 `tests/test_ocr_normalize.cpp` 末尾追加：

```cpp
TEST_CASE("tag_regions: 按 目次/首章/附录/条文说明 切区域") {
    auto H = [](const std::string& t){ ParseElement e; e.type=ElementType::Heading; e.title=t; e.source="ppstructure"; return e; };
    std::vector<ParseElement> els = {
        H("公路技术状况评定标准"),   // front_matter
        H("目次"),                   // -> toc
        H("5.2 沥青路面 …… 13"),     // toc 内
        H("1总则"),                  // -> body（首章）
        H("5.2.1龟裂"),              // body
        H("附录A 调查表"),           // -> appendix
        H("条文说明"),               // -> explanation
        H("3.2 本规程"),             // explanation 内
    };
    // tag_regions 假设 clause_no 已抠（用于首章判定）
    apply_clause_extraction(els);
    tag_regions(els);

    CHECK(els[0].region == Region::FrontMatter);
    CHECK(els[1].region == Region::Toc);
    CHECK(els[2].region == Region::Toc);
    CHECK(els[3].region == Region::Body);
    CHECK(els[4].region == Region::Body);
    CHECK(els[5].region == Region::Appendix);
    CHECK(els[6].region == Region::Explanation);
    CHECK(els[7].region == Region::Explanation);
}
```

- [ ] **Step 2: 实现 `tag_regions`（替换 Task 3 的桩）**

```cpp
void tag_regions(std::vector<ParseElement>& els) {
    Region cur = Region::FrontMatter;
    bool body_started = false;
    auto title_of = [](const ParseElement& e){ return e.title.empty() ? e.text : e.title; };
    auto starts = [](const std::string& s, const std::string& p){
        size_t a = s.find_first_not_of(" \t\r");
        std::string t = (a==std::string::npos)? std::string(): s.substr(a);
        return t.size() >= p.size() && t.compare(0, p.size(), p) == 0;
    };
    for (auto& e : els) {
        std::string t = title_of(e);
        // 区域标题切换（优先级：explanation/appendix/toc 标题）
        if (starts(t, "条文说明")) cur = Region::Explanation;
        else if (starts(t, "附录"))  cur = Region::Appendix;
        else if (!body_started && starts(t, "目次")) cur = Region::Toc;
        else if (!body_started && cur != Region::Toc /*目次后等首章*/) { /*留在 front_matter*/ }

        // 首个合法单级章号（如 "1总则"）→ 正文开始
        if (!body_started && e.type == ElementType::Heading && !e.is_caption
            && !e.clause_no.empty() && e.clause_no.find('.') == std::string::npos) {
            body_started = true;
            cur = Region::Body;
        }
        e.region = cur;
    }
}
```

- [ ] **Step 3: 构建并跑测试**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="tag_regions*"
```
Expected: PASS。

- [ ] **Step 4: Commit**

```bash
git add src/parse/ocr_normalize.cpp tests/test_ocr_normalize.cpp
git commit -m "feat(m2b): region state machine (front_matter/toc/body/appendix/explanation)"
```

---

## Task 5: 英文糊丢弃 + 异常标记 + normalize 总入口

**Files:**
- Modify: `src/parse/ocr_normalize.cpp`（实现 `flag_anomalies` + 完善 `normalize_parsed_doc`）
- Test: `tests/test_ocr_normalize.cpp`（追加用例）

- [ ] **Step 1: 追加失败测试**

```cpp
TEST_CASE("flag_anomalies: 跳号标 seq、正文过短标 short") {
    auto C = [](const std::string& no, const std::string& txt){
        ParseElement e; e.type=ElementType::Heading; e.clause_no=no; e.text=txt;
        e.region=Region::Body; e.source="ppstructure"; return e; };
    std::vector<ParseElement> els = {
        C("7.1","一般规定"), C("7.2","评定方法说明充分"),
        C("7.33","路基技术状况评定"),   // 7.2 后跳到 7.33 -> seq
        C("7.4.9","算："),              // 正文过短 -> short
    };
    flag_anomalies(els);
    CHECK(els[2].suspect == "seq");
    CHECK(els[3].suspect == "short");
    CHECK(els[0].suspect == "");
}

TEST_CASE("normalize_parsed_doc: 丢弃独立英文糊块、跑全链") {
    ParsedDoc d;
    auto add = [&](ElementType ty, const std::string& label, const std::string& title){
        ParseElement e; e.type=ty; e.raw_label=label; e.title=title; e.source="ppstructure";
        d.elements.push_back(e); };
    add(ElementType::Heading, "doc_title", "sessmentSta");        // 英文糊 -> 丢
    add(ElementType::Heading, "paragraph_title", "1总则");
    add(ElementType::Heading, "paragraph_title", "5.2.1龟裂应按面积计算");
    add(ElementType::Heading, "table_title", "表4.0.1等级划分");   // caption

    normalize_parsed_doc(d);

    REQUIRE(d.elements.size() == 3);                  // 英文糊被丢
    CHECK(d.elements[0].clause_no == "1");
    CHECK(d.elements[1].clause_no == "5.2.1");
    CHECK(d.elements[2].is_caption == true);
}
```

- [ ] **Step 2: 实现 `flag_anomalies` 与完善 `normalize_parsed_doc`**

替换 Task 3 的两个桩：

```cpp
#include <vector>
#include <string>

// 把 "7.2"/"7.33"/"5.2.1" 拆成整数段，便于比较顺序。
static std::vector<int> split_no(const std::string& no) {
    std::vector<int> v; std::string cur;
    for (char c : no) {
        if (c >= '0' && c <= '9') cur += c;
        else if (c == '.') { if(!cur.empty()){v.push_back(std::stoi(cur));cur.clear();} else v.push_back(0); }
        else break;   // 遇 '-' 后缀停止
    }
    if (!cur.empty()) v.push_back(std::stoi(cur));
    return v;
}

void flag_anomalies(std::vector<ParseElement>& els) {
    std::vector<int> prev;
    for (auto& e : els) {
        if (e.region != Region::Body || e.clause_no.empty() || e.is_caption) continue;
        // 正文过短（抠号后 text < 9 字节 ≈ 不足 3 个汉字，如 "算："=6 字节）
        if (e.text.size() < 9) { e.suspect = "short"; }
        // 连续性：同级（段数相同）下末段应递增 1，跳变 >1 标 seq
        auto cur = split_no(e.clause_no);
        if (!prev.empty() && cur.size() == prev.size()) {
            bool same_parent = std::equal(cur.begin(), cur.end()-1, prev.begin());
            if (same_parent && cur.back() - prev.back() > 1 && e.suspect.empty())
                e.suspect = "seq";
        }
        prev = cur;
    }
}

void normalize_parsed_doc(ParsedDoc& doc) {
    auto& els = doc.elements;
    // 1) 丢弃独立英文糊块（仅对 ppstructure 源、无 clause 线索的标题/正文）
    els.erase(std::remove_if(els.begin(), els.end(), [](const ParseElement& e){
        return e.source == "ppstructure" && is_english_garble(e.title.empty()? e.text : e.title)
               && e.table_html.empty();
    }), els.end());
    // 2) caption + clause_no
    apply_clause_extraction(els);
    // 3) region
    tag_regions(els);
    // 4) 异常标记
    flag_anomalies(els);
    doc.schema_version = 2;
}
```

并确保文件顶部已 `#include <algorithm>`（`std::remove_if`/`std::equal`）。

- [ ] **Step 3: 构建并跑测试**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="flag_anomalies*,normalize_parsed_doc*"
```
Expected: PASS。

- [ ] **Step 4: Commit**

```bash
git add src/parse/ocr_normalize.cpp tests/test_ocr_normalize.cpp
git commit -m "feat(m2b): english-garble drop + anomaly flags + normalize_parsed_doc entry"
```

---

## Task 6: 串接管道（raw_label 反序列化 + 缓存新字段 + hybrid 调 normalize）

**Files:**
- Modify: `src/parse/ppstructure_backend.cpp`（读 `raw_label`）
- Modify: `src/parse/parse_cache.cpp`（序列化/反序列化新字段）
- Modify: `src/parse/hybrid_parser.cpp`（`parse()` 末尾调 `normalize_parsed_doc`）
- Test: `tests/test_parse_cache_m2b.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_parse_cache_m2b.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/parse_cache.h"

TEST_CASE("parse_cache 往返保留 M2b 新字段") {
    ParsedDoc d;
    d.schema_version = 2;
    ParseElement e;
    e.type = ElementType::Heading; e.page_no = 13; e.clause_no = "5.2.1";
    e.text = "龟裂应按面积计算"; e.raw_label = "paragraph_title";
    e.region = Region::Body; e.is_caption = false; e.suspect = "short";
    e.source = "ppstructure"; e.ocr_confidence = 0.87f;
    d.elements.push_back(e);

    std::string js = parsed_doc_to_json(d);
    ParsedDoc back = parsed_doc_from_json(js);

    REQUIRE(back.elements.size() == 1);
    CHECK(back.schema_version == 2);
    CHECK(back.elements[0].raw_label == "paragraph_title");
    CHECK(back.elements[0].region == Region::Body);
    CHECK(back.elements[0].suspect == "short");
    CHECK(back.elements[0].ocr_confidence == doctest::Approx(0.87f));
}

TEST_CASE("parse_cache 容忍旧缓存（缺 M2b 字段）") {
    std::string old_js = R"({"elements":[{"type":"Text","page_no":1,"text":"x","source":"poppler"}]})";
    ParsedDoc back = parsed_doc_from_json(old_js);
    REQUIRE(back.elements.size() == 1);
    CHECK(back.elements[0].region == Region::Body);   // 默认值
    CHECK(back.elements[0].raw_label == "");
}
```

- [ ] **Step 2: `ppstructure_backend.cpp` 读 raw_label**

在 `parse_ppstructure_json` 的字段读取中（`pe.ocr_confidence = ...` 行后）加：
```cpp
        pe.raw_label = e.value("raw_label", "");
```

- [ ] **Step 3: `parse_cache.cpp` 序列化新字段**

在 `parsed_doc_to_json` 的 `j["elements"].push_back({...})` 元素对象里追加键：
```cpp
            {"raw_label", e.raw_label}, {"region", region_to_string(e.region)},
            {"is_caption", e.is_caption}, {"suspect", e.suspect}
```
并在函数顶部 `j["standard_no"] = ...` 附近加：
```cpp
    j["schema_version"] = doc.schema_version;
```

在 `parsed_doc_from_json`：函数内 `d.standard_no = ...` 附近加：
```cpp
    d.schema_version = j.value("schema_version", 1);
```
元素循环里（`pe.ocr_confidence = ...` 后）加：
```cpp
            pe.raw_label = e.value("raw_label", "");
            pe.region = region_from_string(e.value("region", "body"));
            pe.is_caption = e.value("is_caption", false);
            pe.suspect = e.value("suspect", "");
```

- [ ] **Step 4: `hybrid_parser.cpp` 在 parse 末尾归一化**

`#include "parse/ocr_normalize.h"` 加到文件顶部；把 `HybridParser::parse` 的结尾：
```cpp
    return merge_doc(base, ocr_pages, ocr_els);
```
改为：
```cpp
    ParsedDoc doc = merge_doc(base, ocr_pages, ocr_els);
    normalize_parsed_doc(doc);
    return doc;
```
并把 `ocr_normalize.cpp` 也加入**应用工程**的编译（Task 3 已登记，确认在 `rag2.0.vcxproj` 中）。

- [ ] **Step 5: 登记测试文件**

`rag2.0.tests.vcxproj` 测试段加 `<ClCompile Include="..\tests\test_parse_cache_m2b.cpp" />`；filters 加 `<ClCompile Include="..\tests\test_parse_cache_m2b.cpp"><Filter>测试</Filter></ClCompile>`。（`parse_cache.cpp` 应已在测试工程；若无则一并加 `..\src\parse\parse_cache.cpp` 与 `ocr_normalize.cpp`/`clause_no.cpp` 依赖。）

- [ ] **Step 6: 构建并跑测试（含既有回归）**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 全部用例 PASS（含 M2a 既有 40 用例不劣化）。

- [ ] **Step 7: Commit**

```bash
git add src/parse/ppstructure_backend.cpp src/parse/parse_cache.cpp src/parse/hybrid_parser.cpp tests/test_parse_cache_m2b.cpp rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2b): wire normalize into HybridParser + persist raw_label/region/suspect in cache"
```

---

## Task 7: OCR 体检指标

**Files:**
- Create: `src/parse/ocr_metrics.h`, `src/parse/ocr_metrics.cpp`
- Test: `tests/test_ocr_metrics.cpp`
- Register: 4+2 处（`ocr_metrics` / `test_ocr_metrics`）

- [ ] **Step 1: 写失败测试 `tests/test_ocr_metrics.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/ocr_metrics.h"

TEST_CASE("compute_ocr_metrics: 基本计数与填充率") {
    ParsedDoc d;
    auto mk = [&](Region r, ElementType ty, const std::string& title,
                  const std::string& no, bool cap, const std::string& sus, float conf){
        ParseElement e; e.region=r; e.type=ty; e.title=title; e.clause_no=no;
        e.is_caption=cap; e.suspect=sus; e.ocr_confidence=conf; e.source="ppstructure";
        d.elements.push_back(e); };
    // 2 个 body 候选条款标题，1 个抠到号、1 个没抠到 -> 填充率 50%
    mk(Region::Body, ElementType::Heading, "5.2.1龟裂", "5.2.1", false, "", 0.9f);
    mk(Region::Body, ElementType::Heading, "7总则坏行", "",      false, "", 0.8f);
    // caption 泄漏：title 以"表"起却带了 clause_no
    mk(Region::Body, ElementType::Heading, "表4.0.1划分", "4.0.1", false, "", 0.95f);
    // 一个 suspect
    mk(Region::Body, ElementType::Heading, "7.4.9算：", "7.4.9", false, "short", 0.7f);

    OcrMetrics m = compute_ocr_metrics(d);
    CHECK(m.body_candidate == 3);    // 3 个"数字开头"的 body 标题（"表4.0.1"以表起，不算候选）
    CHECK(m.body_filled == 2);
    CHECK(m.caption_leak == 1);
    CHECK(m.suspect == 1);
    CHECK(m.all_conf_one == false);
}
```

- [ ] **Step 2: 写头文件 `src/parse/ocr_metrics.h`**

```cpp
#pragma once
#include <string>
#include "parse/parser.h"

struct OcrMetrics {
    int body_candidate = 0;   // region=body & Heading & 标题以数字起（应是条款的候选）
    int body_filled = 0;      // 其中成功抠到 clause_no 的
    int caption_leak = 0;     // 标题以 图/表 起 但 clause_no 非空 且 !is_caption
    int suspect = 0;          // body & suspect != ""
    double conf_min = 1.0, conf_mean = 1.0, conf_max = 1.0;
    bool all_conf_one = true; // ppstructure 元素置信度是否全为 1.0（疑似写死）
    int toc_count = 0;        // region=toc 元素数（信息项）
};

OcrMetrics compute_ocr_metrics(const ParsedDoc& doc);
std::string format_ocr_metrics(const OcrMetrics& m);   // 多行可读体检表
```

- [ ] **Step 3: 写实现 `src/parse/ocr_metrics.cpp`**

```cpp
#include "parse/ocr_metrics.h"
#include <sstream>

static bool starts_digit(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    return a != std::string::npos && s[a] >= '0' && s[a] <= '9';
}
static bool starts_fig_table(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    std::string t = (a==std::string::npos)? std::string(): s.substr(a);
    return t.compare(0, std::string("图").size(), "图") == 0
        || t.compare(0, std::string("表").size(), "表") == 0;
}

OcrMetrics compute_ocr_metrics(const ParsedDoc& doc) {
    OcrMetrics m;
    double cmin = 2.0, cmax = -1.0, csum = 0.0; int cn = 0;
    for (const auto& e : doc.elements) {
        const std::string& t = e.title.empty() ? e.text : e.title;
        if (e.region == Region::Toc) ++m.toc_count;
        if (e.source == "ppstructure") {
            ++cn; csum += e.ocr_confidence;
            if (e.ocr_confidence < cmin) cmin = e.ocr_confidence;
            if (e.ocr_confidence > cmax) cmax = e.ocr_confidence;
            if (e.ocr_confidence != 1.0f) m.all_conf_one = false;
        }
        if (e.region == Region::Body && e.type == ElementType::Heading && starts_digit(t)) {
            ++m.body_candidate;
            if (!e.clause_no.empty()) ++m.body_filled;
        }
        if (!e.is_caption && !e.clause_no.empty() && starts_fig_table(t)) ++m.caption_leak;
        if (e.region == Region::Body && !e.suspect.empty()) ++m.suspect;
    }
    if (cn > 0) { m.conf_min = cmin; m.conf_max = cmax; m.conf_mean = csum / cn; }
    return m;
}

std::string format_ocr_metrics(const OcrMetrics& m) {
    std::ostringstream os;
    double fill = m.body_candidate ? 100.0 * m.body_filled / m.body_candidate : 0.0;
    os << "==== OCR 体检表 ====\n";
    os << "正文条款候选        : " << m.body_candidate << "\n";
    os << "  其中抠到号        : " << m.body_filled << "  (填充率 " << fill << "%)\n";
    os << "图表题泄漏          : " << m.caption_leak << "  (目标 0)\n";
    os << "可疑条款(seq/short) : " << m.suspect << "\n";
    os << "TOC 元素            : " << m.toc_count << "\n";
    os << "置信度 min/mean/max : " << m.conf_min << " / " << m.conf_mean << " / " << m.conf_max << "\n";
    os << "置信度全为1.0(疑写死): " << (m.all_conf_one ? "是 ⚠️" : "否") << "\n";
    return os.str();
}
```

- [ ] **Step 4: 登记工程文件**（4+2 处，`ocr_metrics`/`test_ocr_metrics`）。

- [ ] **Step 5: 构建并跑测试**

Run:
```
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="compute_ocr_metrics*"
```
Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/parse/ocr_metrics.h src/parse/ocr_metrics.cpp tests/test_ocr_metrics.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2b): OCR quality metrics (compute_ocr_metrics + format)"
```

---

## Task 8: `ocrcheck` CLI 命令

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: 加 `cmd_ocrcheck`**

在 `main.cpp` 顶部加：
```cpp
#include "parse/parse_cache.h"
#include "parse/ocr_metrics.h"
```
在 `cmd_dump` 之后加：
```cpp
// 体检命令：读一份 parse_cache JSON，打印 OCR 干不干净的指标表。
static int cmd_ocrcheck(const std::string& cache_path) {
    std::string js = read_file(cache_path);
    if (js.empty()) { spdlog::error("读不到缓存文件: {}", cache_path); return 1; }
    ParsedDoc d = parsed_doc_from_json(js);
    spdlog::info("缓存: {} | schema_version={} | pages={} elements={}",
                 cache_path, d.schema_version, d.pages.size(), d.elements.size());
    std::cout << format_ocr_metrics(compute_ocr_metrics(d)) << "\n";
    return 0;
}
```

- [ ] **Step 2: 接入 main 分发**

在 `if (cmd == "dump")` 块后加：
```cpp
    if (cmd == "ocrcheck") {
        if (argc < 3) { std::cout << "usage: rag2 ocrcheck <parse_cache.json>\n"; return 1; }
        return cmd_ocrcheck(argv[2]);
    }
```
并把 usage 行更新为：
```cpp
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck> [args]\n";
```

- [ ] **Step 3: 构建**

Run: `& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: 编译成功。

- [ ] **Step 4: 对现有 OCR 缓存跑一次（手动验证命令可用）**

Run:
```
chcp 65001
.\rag2.0\x64\Debug\rag2.0.exe ocrcheck data\parse_cache\12215131224082667446.json
```
Expected: 打印体检表。注意：**该缓存是 M2b 之前生成的（schema_version=1，全 raw_label 为空、clause_no 全空）**，所以填充率会很低、`置信度全为1.0=是`——这正是 M2b 前的基线，符合预期。真正达标要等 Task 10 重跑 OCR。

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat(m2b): add ocrcheck CLI command to print OCR quality metrics"
```

---

## Task 9: app.py 保真补丁（透传 raw_label + 真实置信度 + DPI300）

**Files:**
- Modify: `services/ppstructure/app.py`

> 该改动是"如实上报"，无法在本仓库 doctest 内单测，靠 Task 10 端到端验证。改动保持最小、不做语义判断。

- [ ] **Step 1: DPI 200 → 300**

把 `_render_page` 的默认 `dpi=200` 改为 `dpi=300`。

- [ ] **Step 2: `_to_elements` 透传 raw_label 与真实置信度**

把 `_to_elements` 整体替换为（每个 element 带 `raw_label` 原始标签 + 真实 `ocr_confidence`；不再把 title 类拍平时丢标签，也不再写死 1.0）：

```python
def _to_elements(result, page_no):
    """把 PP-StructureV3 单页结果映射为 IR 元素。
       如实上报：透传原始 block_label 到 raw_label、透传真实置信度；语义判断交 C++。"""
    res0 = result[0] if isinstance(result, (list, tuple)) and result else result
    d = getattr(res0, "json", None)
    if not isinstance(d, dict):
        return []
    d = d.get("res", d)
    blocks = d.get("parsing_res_list") or []
    out = []
    for b in blocks:
        if not isinstance(b, dict):
            continue
        raw = (b.get("block_label") or "text")
        label = raw.lower()
        content = b.get("block_content", "") or ""
        # 真实置信度：优先块级分数，缺失则置 0.0（而非伪 1.0），便于 C++ 端识别"无分数"
        conf = b.get("block_score", b.get("score", None))
        conf = float(conf) if isinstance(conf, (int, float)) else 0.0
        base = {"page_no": page_no, "raw_label": raw, "ocr_confidence": conf}
        if "table" in label:
            out.append({**base, "type": "Table", "table_html": content, "caption": "", "text": ""})
        elif "formula" in label:
            out.append({**base, "type": "Formula", "text": content})
        elif "title" in label:     # doc_title/paragraph_title/table_title/figure_title...
            out.append({**base, "type": "Heading", "level": 1, "title": content, "text": content})
        else:
            if content.strip():
                out.append({**base, "type": "Text", "text": content})
    return out
```

> 注：`block_score`/`score` 的真实键名以 PP-StructureV3 输出为准；Task 10 首跑时若发现键名不同，按实际改（守卫已保证缺失时为 0.0，不崩）。

- [ ] **Step 3: 重启服务自检**

Run（OCR 服务窗口）：
```
cd "D:\vs2022 code\rag2.0\services\ppstructure"
uv run --no-sync uvicorn app:app --host 0.0.0.0 --port 8001
```
另开窗口验证 health：`curl http://localhost:8001/health` → `{"status":"ok","engine":"v3"}`。

- [ ] **Step 4: Commit**

```bash
git add services/ppstructure/app.py
git commit -m "feat(m2b): app.py faithful passthrough — raw_label + real confidence + DPI 300"
```

---

## Task 10: 端到端验证 + 标定目标线（手动，需 GPU 服务 + 真实 PDF）

**Files:** 无代码改动（仅可能回填阈值/标定记录）。

- [ ] **Step 1: 删旧缓存，重跑扫描件 OCR ingest**

OCR 服务保持开着（Task 9 Step 3）。另一窗口（项目根）：
```
chcp 65001
del data\parse_cache\12215131224082667446.json
# 把 config.json 的 RAG_DOC_PATH 指向 JTC 5210-2018 扫描件，RAG_PARSE_MODE=auto
.\rag2.0\x64\Debug\rag2.0.exe ingest
```
Expected: 入库完成；`data\parse_cache\<新id>.json` 生成，`schema_version=2`。

- [ ] **Step 2: 跑体检表**

```
.\rag2.0\x64\Debug\rag2.0.exe ocrcheck data\parse_cache\<新id>.json
```
Expected（相对 M2b 前基线应明显改善）：
- `置信度全为1.0(疑写死)` = 否；min/mean/max 出现 <1.0 的真实分布。
- `图表题泄漏` = 0。
- `正文条款候选` 的填充率显著高于改造前（改造前几乎 0）。

- [ ] **Step 3: 人眼抽查可疑标记**

用编辑器或 `python -c` 打印新缓存里 `suspect != ""` 的元素，确认确为 OCR 错号/截断（如 `7.33`、`7.4.9算：`），而非误标正常条款。

- [ ] **Step 4: 标定"够用线"并记录**

依据 Step 2/3 的实测基线，在 spec §9 表的"建议目标线"旁用实测数字确认或修订（填充率、可疑占比阈值）。把实测结果与最终目标线追加记录到 `docs/superpowers/2026-06-02-m2a-progress-and-handoff.md` 的进度小节（或新建 M2b 交接小节）。

- [ ] **Step 5: 文字版回归（确保 poppler 路没被破坏）**

把 `RAG_DOC_PATH` 指回 JTG 3432-2024（文字版），`del` 其缓存后 `ingest`，再 `ocrcheck` 该缓存：确认 poppler 页正常、`source=poppler`、既有 `query` 仍可回答（抽一条之前能答的问题）。

- [ ] **Step 6: 全量单测回归 + 提交标定记录**

```
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 全绿（M2a 40 + M2b 新增用例）。

```bash
git add docs/superpowers/2026-06-02-m2a-progress-and-handoff.md docs/superpowers/specs/2026-06-03-m2b-ocr-quality-design.md
git commit -m "docs(m2b): record measured OCR-quality baseline and finalized acceptance thresholds"
```

---

## 完成判据（对照 spec §9 验收）

- [ ] app.py 透传 `raw_label` + 真实置信度，DPI=300；`ocrcheck` 显示置信度非全 1.0。
- [ ] `parse_clause_no` doctest 覆盖全部真实样本（含 `200kN`/`0.5%`/`5.2%`/单级严格负例）通过。
- [ ] JTC 5210-2018 重跑后体检表：图表题泄漏=0、填充率较基线显著提升、置信度真实。
- [ ] 缓存新增 `raw_label/region/is_caption/suspect/schema_version`，旧缓存容忍式读取；M1 `query` 回归不劣化。
- [ ] 全量 doctest 全绿。
