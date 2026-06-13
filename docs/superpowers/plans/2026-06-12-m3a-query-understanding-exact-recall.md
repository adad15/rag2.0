# M3a Query Understanding And Exact Recall Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the query path from single-path dense to: query understanding (extract standard/clause/method numbers) → dense path (optionally narrowed by standard) + method-number exact path → RRF fusion → clause-number pinning → existing chunk lookup + generation.

**Architecture:** Add pure-logic modules (query_analysis, retrieval_filter, rrf+pin) under TDD; evolve the `Retriever` contract to carry a `RetrievalFilter`; add a `PgExactRetriever` and three `PgClient` lookup methods; orchestrate everything in `text_search`. The Milvus collection schema is NOT touched (BM25/synonyms/status filtering are deferred to M3b). The method-number regex currently inlined in `retrieval_chunk.cpp` is extracted into a shared `src/parse/method_no` module so ingest and query produce byte-identical normalized method numbers.

**Tech Stack:** C++20, doctest, nlohmann/json, libpqxx, spdlog, Milvus REST v2, MSBuild/vcpkg, Visual Studio `.vcxproj` projects.

Spec: `docs/superpowers/specs/2026-06-12-m3a-query-understanding-exact-recall-design.md`

---

## Engineering Notes

Work in `D:\vs2022 code\rag2.0`, branch `V2.4`.

Before implementing, run:

```powershell
git status --short
```

Expected: clean except untracked `logs/`.

Build command:

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Run all tests:

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Run filtered tests:

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="<exact name>"
```

Notes:
- Source files contain Chinese string literals; the projects already compile with `/utf-8`. Save all files UTF-8 without BOM.
- Known harmless build warning `MSB4011` from vcpkg — ignore.
- `PgClient` methods need a live PostgreSQL connection; the codebase has no PG test double, so new PG methods get no unit tests (consistent with existing `pg_client.cpp`). They are exercised by the Task 8 live smoke.
- Baseline test count before this plan: **104** doctest cases. Each task states the expected count after.
- The tests project (`rag2.0.tests.vcxproj`) explicitly lists each `src` file it compiles — when a tests-compiled file (`dense_retriever.cpp`, `answer_pipeline.cpp`, `retrieval_chunk.cpp`) starts depending on a new `.cpp`, that new `.cpp` must be added to the tests project too, or linking fails. Each task calls this out.

---

## File Structure

Create:

| File | Responsibility |
|---|---|
| `src/parse/method_no.h` / `.cpp` | Shared pure method-number extractor (`extract_method_no`), used by ingest and query |
| `src/query/query_analysis.h` / `.cpp` | Pure `analyze_query`: extract standard_code / clause_no / method_no |
| `src/retrieve/retrieval_filter.h` / `.cpp` | `RetrievalFilter{standard_id}` + `to_milvus_expr` |
| `src/retrieve/rrf.h` / `.cpp` | Pure `rrf_fuse` + `pin_exact_clause` |
| `src/retrieve/pg_exact_retriever.h` / `.cpp` | Method-number exact path (Retriever) |
| `src/retrieve/text_search.h` / `.cpp` | Orchestration: understanding → 2 paths → RRF → pin |
| `tests/test_method_no.cpp` | method_no extractor cases |
| `tests/test_query_analysis.cpp` | query understanding cases |
| `tests/test_retrieval_filter.cpp` | filter expr cases |
| `tests/test_rrf.cpp` | fusion + pinning cases |

Modify:

| File | Responsibility |
|---|---|
| `src/retrieve/candidate.h` | Rename `clause_id` → `chunk_id` |
| `src/retrieve/retriever.h` | Contract gains `const RetrievalFilter&` |
| `src/retrieve/dense_retriever.h` / `.cpp` | Adapt signature + push filter to Milvus |
| `src/retrieve/retrieval_chunk.cpp` | Use shared `extract_method_no` |
| `src/milvus/milvus_rest.h` / `.cpp` | `search` + `build_search_body` gain filter expr |
| `src/db/pg_client.h` / `.cpp` | `find_standard_by_code`, `chunks_by_method`, `chunk_ids_by_clause` |
| `src/generate/answer_pipeline.h` / `.cpp` | Switch to `text_retrieve` |
| `src/main.cpp` | `cmd_query` uses new `answer_query` |
| `tests/test_milvus_body.cpp` | filter request-body case |
| `rag2.0/rag2.0.vcxproj` + `.filters` | Register new source/headers |
| `rag2.0.tests/rag2.0.tests.vcxproj` + `.filters` | Register new sources + tests |

---

## Task 1: Rename Candidate.clause_id → chunk_id

Mechanical rename done first so later new code can reference `chunk_id`.

**Files:**
- Modify: `src/retrieve/candidate.h`
- Modify: `src/retrieve/dense_retriever.cpp`
- Modify: `src/generate/answer_pipeline.cpp`

- [x] **Step 1: Confirm the only references**

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --version *> $null   # noop, ensure shell ok
```
Use Grep for `clause_id` across `src` and `tests`. Expected matches only in `candidate.h`, `dense_retriever.cpp`, `answer_pipeline.cpp`. If any other file matches, update it the same way in this task.

- [x] **Step 2: Edit `src/retrieve/candidate.h`**

Replace the struct so the field is `chunk_id`:

```cpp
#pragma once
#include <string>

// 所有召回候选统一归一到 standard_id + chunk_id，作为去重键。
struct Candidate {
    std::string standard_id;
    std::string chunk_id;    // == retrieval_chunks.chunk_id
    float score = 0.0f;
    std::string source;      // "dense" | "exact" | "dense+exact" | "exact_pin" ...
};
```

- [x] **Step 3: Edit `src/retrieve/dense_retriever.cpp`**

Change the assignment line:

```cpp
        c.chunk_id = h.chunk_id;   // M3a 起归一化键为 retrieval_chunks.chunk_id
```

- [x] **Step 4: Edit `src/generate/answer_pipeline.cpp`**

In `answer_query`, change the lookup:

```cpp
        auto chunk = pg.get_chunk(c.chunk_id);
```

