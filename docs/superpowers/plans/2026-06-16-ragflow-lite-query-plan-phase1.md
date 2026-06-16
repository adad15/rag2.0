# RAGFlow-lite 查询计划 Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `analyze_query` 升级成"查询计划"：对纯列举类问题（含"哪些"等列举词、且命中已知仪器实体）生成专用的 `sparse_text`/`dense_text` 分别喂给 BM25 与 dense，修复"哪些试验用到天平"召回失败；其余意图回退原句、行为不变。

**Architecture:** 新增 `query_terms` 词表模块（四类词，提取式而非删除式，规避"试验筛"被切坏）；扩展 `QueryAnalysis` 加 `intent/key_terms/section_hints/sparse_text/dense_text`，新增纯函数 `build_query_plan`；`text_retrieve` 改用 `dense_text`/`sparse_text`（空则回退 `clean_text`）。**只有 `ListByCondition` 且命中仪器白名单的查询被改写，其它一律回退原句 → 普通问答零回归。** 不改 Milvus schema、不接 LLM。

**Tech Stack:** C++20、MSBuild + vcpkg、doctest、nlohmann-json、spdlog、Milvus REST（BM25）。

**Scope（本计划 = 方案 B 第 1 刀）：** 只做查询计划核心 + 接线。**不含**：weighted RRF（§6.1）、intent-aware rerank（§6.2）、列举覆盖去重（§6.3）、评测闭环（§9.3）——这些是后续 Phase 2/3 计划。本刀的验收用现有 `retrievecheck` 实测（spec §9.2），尚不引入权重调参，故不依赖评测集。

**关联：** spec [`2026-06-16-ragflow-lite-query-plan-design.md`](../specs/2026-06-16-ragflow-lite-query-plan-design.md)。分支 V3.1。

---

## File Structure

| 文件 | 职责 |
|---|---|
| `config/query_terms.txt`（新增） | 四类词表（stopword/background/section/instrument）。Phase 1 只用 section + instrument |
| `src/query/query_terms.h`（新增） | `QueryTerms` 结构、`load_query_terms`、`match_terms`（纯） |
| `src/query/query_terms.cpp`（新增） | 上述实现 |
| `src/query/query_analysis.h`（改） | 加 `QueryIntent` 枚举、查询计划字段、`build_query_plan`、`query_intent_name` |
| `src/query/query_analysis.cpp`（改） | 意图分类 + `build_query_plan` + `query_intent_name` |
| `src/retrieve/text_search.cpp`（改） | 加载词表、构建查询计划、dense/BM25 改用 `dense_text`/`sparse_text`、日志 |
| `tests/test_query_terms.cpp`（新增） | `match_terms`/`load_query_terms` 单测 |
| `tests/test_query_analysis.cpp`（改） | 意图 + `build_query_plan` 单测 |
| `rag2.0/rag2.0.vcxproj`(+`.filters`)（改） | 登记 `query_terms.cpp/.h` |
| `rag2.0.tests/rag2.0.tests.vcxproj`(+`.filters`)（改） | 登记 `query_terms.cpp` + `test_query_terms.cpp` |

**构建命令（PowerShell，项目根目录）：**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

**测试 exe：** `rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest，单测过滤用 `--test-case="名字"`）。
**应用 exe：** `rag2.0\x64\Debug\rag2.0.exe`。

---

## Task 1: query_terms 词表模块（纯函数）

**Files:**
- Create: `config/query_terms.txt`
- Create: `src/query/query_terms.h`
- Create: `src/query/query_terms.cpp`
- Test: `tests/test_query_terms.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`, `rag2.0.tests/rag2.0.tests.vcxproj`, `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [ ] **Step 1: 写 `config/query_terms.txt`**