- [x] **Step 5: Build and run full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, **104** tests pass (mechanical rename, no behavior change).

- [x] **Step 6: Commit**

```powershell
git add src/retrieve/candidate.h src/retrieve/dense_retriever.cpp src/generate/answer_pipeline.cpp
git commit -m @'
refactor(m3a): rename Candidate.clause_id to chunk_id

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 2: Shared method_no Module

Extract the inlined method-number functions from `retrieval_chunk.cpp` into a shared `src/parse/method_no` module, widening the regex so the year is optional (queries often omit it). Ingest behavior is preserved because the regex is greedy and still captures the year when present.

**Files:**
- Create: `src/parse/method_no.h`, `src/parse/method_no.cpp`
- Create: `tests/test_method_no.cpp`
- Modify: `src/retrieve/retrieval_chunk.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`, `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing test `tests/test_method_no.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/method_no.h"

TEST_CASE("extract_method_no keeps full method number with year") {
    CHECK(extract_method_no("T 0302-2024 集料筛分试验") == "T0302-2024");
    CHECK(extract_method_no("见 T0501-2005 水泥取样方法") == "T0501-2005");
}

TEST_CASE("extract_method_no accepts year-less codes from queries") {
    CHECK(extract_method_no("T0302 需要哪些仪具") == "T0302");
    CHECK(extract_method_no("T 0604") == "T0604");
}

TEST_CASE("extract_method_no normalizes full-width dash") {
    CHECK(extract_method_no("T0302\xE2\x80\x942024") == "T0302-2024"); // em dash
}

TEST_CASE("extract_method_no returns empty when no method number") {
    CHECK(extract_method_no("路基沉降怎么评定").empty());
    CHECK(extract_method_no("第5.1.2条").empty());
}
```

- [x] **Step 2: Add project entries**

In `rag2.0/rag2.0.vcxproj`, next to the other `src\parse` `ClCompile` entries add:

```xml
<ClCompile Include="..\src\parse\method_no.cpp" />
```

and next to the `src\parse` `ClInclude` entries add:

```xml
<ClInclude Include="..\src\parse\method_no.h" />
```

In `rag2.0/rag2.0.vcxproj.filters` add:

```xml
<ClCompile Include="..\src\parse\method_no.cpp"><Filter>源文件\parse</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\parse\method_no.h"><Filter>头文件\parse</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj` add (method_no.cpp is needed because the tests project compiles `retrieval_chunk.cpp`, which will include it):

```xml
<ClCompile Include="..\src\parse\method_no.cpp" />
<ClCompile Include="..\tests\test_method_no.cpp" />
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters` add:

```xml
<ClCompile Include="..\tests\test_method_no.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\parse\method_no.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [x] **Step 3: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `parse/method_no.h` does not exist.

- [x] **Step 4: Create `src/parse/method_no.h`**

```cpp
#pragma once
#include <string>

// 从文本中抽取试验方法号（如 "T 0302-2024" / "T0302" / 全角破折号变体），
// 归一化为无空格、半角破折号形式（"T0302-2024" / "T0302"）。年份可选。
// 无匹配返回 ""。入库侧与查询侧共用，保证两侧字符串逐字可比。纯函数。
std::string extract_method_no(const std::string& text);
```

- [x] **Step 5: Create `src/parse/method_no.cpp`**

```cpp
#include "parse/method_no.h"
#include <regex>

namespace {

// 把 UTF-8 全角破折号（U+2014 em / U+2013 en）规整为 ASCII '-'。
std::string normalize_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 2 < s.size() &&
            static_cast<unsigned char>(s[i]) == 0xE2 &&
            static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0x94 ||
             static_cast<unsigned char>(s[i + 2]) == 0x93)) {
            out += '-';
            i += 3;
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string remove_ascii_spaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\t') out += c;
    }
    return out;
}

}  // namespace

std::string extract_method_no(const std::string& text) {
    // 年份段可选：入库标题 "T 0302-2024" 贪婪匹配到完整带年份；
    // 查询 "T0302" 仅匹配前缀。
    static const std::regex re(R"(T\s*\d{4}(?:\s*-\s*\d{4})?)");
    std::smatch m;
    std::string normalized = normalize_dashes(text);
    if (!std::regex_search(normalized, m, re)) return "";
    return remove_ascii_spaces(m.str(0));
}
```

- [x] **Step 6: Refactor `src/retrieve/retrieval_chunk.cpp` to use the shared function**

Add the include near the top (after the existing includes):

```cpp
#include "parse/method_no.h"
```

Delete the three inlined helpers from the anonymous namespace: `normalize_dashes`, `remove_ascii_spaces`, and `extract_method_no_from_text` (the block currently around lines 137–170).

In `method_no_for`, replace the two calls to `extract_method_no_from_text(...)` with `extract_method_no(...)`:

```cpp
std::string method_no_for(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::string self = extract_method_no(n.number + " " + n.title + " " + n.node_id);
    if (!self.empty()) return self;
    std::vector<const TreeNode*> ancestors = ancestor_chain(n, by_id);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        std::string found = extract_method_no((*it)->number + " " + (*it)->title + " " + (*it)->node_id);
        if (!found.empty()) return found;
    }
    return "";
}
```

- [x] **Step 7: Build and run the method_no tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="extract_method_no*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: 4 new cases pass; full suite **108** pass (104 + 4). The existing "retrieval chunks keep T method numbers..." case still passes (ingest behavior preserved).

- [x] **Step 8: Commit**

```powershell
git add src/parse/method_no.h src/parse/method_no.cpp tests/test_method_no.cpp src/retrieve/retrieval_chunk.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
refactor(m3a): extract shared method_no module (year optional)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 3: Query Understanding Module

**Files:**
- Create: `src/query/query_analysis.h`, `src/query/query_analysis.cpp`
- Create: `tests/test_query_analysis.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`, `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing test `tests/test_query_analysis.cpp`**

```cpp
#include <doctest/doctest.h>
#include "query/query_analysis.h"

TEST_CASE("analyze_query extracts and normalizes a standard code with year") {
    QueryAnalysis a = analyze_query("JTC 5210-2018 第5.1.2条是什么规定");
    CHECK(a.standard_code == "JTC5210-2018");
    CHECK(a.clause_no == "5.1.2");
    CHECK(a.method_no.empty());
    CHECK(a.clean_text == "JTC 5210-2018 第5.1.2条是什么规定");
}

TEST_CASE("analyze_query accepts a year-less standard code") {
    QueryAnalysis a = analyze_query("JTG 3420 里水泥怎么取样");
    CHECK(a.standard_code == "JTG3420");
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
}

TEST_CASE("analyze_query extracts a method number, not a standard code") {
    QueryAnalysis a = analyze_query("T0302 需要哪些仪具");
    CHECK(a.method_no == "T0302");
    CHECK(a.standard_code.empty());   // 单字母 T 前缀不构成标准号
    CHECK(a.clause_no.empty());
}

TEST_CASE("analyze_query leaves all codes empty for a plain question") {
    QueryAnalysis a = analyze_query("路基沉降怎么评定");
    CHECK(a.standard_code.empty());
    CHECK(a.clause_no.empty());
    CHECK(a.method_no.empty());
    CHECK(a.clean_text == "路基沉降怎么评定");
}

TEST_CASE("analyze_query does not treat a single-level number as a clause") {
    QueryAnalysis a = analyze_query("第5章讲了什么");
    CHECK(a.clause_no.empty());       // 单级号不算条款号
}
```

- [x] **Step 2: Add project entries (and new VS filter folders for query)**

In `rag2.0/rag2.0.vcxproj` add (near other source / header groups):

```xml
<ClCompile Include="..\src\query\query_analysis.cpp" />
```
```xml
<ClInclude Include="..\src\query\query_analysis.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`, add two new filter folders next to the existing `源文件\retrieve` / `头文件\retrieve` filter declarations:

```xml
<Filter Include="源文件\query"><UniqueIdentifier>{a0000000-0000-0000-0000-000000000010}</UniqueIdentifier></Filter>
<Filter Include="头文件\query"><UniqueIdentifier>{b0000000-0000-0000-0000-000000000010}</UniqueIdentifier></Filter>
```

and the file entries:

```xml
<ClCompile Include="..\src\query\query_analysis.cpp"><Filter>源文件\query</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\query\query_analysis.h"><Filter>头文件\query</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj` add:

```xml
<ClCompile Include="..\src\query\query_analysis.cpp" />
<ClCompile Include="..\tests\test_query_analysis.cpp" />
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters` add:

```xml
<ClCompile Include="..\tests\test_query_analysis.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\query\query_analysis.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [x] **Step 3: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `query/query_analysis.h` does not exist.

- [x] **Step 4: Create `src/query/query_analysis.h`**

```cpp
#pragma once
#include <string>

struct QueryAnalysis {
    std::string clean_text;    // 原始问题（dense 路输入，不抠编号）
    std::string standard_code; // 归一化裸代号，如 "JTC5210-2018" / "JTG3420"（空=未指定）
    std::string clause_no;     // 归一化条款号，如 "5.1.2"（空=未指定）
    std::string method_no;     // 归一化方法号，如 "T0302"（空=未指定）
};

// 查询理解（纯函数）：抽取标准号/条款号/方法号并归一化。
QueryAnalysis analyze_query(const std::string& question);
```

- [x] **Step 5: Create `src/query/query_analysis.cpp`**

```cpp
#include "query/query_analysis.h"
#include "parse/method_no.h"
#include <regex>

namespace {

std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '\t') out += c;
    return out;
}

}  // namespace

QueryAnalysis analyze_query(const std::string& question) {
    QueryAnalysis a;
    a.clean_text = question;

    // 标准号：比入库 extract_standard_no 更宽松——年份段可选，
    // 因为用户常省年份（"JTG 3420"）。需 >=2 个大写字母前缀，
    // 故单字母方法号 "T0302" 不会被误判为标准号。
    {
        static const std::regex pat(
            R"([A-Z]{2,}(?:/[A-Z]+)?[ \t]*[A-Z]{0,2}\d+(?:\.\d+)?(?:-(?:19|20)\d{2})?)");
        std::smatch m;
        if (std::regex_search(question, m, pat)) a.standard_code = strip_spaces(m[0].str());
    }

    // 条款号：可选"第"前缀 + 多级号（>=2 级，必含小数点）+ 可选"条"。
    // 误命中（如散文里的 "0.98"）无害：置顶时查不到该条款即空操作。
    {
        static const std::regex pat(R"((?:第\s*)?(\d+(?:\.\d+)+)\s*条?)");
        std::smatch m;
        if (std::regex_search(question, m, pat)) a.clause_no = m[1].str();
    }

    // 方法号：与入库共用同一抽取函数，年份可选。
    a.method_no = extract_method_no(question);

    return a;
}
```

- [x] **Step 6: Build and run the query_analysis tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="analyze_query*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: 5 new cases pass; full suite **113** pass (108 + 5).

- [x] **Step 7: Commit**

```powershell
git add src/query/query_analysis.h src/query/query_analysis.cpp tests/test_query_analysis.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(m3a): query understanding for standard/clause/method numbers

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 4: RetrievalFilter And Milvus Expression

**Files:**
- Create: `src/retrieve/retrieval_filter.h`, `src/retrieve/retrieval_filter.cpp`
- Create: `tests/test_retrieval_filter.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`, `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing test `tests/test_retrieval_filter.cpp`**

```cpp
#include <doctest/doctest.h>
#include "retrieve/retrieval_filter.h"

TEST_CASE("to_milvus_expr returns empty string for an empty filter") {
    RetrievalFilter f;
    CHECK(to_milvus_expr(f).empty());
}

TEST_CASE("to_milvus_expr builds a standard_id predicate when set") {
    RetrievalFilter f;
    f.standard_id = "12215131224082667446";
    CHECK(to_milvus_expr(f) == "standard_id == \"12215131224082667446\"");
}
```

- [x] **Step 2: Add project entries**

In `rag2.0/rag2.0.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\retrieval_filter.cpp" />
```
```xml
<ClInclude Include="..\src\retrieve\retrieval_filter.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`:

```xml
<ClCompile Include="..\src\retrieve\retrieval_filter.cpp"><Filter>源文件\retrieve</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\retrieve\retrieval_filter.h"><Filter>头文件\retrieve</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj` (retrieval_filter.cpp is also needed later because tests-compiled `dense_retriever.cpp` will call `to_milvus_expr`):

```xml
<ClCompile Include="..\src\retrieve\retrieval_filter.cpp" />
<ClCompile Include="..\tests\test_retrieval_filter.cpp" />
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters`:

```xml
<ClCompile Include="..\tests\test_retrieval_filter.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\retrieve\retrieval_filter.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [x] **Step 3: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `retrieve/retrieval_filter.h` does not exist.

- [x] **Step 4: Create `src/retrieve/retrieval_filter.h`**

```cpp
#pragma once
#include <string>

// 统一过滤条件。M3a 只有 standard_id（空=不过滤）；M3b 增 status 等字段。
struct RetrievalFilter {
    std::string standard_id;
};

// 空 filter → 空串（不下推）；否则形如 standard_id == "..."
std::string to_milvus_expr(const RetrievalFilter& f);
```

- [x] **Step 5: Create `src/retrieve/retrieval_filter.cpp`**

```cpp
#include "retrieve/retrieval_filter.h"

std::string to_milvus_expr(const RetrievalFilter& f) {
    // standard_id 由 find_standard_by_code 返回，源自文件路径哈希的十进制串，
    // 不含引号；若来源放宽需在此加转义。
    if (f.standard_id.empty()) return "";
    return "standard_id == \"" + f.standard_id + "\"";
}
```

- [x] **Step 6: Build and run the filter tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="to_milvus_expr*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: 2 new cases pass; full suite **115** pass (113 + 2).

- [x] **Step 7: Commit**

```powershell
git add src/retrieve/retrieval_filter.h src/retrieve/retrieval_filter.cpp tests/test_retrieval_filter.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(m3a): RetrievalFilter and milvus filter expression

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 5: RRF Fusion And Clause Pinning

**Files:**
- Create: `src/retrieve/rrf.h`, `src/retrieve/rrf.cpp`
- Create: `tests/test_rrf.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`, `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing test `tests/test_rrf.cpp`**

```cpp
#include <doctest/doctest.h>
#include "retrieve/rrf.h"

static Candidate cand(const std::string& sid, const std::string& cid,
                      float score, const std::string& src) {
    Candidate c; c.standard_id = sid; c.chunk_id = cid; c.score = score; c.source = src;
    return c;
}

TEST_CASE("rrf_fuse merges per-list ranks and dedups by chunk_id") {
    std::vector<Candidate> dense = {
        cand("S", "S:4.2.1#main", 0.9f, "dense"),
        cand("S", "S:4.3.1#main", 0.8f, "dense") };
    std::vector<Candidate> exact = {
        cand("S", "S:4.3.1#main", 1.0f, "exact"),
        cand("S", "S:1.0.1#main", 1.0f, "exact") };

    auto fused = rrf_fuse({dense, exact}, /*k=*/60, /*top_k=*/10);

    REQUIRE(fused.size() == 3);
    CHECK(fused[0].chunk_id == "S:4.3.1#main");          // 两路同中 → 排第一
    CHECK(fused[0].source.find("dense") != std::string::npos);
    CHECK(fused[0].source.find("exact") != std::string::npos);
}

TEST_CASE("rrf_fuse truncates to top_k") {
    std::vector<Candidate> a = {
        cand("S","S:1#main",1,"dense"), cand("S","S:2#main",1,"dense"),
        cand("S","S:3#main",1,"dense") };
    CHECK(rrf_fuse({a}, 60, 2).size() == 2);
}

TEST_CASE("pin_exact_clause moves an existing hit to the front") {
    std::vector<Candidate> fused = {
        cand("S","S:4.2.1#main",0.9f,"dense"),
        cand("S","S:5.1.2#main",0.5f,"dense"),
        cand("S","S:4.3.1#main",0.4f,"dense") };

    auto out = pin_exact_clause(fused, {"S:5.1.2#main"}, 10);

    REQUIRE(out.size() == 3);
    CHECK(out[0].chunk_id == "S:5.1.2#main");
    CHECK(out[0].source.find("pin") != std::string::npos);
    // 不重复出现
    int count = 0;
    for (auto& c : out) if (c.chunk_id == "S:5.1.2#main") ++count;
    CHECK(count == 1);
}

TEST_CASE("pin_exact_clause inserts a pinned id that was not recalled") {
    std::vector<Candidate> fused = { cand("S","S:4.2.1#main",0.9f,"dense") };
    auto out = pin_exact_clause(fused, {"S:9.9.9#main"}, 10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:9.9.9#main");
    CHECK(out[0].source == "exact_pin");
}

TEST_CASE("pin_exact_clause respects top_k after pinning") {
    std::vector<Candidate> fused = {
        cand("S","S:1#main",0.9f,"dense"), cand("S","S:2#main",0.8f,"dense") };
    auto out = pin_exact_clause(fused, {"S:9#main"}, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].chunk_id == "S:9#main");
}
```

- [x] **Step 2: Add project entries**

In `rag2.0/rag2.0.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\rrf.cpp" />
```
```xml
<ClInclude Include="..\src\retrieve\rrf.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`:

```xml
<ClCompile Include="..\src\retrieve\rrf.cpp"><Filter>源文件\retrieve</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\retrieve\rrf.h"><Filter>头文件\retrieve</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\rrf.cpp" />
<ClCompile Include="..\tests\test_rrf.cpp" />
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters`:

```xml
<ClCompile Include="..\tests\test_rrf.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\retrieve\rrf.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [x] **Step 3: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `retrieve/rrf.h` does not exist.

- [x] **Step 4: Create `src/retrieve/rrf.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"

// RRF：对多路候选按各自排名融合（score = Σ 1/(k+rank)），
// 按 chunk_id 去重、合并来源标记（"dense+exact"），降序排序，截断 top_k。纯函数。
std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k);