```text
# 方案 B 查询词表。'#' 与空行忽略。[name] 起一个分节。
# Phase 1 只消费 [section] 与 [instrument]；[stopword]/[background] 预留给 Phase 2。

[stopword]
哪些
有哪些
哪几项
哪几种
什么
怎么
如何
用到
需要
包含
涉及
使用
采用

[background]
公路
工程
水泥
混凝土
试验
规程
方法
标准
砂浆

[section]
仪具
材料
设备
器具
试剂
步骤
结果

[instrument]
天平
烘箱
压力机
万能试验机
试验筛
坍落度筒
振动台
量筒
容量瓶
李氏瓶
比重瓶
游标卡尺
温度计
秒表
```

- [ ] **Step 2: 写 `src/query/query_terms.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 查询词表（方案 B §4.3）。Phase 1 只消费 instruments + sections；
// background/stopwords 预留给 Phase 2 的 GeneralFact 清洗。
struct QueryTerms {
    std::vector<std::string> stopwords;   // 句式停用词（预留）
    std::vector<std::string> background;  // 领域背景词（预留）
    std::vector<std::string> sections;    // 章节提示词
    std::vector<std::string> instruments; // 核心条件词白名单
};

// 从分节文件加载（[stopword]/[background]/[section]/[instrument] 标头；
// '#' 开头与空行忽略）。文件不存在 → 返回空表（调用方回退原句）。
QueryTerms load_query_terms(const std::string& path);

// 纯函数：返回 dict 中作为 text 子串出现的词，按 dict 顺序、去重。
// 提取式（非删除式）：天然规避"试验筛"被按"试验"切成"筛"的问题。
std::vector<std::string> match_terms(const std::string& text,
                                     const std::vector<std::string>& dict);
```

- [ ] **Step 3: 写 `src/query/query_terms.cpp`**

```cpp
#include "query/query_terms.h"
#include <fstream>

std::vector<std::string> match_terms(const std::string& text,
                                     const std::vector<std::string>& dict) {
    std::vector<std::string> out;
    for (const auto& w : dict) {
        if (w.empty() || text.find(w) == std::string::npos) continue;
        bool dup = false;
        for (const auto& o : out) if (o == w) { dup = true; break; }
        if (!dup) out.push_back(w);
    }
    return out;
}

namespace {
std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

QueryTerms load_query_terms(const std::string& path) {
    QueryTerms t;
    std::ifstream f(path, std::ios::binary);
    if (!f) return t;
    std::vector<std::string>* cur = nullptr;
    std::string line;
    while (std::getline(f, line)) {
        std::string s = strip(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            std::string name = s.substr(1, s.size() - 2);
            if (name == "stopword")        cur = &t.stopwords;
            else if (name == "background") cur = &t.background;
            else if (name == "section")    cur = &t.sections;
            else if (name == "instrument") cur = &t.instruments;
            else cur = nullptr;
            continue;
        }
        if (cur) cur->push_back(s);
    }
    return t;
}
```

- [ ] **Step 4: 写 `tests/test_query_terms.cpp`**

```cpp
#include <doctest/doctest.h>
#include "query/query_terms.h"
#include <fstream>
#include <cstdio>

TEST_CASE("match_terms returns dict entries that are substrings, dict order, deduped") {
    std::vector<std::string> dict = {"天平", "烘箱", "试验筛"};
    auto out = match_terms("哪些混凝土试验用到了天平", dict);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "天平");
}

TEST_CASE("match_terms keeps a longer term intact (no substring corruption)") {
    // 提取式：'试验筛' 整体命中保留，不会被切成 '筛'
    std::vector<std::string> dict = {"试验筛"};
    auto out = match_terms("负压筛法用到试验筛", dict);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "试验筛");
}

TEST_CASE("load_query_terms parses a sectioned file") {
    const char* path = "test_query_terms_fixture.txt";
    {
        std::ofstream f(path, std::ios::binary);
        f << "[instrument]\n天平\n# comment\n烘箱\n[section]\n仪具\n";
    }
    QueryTerms t = load_query_terms(path);
    std::remove(path);
    REQUIRE(t.instruments.size() == 2);
    CHECK(t.instruments[0] == "天平");
    CHECK(t.instruments[1] == "烘箱");
    REQUIRE(t.sections.size() == 1);
    CHECK(t.sections[0] == "仪具");
}

TEST_CASE("load_query_terms returns empty on missing file") {
    QueryTerms t = load_query_terms("definitely_missing_file_xyz.txt");
    CHECK(t.instruments.empty());
    CHECK(t.sections.empty());
}
```