// 条款号精确命中置顶：pinned 中的 chunk_id 提到最前。
// 已在 fused 中 → 上移并去重，source 追加 "+pin"；
// 不在 → 插入第一位，source = "exact_pin"，score = 1.0。最后截断 top_k。纯函数。
std::vector<Candidate> pin_exact_clause(const std::vector<Candidate>& fused,
                                        const std::vector<std::string>& pinned_chunk_ids,
                                        int top_k);
```

- [x] **Step 5: Create `src/retrieve/rrf.cpp`**

```cpp
#include "retrieve/rrf.h"
#include <algorithm>
#include <map>
#include <set>

std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k) {
    struct Acc { Candidate c; double score = 0.0; std::string sources; };
    std::map<std::string, Acc> by_id;
    std::vector<std::string> order;   // 保持首次出现顺序，使排序稳定

    for (const auto& list : lists) {
        for (size_t rank = 0; rank < list.size(); ++rank) {
            const Candidate& c = list[rank];
            auto it = by_id.find(c.chunk_id);
            if (it == by_id.end()) {
                Acc a;
                a.c = c;
                a.c.source.clear();
                by_id.emplace(c.chunk_id, a);
                order.push_back(c.chunk_id);
                it = by_id.find(c.chunk_id);
            }
            it->second.score += 1.0 / (k + static_cast<int>(rank) + 1);
            if (it->second.sources.find(c.source) == std::string::npos)
                it->second.sources += (it->second.sources.empty() ? "" : "+") + c.source;
        }
    }

    std::vector<Candidate> out;
    out.reserve(order.size());
    for (const auto& id : order) {
        Acc& a = by_id[id];
        a.c.score = static_cast<float>(a.score);
        a.c.source = a.sources;
        out.push_back(a.c);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::vector<Candidate> pin_exact_clause(const std::vector<Candidate>& fused,
                                        const std::vector<std::string>& pinned_chunk_ids,
                                        int top_k) {
    std::vector<Candidate> out;
    std::set<std::string> pinned_set(pinned_chunk_ids.begin(), pinned_chunk_ids.end());

    // 先放置顶项（按 pinned 给定顺序，去重）
    std::set<std::string> placed;
    for (const auto& id : pinned_chunk_ids) {
        if (placed.count(id)) continue;
        placed.insert(id);
        auto it = std::find_if(fused.begin(), fused.end(),
                               [&](const Candidate& c) { return c.chunk_id == id; });
        if (it != fused.end()) {
            Candidate c = *it;
            c.source += "+pin";
            out.push_back(c);
        } else {
            Candidate c;
            c.chunk_id = id;
            c.score = 1.0f;
            c.source = "exact_pin";
            out.push_back(c);
        }
    }
    // 再放其余融合结果（跳过已置顶）
    for (const auto& c : fused) {
        if (pinned_set.count(c.chunk_id)) continue;
        out.push_back(c);
    }
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}
```

- [x] **Step 6: Build and run the rrf tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="rrf_fuse*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="pin_exact_clause*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: 6 new cases pass; full suite **121** pass (115 + 6).

- [x] **Step 7: Commit**

```powershell
git add src/retrieve/rrf.h src/retrieve/rrf.cpp tests/test_rrf.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(m3a): RRF fusion and clause pinning

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 6: Retriever Contract Evolution

Evolve the `Retriever` contract to carry a `RetrievalFilter`, push the filter into the Milvus search, and keep the build green with identical single-path behavior (the only caller passes an empty filter for now).

**Files:**
- Modify: `src/retrieve/retriever.h`
- Modify: `src/retrieve/dense_retriever.h`, `src/retrieve/dense_retriever.cpp`
- Modify: `src/milvus/milvus_rest.h`, `src/milvus/milvus_rest.cpp`
- Modify: `tests/test_milvus_body.cpp`
- Modify: `src/generate/answer_pipeline.cpp`

- [x] **Step 1: Add the failing filter request-body test**

Append to `tests/test_milvus_body.cpp`:

```cpp
TEST_CASE("build_search_body includes filter when provided") {
    std::vector<float> vec = {0.1f, 0.2f};
    std::string body = milvus::build_search_body(
        "clause_text", vec, 5, {"chunk_id", "node_id", "standard_id"},
        "standard_id == \"S1\"");
    auto j = nlohmann::json::parse(body);
    CHECK(j["filter"] == "standard_id == \"S1\"");
}

TEST_CASE("build_search_body omits filter key when expression is empty") {
    std::vector<float> vec = {0.1f, 0.2f};
    std::string body = milvus::build_search_body(
        "clause_text", vec, 5, {"chunk_id"}, "");
    auto j = nlohmann::json::parse(body);
    CHECK_FALSE(j.contains("filter"));
}
```

- [x] **Step 2: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `build_search_body` has no 5-argument overload.

- [x] **Step 3: Update `src/milvus/milvus_rest.h`**

Change the `build_search_body` declaration to add an optional filter:

```cpp
std::string build_search_body(const std::string& collection,
                              const std::vector<float>& query,
                              int top_k,
                              const std::vector<std::string>& output_fields,
                              const std::string& filter_expr = "");
```

Change the `MilvusRest::search` declaration to add an optional filter:

```cpp
    std::vector<Hit> search(const std::string& collection,
                            const std::vector<float>& query, int top_k,
                            const std::string& filter_expr = "");
```

- [x] **Step 4: Update `src/milvus/milvus_rest.cpp`**

Replace `build_search_body` with:

```cpp
std::string build_search_body(const std::string& collection, const std::vector<float>& query,
                              int top_k, const std::vector<std::string>& output_fields,
                              const std::string& filter_expr) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query});
    body["annsField"] = "dense";
    body["limit"] = top_k;
    body["outputFields"] = output_fields;
    if (!filter_expr.empty()) body["filter"] = filter_expr;
    return body.dump();
}
```

In `MilvusRest::search`, change the signature and pass the filter through:

```cpp
std::vector<Hit> MilvusRest::search(const std::string& collection,
                                    const std::vector<float>& query, int top_k,
                                    const std::string& filter_expr) {
    auto body = build_search_body(collection, query, top_k,
                                  {"chunk_id", "node_id", "standard_id"}, filter_expr);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus search failed: " + res.body + res.error);
    auto j = json::parse(res.body);
    std::vector<Hit> hits;
    for (auto& item : j["data"]) {
        Hit h;
        h.chunk_id = item.value("chunk_id", "");
        h.node_id = item.value("node_id", "");
        h.standard_id = item.value("standard_id", "");
        h.score = item.value("distance", 0.0f);
        hits.push_back(h);
    }
    return hits;
}
```

- [x] **Step 5: Evolve the contract in `src/retrieve/retriever.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "retrieve/retrieval_filter.h"

// 契约③（M3a 演进）：query + 过滤条件 → 候选列表。
// dense / exact 均实现本接口，RRF 在其输出上融合。
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<Candidate> retrieve(const std::string& query,
                                            const RetrievalFilter& filter,
                                            int top_k) = 0;
};
```

- [x] **Step 6: Adapt `src/retrieve/dense_retriever.h`**

```cpp
#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"

class DenseRetriever : public Retriever {
public:
    DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    milvus::MilvusRest& mv_;
    EmbeddingClient& embed_;
    std::string collection_;
};
```

- [x] **Step 7: Adapt `src/retrieve/dense_retriever.cpp`**

```cpp
#include "retrieve/dense_retriever.h"

DenseRetriever::DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed,
                               std::string collection)
    : mv_(mv), embed_(embed), collection_(std::move(collection)) {}

std::vector<Candidate> DenseRetriever::retrieve(const std::string& query,
                                                const RetrievalFilter& filter, int top_k) {
    std::vector<float> qv = embed_.embed(query);
    auto hits = mv_.search(collection_, qv, top_k, to_milvus_expr(filter));
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id;
        c.chunk_id = h.chunk_id;
        c.score = h.score;
        c.source = "dense";
        out.push_back(c);
    }
    return out;
}
```

- [x] **Step 8: Keep the one existing caller compiling (`src/generate/answer_pipeline.cpp`)**

In `answer_query`, change the retrieve call to pass a default empty filter (behavior identical — this caller is fully rewritten in Task 8):

```cpp
    auto candidates = retriever.retrieve(question, RetrievalFilter{}, top_k);
```

- [x] **Step 9: Build and run the milvus body tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_search_body*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: 2 new cases pass; full suite **123** pass (121 + 2). Behavior unchanged (single dense path, empty filter).

- [x] **Step 10: Commit**

```powershell
git add src/retrieve/retriever.h src/retrieve/dense_retriever.h src/retrieve/dense_retriever.cpp src/milvus/milvus_rest.h src/milvus/milvus_rest.cpp tests/test_milvus_body.cpp src/generate/answer_pipeline.cpp
git commit -m @'
feat(m3a): evolve Retriever contract with RetrievalFilter

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 7: PG Exact Methods And Retriever

**Files:**
- Modify: `src/db/pg_client.h`, `src/db/pg_client.cpp`
- Create: `src/retrieve/pg_exact_retriever.h`, `src/retrieve/pg_exact_retriever.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`

No unit tests (live PG required); covered by Task 8 live smoke. The tests project does NOT need `pg_exact_retriever.cpp` yet — nothing tests-compiled references it until Task 8 (which adds it to the tests project).

- [x] **Step 1: Declare the three lookup methods in `src/db/pg_client.h`**

Inside `class PgClient`, after `get_chunk`, add:

```cpp
    // M3a 精确路：归一化裸代号 → standard_id（现行优先；未找到返回空）
    std::string find_standard_by_code(const std::string& code);
    // 方法号前缀匹配取该方法全部 chunk（只填 chunk_id/standard_id，按 clause_no 排序）
    std::vector<RetrievalChunkRow> chunks_by_method(const std::string& method_prefix,
                                                    const std::string& standard_id);
    // 条款号精确命中（standard_id 可空），返回 chunk_id 列表
    std::vector<std::string> chunk_ids_by_clause(const std::string& clause_no,
                                                 const std::string& standard_id);
```

- [x] **Step 2: Implement them in `src/db/pg_client.cpp`**

Append at end of file:

```cpp
std::string PgClient::find_standard_by_code(const std::string& code) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    // 库内 standard_no 是带空格全称（"…（JTC 5210-2018）"），去空格后与裸代号子串匹配
    auto r = tx.exec(
        "SELECT standard_id FROM standards "
        "WHERE REPLACE(standard_no,' ','') LIKE $1 "
        "ORDER BY (status='现行') DESC LIMIT 1",
        pqxx::params{"%" + code + "%"});
    return r.empty() ? "" : std::string(r[0][0].c_str());
}

std::vector<RetrievalChunkRow> PgClient::chunks_by_method(const std::string& method_prefix,
                                                          const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    pqxx::result r;
    if (standard_id.empty()) {
        r = tx.exec(
            "SELECT chunk_id,standard_id FROM retrieval_chunks "
            "WHERE method_no LIKE $1 ORDER BY clause_no",
            pqxx::params{method_prefix + "%"});
    } else {
        r = tx.exec(
            "SELECT chunk_id,standard_id FROM retrieval_chunks "
            "WHERE method_no LIKE $1 AND standard_id=$2 ORDER BY clause_no",
            pqxx::params{method_prefix + "%", standard_id});
    }
    std::vector<RetrievalChunkRow> out;
    for (auto row : r) {
        RetrievalChunkRow c;
        c.chunk_id = row[0].c_str();
        c.standard_id = row[1].c_str();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> PgClient::chunk_ids_by_clause(const std::string& clause_no,
                                                       const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    pqxx::result r;
    if (standard_id.empty()) {
        r = tx.exec(
            "SELECT chunk_id FROM retrieval_chunks WHERE clause_no=$1",
            pqxx::params{clause_no});
    } else {
        r = tx.exec(
            "SELECT chunk_id FROM retrieval_chunks WHERE clause_no=$1 AND standard_id=$2",
            pqxx::params{clause_no, standard_id});
    }
    std::vector<std::string> out;
    for (auto row : r) out.push_back(row[0].c_str());
    return out;
}
```

- [x] **Step 3: Create `src/retrieve/pg_exact_retriever.h`**

```cpp
#pragma once
#include "retrieve/retriever.h"
#include "db/pg_client.h"

// 方法号精确路：构造时注入解析出的 method_no（契约③签名不携带编号）。
// method_no 为空时返回空列表；非空时前缀匹配取该方法全部 chunk。
class PgExactRetriever : public Retriever {
public:
    PgExactRetriever(PgClient& pg, std::string method_no);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    PgClient& pg_;
    std::string method_no_;
};
```

- [x] **Step 4: Create `src/retrieve/pg_exact_retriever.cpp`**

```cpp
#include "retrieve/pg_exact_retriever.h"

PgExactRetriever::PgExactRetriever(PgClient& pg, std::string method_no)
    : pg_(pg), method_no_(std::move(method_no)) {}

std::vector<Candidate> PgExactRetriever::retrieve(const std::string& /*query*/,
                                                  const RetrievalFilter& filter,
                                                  int /*top_k*/) {
    std::vector<Candidate> out;
    if (method_no_.empty()) return out;
    auto rows = pg_.chunks_by_method(method_no_, filter.standard_id);
    for (const auto& r : rows) {
        Candidate c;
        c.standard_id = r.standard_id;
        c.chunk_id = r.chunk_id;
        c.score = 1.0f;
        c.source = "exact";
        out.push_back(c);
    }
    return out;
}
```

- [x] **Step 5: Add project entries (main project only)**

In `rag2.0/rag2.0.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\pg_exact_retriever.cpp" />
```
```xml
<ClInclude Include="..\src\retrieve\pg_exact_retriever.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`:

```xml
<ClCompile Include="..\src\retrieve\pg_exact_retriever.cpp"><Filter>源文件\retrieve</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\retrieve\pg_exact_retriever.h"><Filter>头文件\retrieve</Filter></ClInclude>
```

- [x] **Step 6: Build and run the full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, **123** tests pass (additive; no new tests).

- [x] **Step 7: Commit**

```powershell
git add src/db/pg_client.h src/db/pg_client.cpp src/retrieve/pg_exact_retriever.h src/retrieve/pg_exact_retriever.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters
git commit -m @'
feat(m3a): pg exact lookups and method-number retriever

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 8: Text Search Orchestration And Wiring

**Files:**
- Create: `src/retrieve/text_search.h`, `src/retrieve/text_search.cpp`
- Modify: `src/generate/answer_pipeline.h`, `src/generate/answer_pipeline.cpp`
- Modify: `src/main.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`, `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj` (link new sources pulled in by `answer_pipeline.cpp`)

- [x] **Step 1: Create `src/retrieve/text_search.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"

// M3a 编排：查询理解 → 标准号收窄 → dense 路 + 方法号路 → RRF → 条款号置顶。
// 返回最终候选（chunk_id 维度，已截断 top_k）。
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv,
                                     EmbeddingClient& embed,
                                     PgClient& pg,
                                     const std::string& collection,
                                     int per_path_k, int top_k);
```

- [x] **Step 2: Create `src/retrieve/text_search.cpp`**

```cpp
#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/retrieval_filter.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"
#include <spdlog/spdlog.h>

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, PgClient& pg,
                                     const std::string& collection,
                                     int per_path_k, int top_k) {
    QueryAnalysis qa = analyze_query(question);

    // 标准号收窄：解析出代号则查 standard_id 下推；查不到回退全库并告警
    RetrievalFilter filter;
    if (!qa.standard_code.empty()) {
        std::string sid = pg.find_standard_by_code(qa.standard_code);
        if (!sid.empty()) filter.standard_id = sid;
        else spdlog::warn("查询提到标准号 {} 但库中未找到，回退全库检索", qa.standard_code);
    }

    // dense 路（必跑）+ 方法号路（有方法号才跑）
    std::vector<std::vector<Candidate>> lists;
    DenseRetriever dense(mv, embed, collection);
    lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
    if (!qa.method_no.empty()) {
        PgExactRetriever exact(pg, qa.method_no);
        lists.push_back(exact.retrieve(qa.clean_text, filter, per_path_k));
    }

    std::vector<Candidate> fused = rrf_fuse(lists, /*k=*/60, top_k);

    // 条款号置顶
    if (!qa.clause_no.empty()) {
        std::vector<std::string> pinned = pg.chunk_ids_by_clause(qa.clause_no, filter.standard_id);
        fused = pin_exact_clause(fused, pinned, top_k);
    }
    return fused;
}
```

- [x] **Step 3: Rewrite `src/generate/answer_pipeline.h`**

```cpp
#pragma once
#include <optional>
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "generate/context.h"
#include "generate/deepseek_client.h"

// 纯函数：chunk 行 + standards 行 -> LLM 上下文片段。
ContextFragment fragment_from_chunk(int idx,
                                    const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row);

// M3a 问答：text_retrieve 三角色召回 → PG 回查 → ContextFragment → DeepSeek。
// 候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         PgClient& pg,
                         deepseek::DeepSeekClient& ds,
                         const std::string& collection,
                         int top_k);
```

- [x] **Step 4: Rewrite `src/generate/answer_pipeline.cpp`**

```cpp
#include "generate/answer_pipeline.h"
#include "generate/prompt_builder.h"
#include "retrieve/text_search.h"
#include <vector>

ContextFragment fragment_from_chunk(int idx, const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row) {
    ContextFragment f;
    f.source_id = "S" + std::to_string(idx);
    f.standard_no = std_row ? std_row->standard_no : "";
    f.standard_name = std_row ? std_row->standard_name : "";
    f.status = std_row ? std_row->status : "";
    f.clause_no = chunk.clause_no;
    f.path = chunk.path_text;
    f.is_mandatory = false;   // 强制性条文识别留 M5
    // context_text 自带"路径：…"首行，与 f.path 在 prompt JSON 中重复一次；
    // 去重属 M5 prompt 改版范围
    f.text = chunk.context_text;
    return f;
}

std::string answer_query(const std::string& question, milvus::MilvusRest& mv,
                         EmbeddingClient& embed, PgClient& pg,
                         deepseek::DeepSeekClient& ds, const std::string& collection,
                         int top_k) {
    auto candidates = text_retrieve(question, mv, embed, pg, collection,
                                    /*per_path_k=*/top_k * 4, top_k);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        auto chunk = pg.get_chunk(c.chunk_id);   // 底座原则：以 PG 回查为权威源
        if (!chunk) continue;
        auto std_row = pg.get_standard(chunk->standard_id);
        fragments.push_back(fragment_from_chunk(idx++, *chunk, std_row));
    }
    if (fragments.empty())
        return "检索命中但回查规范原文为空，无法作答。";

    std::string sys = build_system_prompt();
    std::string user = build_user_prompt(question, fragments);
    return ds.chat(sys, user);
}
```

- [x] **Step 5: Update `src/main.cpp` `cmd_query`**

Replace the body of `cmd_query` with (drops the `DenseRetriever` construction):

```cpp
static int cmd_query(const Config& cfg, const std::string& question) {
    try {
        PgClient pg(cfg.pg_conninfo);
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                    cfg.deepseek_model, cfg.deepseek_key);

        std::string ans = answer_query(question, mv, embed, pg, ds,
                                       cfg.milvus_collection, /*top_k=*/5);
        std::cout << "\n===== 回答 =====\n" << ans << "\n";
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] query 失败: {}", e.what());
        return 1;
    }
}
```

If `#include "retrieve/dense_retriever.h"` in `main.cpp` is now unused, leave it (harmless) — `answer_pipeline.h` no longer needs it but removing risks an unrelated edit.