- [ ] **Step 5: 登记新文件到工程（否则 MSBuild 不编译）**

`rag2.0/rag2.0.vcxproj`：在 `<ClCompile Include="..\src\query\query_analysis.cpp" />` 后加一行，并在 `<ClInclude Include="..\src\query\query_analysis.h" />` 后加一行：

```xml
    <ClCompile Include="..\src\query\query_terms.cpp" />
    <ClInclude Include="..\src\query\query_terms.h" />
```

`rag2.0/rag2.0.vcxproj.filters`：在 `query_analysis` 两行后各加：

```xml
    <ClCompile Include="..\src\query\query_terms.cpp"><Filter>源文件\query</Filter></ClCompile>
    <ClInclude Include="..\src\query\query_terms.h"><Filter>头文件\query</Filter></ClInclude>
```

`rag2.0.tests/rag2.0.tests.vcxproj`：在 `<ClCompile Include="..\src\query\query_analysis.cpp" />` 与 `<ClCompile Include="..\tests\test_query_analysis.cpp" />` 附近加：

```xml
    <ClCompile Include="..\src\query\query_terms.cpp" />
    <ClCompile Include="..\tests\test_query_terms.cpp" />
```

`rag2.0.tests/rag2.0.tests.vcxproj.filters`：加：

```xml
    <ClCompile Include="..\tests\test_query_terms.cpp"><Filter>测试</Filter></ClCompile>
    <ClCompile Include="..\src\query\query_terms.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [ ] **Step 6: 构建并运行新单测，确认通过**

Run:
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*query_terms*","*match_terms*","*sectioned file*","*missing file*"
```
Expected: 编译通过；上述 TEST_CASE 全 PASS（`match_terms`/`load_query_terms` 4 例）。

- [ ] **Step 7: Commit**

```powershell
git add config/query_terms.txt src/query/query_terms.h src/query/query_terms.cpp tests/test_query_terms.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(queryplan): add query_terms wordlist module

四类查询词表 + 提取式 match_terms（规避"试验筛"被切坏）+ 分节文件加载。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: 意图分类 + build_query_plan（扩展 query_analysis）

**Files:**
- Modify: `src/query/query_analysis.h`
- Modify: `src/query/query_analysis.cpp`
- Test: `tests/test_query_analysis.cpp`

- [ ] **Step 1: 在 `tests/test_query_analysis.cpp` 末尾追加失败测试**

文件顶部 `#include "query/query_analysis.h"` 下方加一行：

```cpp
#include "query/query_terms.h"
```

文件末尾追加：

```cpp
TEST_CASE("analyze_query classifies a list-by-condition question") {
    QueryAnalysis a = analyze_query("公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平");
    CHECK(a.intent == QueryIntent::ListByCondition);
    CHECK(a.standard_code.empty());
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
}

TEST_CASE("method number outranks list marker for intent") {
    QueryAnalysis a = analyze_query("T0302 需要哪些仪具");
    CHECK(a.intent == QueryIntent::MethodLookup);
}

TEST_CASE("plain question is GeneralFact") {
    QueryAnalysis a = analyze_query("路基沉降怎么评定");
    CHECK(a.intent == QueryIntent::GeneralFact);
}

TEST_CASE("build_query_plan rewrites a list query to key terms + injected section hints") {
    QueryTerms terms;
    terms.instruments = {"天平"};
    // sections 留空 → 触发默认章节注入
    QueryAnalysis a = build_query_plan(
        "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平", terms);
    CHECK(a.intent == QueryIntent::ListByCondition);
    REQUIRE(a.key_terms.size() == 1);
    CHECK(a.key_terms[0] == "天平");
    REQUIRE(a.section_hints.size() == 2);
    CHECK(a.section_hints[0] == "仪具");
    CHECK(a.section_hints[1] == "材料");
    CHECK(a.sparse_text == "天平 仪具 材料");
    CHECK(a.dense_text == "查找试验方法中仪具和材料包含天平的段落");
}

TEST_CASE("build_query_plan leaves non-list queries on clean_text fallback") {
    QueryTerms terms; terms.instruments = {"天平"};
    QueryAnalysis a = build_query_plan("路基沉降怎么评定", terms);
    CHECK(a.intent == QueryIntent::GeneralFact);
    CHECK(a.sparse_text.empty());
    CHECK(a.dense_text.empty());
}

TEST_CASE("build_query_plan does not rewrite when no instrument matched") {
    QueryTerms terms; terms.instruments = {"天平"};
    QueryAnalysis a = build_query_plan("哪些试验需要养护", terms);
    CHECK(a.intent == QueryIntent::ListByCondition);
    CHECK(a.key_terms.empty());
    CHECK(a.sparse_text.empty());   // 回退
    CHECK(a.dense_text.empty());
}
```

- [ ] **Step 2: 改 `src/query/query_analysis.h`（加枚举/字段/声明）**

整文件替换为：

```cpp
#pragma once
#include <string>
#include <vector>
#include "query/query_terms.h"

enum class QueryIntent { GeneralFact, ClauseLookup, MethodLookup, ListByCondition };

struct QueryAnalysis {
    std::string clean_text;    // 原始问题（dense 回退输入，不抠编号）
    std::string standard_code; // 归一化裸代号，如 "JTC5210-2018"（空=未指定）
    std::string clause_no;     // 归一化条款号，如 "5.1.2"（空=未指定）
    std::string method_no;     // 归一化方法号，如 "T0302"（空=未指定）

    // 方案 B 查询计划（Phase 1）：
    QueryIntent intent = QueryIntent::GeneralFact;
    std::vector<std::string> key_terms;     // 命中的核心条件词
    std::vector<std::string> section_hints; // 章节提示（含默认注入）
    std::string sparse_text;   // BM25 输入；空 = 调用方回退 clean_text
    std::string dense_text;    // dense 输入；空 = 调用方回退 clean_text
};

// 查询理解（纯函数）：抽取标准号/条款号/方法号并归一化，并判定 intent。
QueryAnalysis analyze_query(const std::string& question);

// 在 analyze_query 基础上补全查询计划（key_terms/section_hints/sparse_text/dense_text）。
// 纯函数。仅 ListByCondition 且命中仪器白名单时生成改写文本，否则留空（回退）。
QueryAnalysis build_query_plan(const std::string& question, const QueryTerms& terms);

// 意图名（日志/调试用）。
const char* query_intent_name(QueryIntent intent);
```

- [ ] **Step 3: 改 `src/query/query_analysis.cpp`**

在文件顶部的匿名 `namespace { ... }` 内、`strip_spaces` 之后，加入这些 helper：

```cpp
bool contains_any(const std::string& text, const std::vector<std::string>& words) {
    for (const auto& w : words)
        if (!w.empty() && text.find(w) != std::string::npos) return true;
    return false;
}

// 明确列举信号（硬编码，不依赖词表）。
const std::vector<std::string>& list_markers() {
    static const std::vector<std::string> m = {"哪些", "有哪些", "哪几项", "哪几种"};
    return m;
}

QueryIntent classify_intent(const QueryAnalysis& a, const std::string& question) {
    if (!a.clause_no.empty())  return QueryIntent::ClauseLookup;
    if (!a.method_no.empty())  return QueryIntent::MethodLookup;
    if (contains_any(question, list_markers())) return QueryIntent::ListByCondition;
    return QueryIntent::GeneralFact;
}

std::string join_terms(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}
```

在现有 `analyze_query` 的 `return a;` 之前，加一行：

```cpp
    a.intent = classify_intent(a, question);
```