- [x] **Step 6: Register new sources**

In `rag2.0/rag2.0.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\text_search.cpp" />
```
```xml
<ClInclude Include="..\src\retrieve\text_search.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`:

```xml
<ClCompile Include="..\src\retrieve\text_search.cpp"><Filter>源文件\retrieve</Filter></ClCompile>
```
```xml
<ClInclude Include="..\src\retrieve\text_search.h"><Filter>头文件\retrieve</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj`, the tests project compiles `answer_pipeline.cpp` (via `test_context.cpp`), which now includes `text_search.h` and calls `text_retrieve` → its definition and `PgExactRetriever` must be linked into the test binary. Add:

```xml
<ClCompile Include="..\src\retrieve\text_search.cpp" />
<ClCompile Include="..\src\retrieve\pg_exact_retriever.cpp" />
```

(`dense_retriever.cpp`, `rrf.cpp`, `query_analysis.cpp`, `retrieval_filter.cpp`, `pg_client.cpp`, `milvus_rest.cpp` are already in the tests project.)

- [x] **Step 7: Build and run the full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, **123** tests pass (no new unit tests; `fragment_from_chunk` tests in `test_context.cpp` still pass — signature unchanged).

- [x] **Step 8: Live end-to-end smoke (only if services configured)**

Check connectivity first:

```powershell
.\rag2.0\x64\Debug\rag2.0.exe smoke
```

If smoke fails (PG/Milvus/embedding/DeepSeek unavailable), STOP and report "live smoke skipped: services unavailable"; build + unit tests already cover the code paths. If smoke passes, run the five spec scenarios:

```powershell
.\rag2.0\x64\Debug\rag2.0.exe query "JTC 5210 第5.1.2条是什么规定"
.\rag2.0\x64\Debug\rag2.0.exe query "T0302 需要哪些仪具"
.\rag2.0\x64\Debug\rag2.0.exe query "JTG 3420 里水泥怎么取样"
.\rag2.0\x64\Debug\rag2.0.exe query "路基沉降怎么评定"
.\rag2.0\x64\Debug\rag2.0.exe query "JTG 9999 的规定是什么"
```

Expected:
- Q1: answer cites clause 5.1.2 (clause pin + standard narrowing).
- Q2: answer cites a T0302 method clause (exact path fuses with dense).
- Q3: answer cites only 试验规程 (standard narrowing).
- Q4: behaves as before M3a (plain dense).
- Q5: a warn line "库中未找到…回退全库检索" appears; still answers (no crash).

- [x] **Step 9: Commit**

```powershell
git add src/retrieve/text_search.h src/retrieve/text_search.cpp src/generate/answer_pipeline.h src/generate/answer_pipeline.cpp src/main.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj
git commit -m @'
feat(m3a): three-role text retrieval orchestration

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Final Verification

```powershell
git status --short
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected:
- MSBuild succeeds.
- **123** doctest cases pass.
- Only intentional M3a files changed; `logs/` stays untracked.
- No `clause_id` references remain in the repo (Grep confirms).

---

## Self-Review Checklist

- [x] Spec §2.1 编号分角色: clause pin (Task 5 `pin_exact_clause` + Task 8 wiring), method path (Task 7 `PgExactRetriever` + Task 8 RRF), standard narrowing (Task 4 filter + Task 6 dense + Task 8 `find_standard_by_code`).
- [x] Spec §2.2 contract evolution: Task 6.
- [x] Spec §2.3 chunk_id rename: Task 1.
- [x] Spec §2.4 lenient fallback: Task 8 warn-and-continue on unknown standard code.
- [x] Spec §3 query understanding incl. shared method_no: Tasks 2 + 3.
- [x] Spec §4.1–4.5 filter/contract/dense/milvus: Tasks 4 + 6.
- [x] Spec §4.6–4.7 PgExactRetriever + PgClient methods: Task 7.
- [x] Spec §4.8–4.9 rrf + pin: Task 5.
- [x] Spec §4.10 answer_query + main wiring: Task 8.
- [x] Spec §7 test strategy: query_analysis (T3), method_no (T2), rrf+pin (T5), filter (T4), build_search_body filter (T6); PG methods unit-untested by design.
- [x] Spec §6 non-goals untouched: no Milvus schema change, no BM25/synonyms/status/wants_table/access_level/rerank.
- [x] Type consistency: `Candidate.chunk_id`, `RetrievalFilter.standard_id`, `analyze_query`→`QueryAnalysis{clean_text,standard_code,clause_no,method_no}`, `rrf_fuse`/`pin_exact_clause`, `text_retrieve`, `answer_query` new signature — consistent across tasks.