在 `analyze_query` 函数结束的 `}` 之后，追加两个新函数：

```cpp
QueryAnalysis build_query_plan(const std::string& question, const QueryTerms& terms) {
    QueryAnalysis a = analyze_query(question);
    a.key_terms     = match_terms(question, terms.instruments);
    a.section_hints = match_terms(question, terms.sections);

    // 仅改写"列举 + 命中仪器"的查询；其它一律留空 → 调用方回退 clean_text。
    if (a.intent == QueryIntent::ListByCondition && !a.key_terms.empty()) {
        if (a.section_hints.empty())
            a.section_hints = {"仪具", "材料"};   // 仪器类列举的默认章节

        std::vector<std::string> bag = a.key_terms;
        bag.insert(bag.end(), a.section_hints.begin(), a.section_hints.end());
        a.sparse_text = join_terms(bag, " ");
        a.dense_text  = "查找试验方法中" + join_terms(a.section_hints, "和")
                      + "包含" + join_terms(a.key_terms, "和") + "的段落";
    }
    return a;
}

const char* query_intent_name(QueryIntent intent) {
    switch (intent) {
        case QueryIntent::ClauseLookup:    return "ClauseLookup";
        case QueryIntent::MethodLookup:    return "MethodLookup";
        case QueryIntent::ListByCondition: return "ListByCondition";
        default:                           return "GeneralFact";
    }
}
```

- [ ] **Step 4: 构建并运行 query_analysis 单测，确认通过**

Run:
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*analyze_query*","*build_query_plan*","*intent*","*GeneralFact*"
```
Expected: 全 PASS——含原有 5 例（标准号/条款号/方法号/普通/单级号不误判）+ 本任务新增 6 例。

- [ ] **Step 5: 跑全量测试，确认无回归**

Run:
```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 全绿（`status=success`，0 failed）。

- [ ] **Step 6: Commit**

```powershell
git add src/query/query_analysis.h src/query/query_analysis.cpp tests/test_query_analysis.cpp
git commit -m @'
feat(queryplan): intent classification + build_query_plan

QueryAnalysis 加 intent/key_terms/section_hints/sparse_text/dense_text；
build_query_plan 仅改写"列举+命中仪器"查询，其余回退原句。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: 接入 text_retrieve + 实测验收

**Files:**
- Modify: `src/retrieve/text_search.cpp`

- [ ] **Step 1: 改 `src/retrieve/text_search.cpp` 接线**

在文件顶部 include 区加：

```cpp
#include "query/query_terms.h"
```

把：

```cpp
    QueryAnalysis qa = analyze_query(question);
```

替换为：

```cpp
    static const QueryTerms query_terms = load_query_terms("config/query_terms.txt");
    QueryAnalysis qa = build_query_plan(question, query_terms);
    spdlog::info("[queryplan] intent={} sparse=\"{}\" dense=\"{}\"",
                 query_intent_name(qa.intent), qa.sparse_text, qa.dense_text);
```

把这两行：

```cpp
    lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
    lists.push_back(bm25.retrieve(qa.clean_text, filter, per_path_k));
```

替换为：

```cpp
    const std::string& dtext = qa.dense_text.empty()  ? qa.clean_text : qa.dense_text;
    const std::string& stext = qa.sparse_text.empty() ? qa.clean_text : qa.sparse_text;
    lists.push_back(dense.retrieve(dtext, filter, per_path_k));
    lists.push_back(bm25.retrieve(stext, filter, per_path_k));
```

- [ ] **Step 2: 构建**

Run:
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected: 0 error。

- [ ] **Step 3: 确认 Milvus 在跑**

Run:
```powershell
docker ps --filter "name=milvus-standalone" --format "{{.Names}} {{.Status}}"
```
Expected: `milvus-standalone Up ... (healthy)`。若没起，先按 [[local-services]] 启动。

- [ ] **Step 4: 实测原失败样本（核心验收，spec §9.2）**

Run:
```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe retrievecheck "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平" 20 2>&1 | Out-String
```
Expected：
- 日志出现 `[queryplan] intent=ListByCondition sparse="天平 仪具 材料" dense="查找试验方法中仪具和材料包含天平的段落"`；
- top20 中出现**多个不同 method_no** 的"仪具与材料"chunk，且片段含"天平"；
- "总则/目的、适用范围和引用标准"不再占据主要 top 结果。

> 若 dense 改写模板表现不如关键词式（pollution by 指令词"查找/段落"），把 Task 2 Step 3 的 `a.dense_text` 改成 `a.dense_text = a.sparse_text;` 重测取优（spec §4.4 标注的 A/B 点）。记录所选形态。

- [ ] **Step 5: 回归实测（不应变差）**

Run:
```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
foreach ($q in @("天平","混凝土试验天平","天平 仪具","路基沉降怎么评定","T0302 需要哪些仪具")) {
  ".\n==== $q ===="
  .\rag2.0\x64\Debug\rag2.0.exe retrievecheck "$q" 8 2>&1 | Out-String
}
```
Expected：
- `天平` / `混凝土试验天平` / `天平 仪具`：仍是"仪具与材料"chunk（日志 intent=GeneralFact，sparse/dense 空 → 回退原句，行为同改前）；
- `路基沉降怎么评定`：intent=GeneralFact，回退原句，结果同改前；
- `T0302 需要哪些仪具`：intent=MethodLookup，回退原句，方法号精确路照旧。

- [ ] **Step 6: Commit**

```powershell
git add src/retrieve/text_search.cpp
git commit -m @'
feat(queryplan): wire query plan into text_retrieve

dense/BM25 改用 dense_text/sparse_text（空则回退 clean_text）；
列举类"天平"问题召回多试验"仪具与材料"chunk，普通问答零回归。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Self-Review

**1. Spec coverage（本刀 = 方案 B 查询侧核心，§6.1/§6.2/§6.3/§9.3 明确不在本刀，见 Scope）：**
- §4.1 QueryAnalysis 升级 → Task 2 Step 2。
- §4.2 意图识别（含 §4.2 触发词收紧为明确列举词）→ Task 2 Step 3 `classify_intent`/`list_markers`。
- §4.3 词类与权重（提取式、`key_terms`/`section_hints`）→ Task 1 + Task 2；`instrument` 白名单 = §4.3 推荐的"白名单兜底"，提取式规避"试验筛"切坏。
- §4.4 sparse_text/dense_text 生成（含 dense A/B 注记）→ Task 2 Step 3 + Task 3 Step 4 备注。
- §4.4 `section_hints` 注入来源 → Task 2 Step 3：ListByCondition + 命中仪器 + 无章节命中 → 注入 `仪具/材料`。
- §5 检索链路改 dense_text/sparse_text + 空回退 → Task 3 Step 1。
- §2.4 可观察 → Task 3 Step 1 `spdlog::info [queryplan]`。
- §9.1 单测（含"试验筛"反例）→ Task 1 Step 4 + Task 2 Step 1。
- §9.2 检索验收 → Task 3 Step 4/5。
- §2.5 短查询/普通问答不回归 → Task 3 Step 5（设计上仅 ListByCondition+命中仪器被改写，结构性保证零回归）。

**2. Placeholder scan：** 无 TBD/TODO/“类似 Task N”；每个代码步给出完整代码与确切命令、预期输出。

**3. Type consistency：** `QueryTerms{stopwords,background,sections,instruments}`、`match_terms`、`load_query_terms`、`QueryIntent`、`build_query_plan`、`query_intent_name`、`QueryAnalysis` 新字段名在 Task 1/2/3 与测试中一致；`text_retrieve` 签名不变（词表在内部 `static` 加载，不波及 `answer_pipeline` 等调用方）。

**4. 已知限制（转入后续计划，见 spec §13）：** 未登记仪器 → 回退原句不改善；连续词权/文档侧 enrichment/rerank 模型/同义词织入不在本刀；weighted RRF 与列举覆盖去重为 Phase 2。
