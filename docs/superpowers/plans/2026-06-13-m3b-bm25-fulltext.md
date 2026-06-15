# M3b BM25 Full-Text And Status Filter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a BM25 keyword recall path (Milvus 2.6 built-in full-text search with jieba + custom dictionary), query-side synonym expansion, and a `status==现行` filter pipeline, fused with the existing dense + method-exact paths via the M3a RRF orchestration.

**Architecture:** New pure modules (`SynonymDict`) and a `Bm25Retriever` implementing the existing `Retriever` contract, plus a second Milvus collection schema (`text`/`sparse`/`status` + BM25 Function) reached through new `ensure_collection_text`/`insert_full`/`search_bm25` calls. `RetrievalFilter` gains a `status` field. The orchestration adds one list to `rrf_fuse`. A one-time collection drop + rebuild + rechunkload migrates the data.

**Tech Stack:** C++20, doctest, nlohmann/json, libpqxx, spdlog, Milvus REST v2 (v2.6.17, full-text search), MSBuild/vcpkg, Visual Studio `.vcxproj` projects.

Spec: `docs/superpowers/specs/2026-06-13-m3b-bm25-fulltext-design.md`

---

## Engineering Notes

Work in `D:\vs2022 code\rag2.0`, branch `V3.0`.

Before implementing: `git status --short` (expected clean except untracked `logs/`).

Build:
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Run all tests:
```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Run filtered: `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="<name>"`

Notes:
- Chinese string literals; projects compile `/utf-8`; save UTF-8 without BOM.
- Harmless vcpkg warning `MSB4011` — ignore.
- **Test-count discipline:** the baseline before this plan is **122** doctest cases. Each task states how many `TEST_CASE` blocks it adds; the expected total = previous green total + that number. If a pasted test block contains a different count than stated, trust the actual blocks and report the real number — do not weaken tests to hit a number.
- **Live-only changes:** Tasks 2 and 5 change runtime behavior (status filter on search; full-field insert) that only works against the rebuilt full-text collection. Between those tasks and Task 7 the unit tests + build stay green, but a live `query`/`ingest` would fail until Task 7 rebuilds the collection. Do NOT run live `query`/`ingest`/`chunkload` before Task 7.
- **Milvus full-text schema is the one externally-uncertain piece.** The create-collection JSON for jieba analyzer + BM25 Function is given per Milvus 2.6 conventions, but the exact `analyzer_params`/`functions`/sparse-index keys may differ by patch version. The unit test in Task 3 pins the body our code produces; Task 7 (live rebuild) is where reality is confirmed. If Task 7's create call returns a schema error, the implementer is authorized to adjust the JSON shape per the Milvus error message and update the Task 3 body test to match — report any such adjustment.
- PG/Milvus/embedding network methods are not unit-tested (no test doubles; codebase convention). They are covered by the Task 7 live smoke.

---

## File Structure

Create:

| File | Responsibility |
|---|---|
| `src/query/synonyms.h` / `.cpp` | `SynonymDict` load + `expand` (pure) |
| `src/retrieve/bm25_retriever.h` / `.cpp` | BM25 path (Retriever): synonym expand → Milvus sparse search |
| `config/synonyms.txt` | seed domain synonyms (query-side) |
| `config/user_dict.txt` | seed jieba custom dictionary (collection-side) |
| `tests/test_synonyms.cpp` | expand cases |

Modify:

| File | Responsibility |
|---|---|
| `src/retrieve/retrieval_filter.h` / `.cpp` | add `status` field; `to_milvus_expr` emits status [+ standard_id] |
| `tests/test_retrieval_filter.cpp` | update 2 M3a assertions for the new default |
| `src/milvus/milvus_rest.h` / `.cpp` | `build_text_collection_body`/`ensure_collection_text`, `build_insert_full_body`/`insert_full`, `build_bm25_body`/`search_bm25` |
| `tests/test_milvus_body.cpp` | body tests for the three new builders |
| `src/ingest/chunk_loader.h` / `.cpp` | `load_chunks` gains `status`; insert via `insert_full(text=embedding_text)` |
| `src/ingest/ingest_pipeline.h` / `.cpp` | `ingest_file` gains `user_dict`; uses `ensure_collection_text` + status |
| `src/retrieve/text_search.h` / `.cpp` | add `const SynonymDict&` param; add Bm25 path to lists |
| `src/generate/answer_pipeline.h` / `.cpp` | `answer_query` gains `const SynonymDict&` |
| `src/main.cpp` | load dicts; thread synonyms + user_dict + status through commands |
| `rag2.0/rag2.0.vcxproj` + `.filters`, `rag2.0.tests/...` | register new sources/tests |

---

## Task 1: SynonymDict (pure)

**Files:**
- Create: `src/query/synonyms.h`, `src/query/synonyms.cpp`, `config/synonyms.txt`, `tests/test_synonyms.cpp`
- Modify: 4 project files

- [x] **Step 1: Write `tests/test_synonyms.cpp`**

```cpp
#include <doctest/doctest.h>
#include "query/synonyms.h"

TEST_CASE("SynonymDict expand appends synonyms of matched terms, keeping the original") {
    SynonymDict d;
    d.load_from_lines({"针入度,贯入度", "精度,精密度,重复性"});
    std::string out = d.expand("沥青针入度试验精度");
    CHECK(out.find("针入度") != std::string::npos);   // 原词保留
    CHECK(out.find("贯入度") != std::string::npos);   // 同义词追加
    CHECK(out.find("精密度") != std::string::npos);
    CHECK(out.find("重复性") != std::string::npos);
}

TEST_CASE("SynonymDict expand is a no-op when nothing matches") {
    SynonymDict d;
    d.load_from_lines({"针入度,贯入度"});
    CHECK(d.expand("水泥取样方法") == "水泥取样方法");
}

TEST_CASE("SynonymDict expand does not duplicate a synonym already present") {
    SynonymDict d;
    d.load_from_lines({"精度,精密度"});
    std::string out = d.expand("精度和精密度");
    // "精密度" 已在原查询中，不应再次追加（出现次数仍为 1）
    size_t first = out.find("精密度");
    REQUIRE(first != std::string::npos);
    CHECK(out.find("精密度", first + 1) == std::string::npos);
}

TEST_CASE("SynonymDict skips blank and comment lines") {
    SynonymDict d;
    d.load_from_lines({"# 注释", "", "针入度,贯入度"});
    CHECK(d.expand("针入度").find("贯入度") != std::string::npos);
}
```

- [x] **Step 2: Register project files**

`rag2.0/rag2.0.vcxproj`: `<ClCompile Include="..\src\query\synonyms.cpp" />`, `<ClInclude Include="..\src\query\synonyms.h" />`.
`rag2.0/rag2.0.vcxproj.filters`: `<ClCompile Include="..\src\query\synonyms.cpp"><Filter>源文件\query</Filter></ClCompile>`, `<ClInclude Include="..\src\query\synonyms.h"><Filter>头文件\query</Filter></ClInclude>`.
`rag2.0.tests/rag2.0.tests.vcxproj`: `<ClCompile Include="..\src\query\synonyms.cpp" />`, `<ClCompile Include="..\tests\test_synonyms.cpp" />`.
`rag2.0.tests/rag2.0.tests.vcxproj.filters`: `<ClCompile Include="..\tests\test_synonyms.cpp"><Filter>测试</Filter></ClCompile>`, `<ClCompile Include="..\src\query\synonyms.cpp"><Filter>被测源码</Filter></ClCompile>`.

- [x] **Step 3: Build, expect FAIL** (`query/synonyms.h` missing).

- [x] **Step 4: Create `src/query/synonyms.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>

// 领域同义词词典。每行一组互为同义的词（逗号分隔，# 开头为注释）。
// 仅用于查询侧 BM25 路扩展（纯逻辑，可单测）。
class SynonymDict {
public:
    void load_from_lines(const std::vector<std::string>& lines);
    void load_from_file(const std::string& path);   // 文件缺失则保持空词典
    // 查询中命中某词则追加其同义词（空格分隔），原词与已含词不重复追加。
    std::string expand(const std::string& query) const;
private:
    std::map<std::string, std::vector<std::string>> alias_;  // 词 -> 同组其他词
};
```

- [x] **Step 5: Create `src/query/synonyms.cpp`**

```cpp
#include "query/synonyms.h"
#include <fstream>
#include <sstream>

namespace {

std::vector<std::string> split_commas(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t\r");
        size_t b = item.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

}  // namespace

void SynonymDict::load_from_lines(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        auto group = split_commas(line);
        for (size_t i = 0; i < group.size(); ++i)
            for (size_t j = 0; j < group.size(); ++j)
                if (i != j) alias_[group[i]].push_back(group[j]);
    }
}

void SynonymDict::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    load_from_lines(lines);
}

std::string SynonymDict::expand(const std::string& query) const {
    std::string extra;
    for (const auto& kv : alias_) {
        if (query.find(kv.first) != std::string::npos) {
            for (const auto& syn : kv.second) {
                if (query.find(syn) == std::string::npos &&
                    extra.find(syn) == std::string::npos)
                    extra += " " + syn;
            }
        }
    }
    return extra.empty() ? query : query + extra;
}
```

- [x] **Step 6: Create `config/synonyms.txt`**

```text
# 每行一组同义词，逗号分隔，查询侧 BM25 扩展用
沥青混合料,AC,沥青砼
针入度,贯入度
精度,精密度,重复性,允许误差
压实度,密实度
回弹模量,弹性模量
抗压强度,立方体抗压强度
延度,延伸度
```

- [x] **Step 7: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="SynonymDict*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 4 new cases pass; full suite **126** (122 + 4).

- [x] **Step 8: Commit**

```powershell
git add src/query/synonyms.h src/query/synonyms.cpp config/synonyms.txt tests/test_synonyms.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(m3b): synonym dictionary with query expansion

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 2: RetrievalFilter Status

**Files:**
- Modify: `src/retrieve/retrieval_filter.h`, `src/retrieve/retrieval_filter.cpp`, `tests/test_retrieval_filter.cpp`

- [x] **Step 1: Update `tests/test_retrieval_filter.cpp`** — replace the two existing cases and add a third:

```cpp
#include <doctest/doctest.h>
#include "retrieve/retrieval_filter.h"

TEST_CASE("to_milvus_expr defaults to current-status filter") {
    RetrievalFilter f;   // status 默认 "现行"
    CHECK(to_milvus_expr(f) == "status == \"现行\"");
}

TEST_CASE("to_milvus_expr conjoins status and standard_id when both set") {
    RetrievalFilter f;
    f.standard_id = "12215131224082667446";
    CHECK(to_milvus_expr(f) ==
          "status == \"现行\" and standard_id == \"12215131224082667446\"");
}

TEST_CASE("to_milvus_expr returns empty when both status and standard_id are empty") {
    RetrievalFilter f;
    f.status = "";
    CHECK(to_milvus_expr(f).empty());
}

TEST_CASE("to_milvus_expr with only standard_id and empty status") {
    RetrievalFilter f;
    f.status = "";
    f.standard_id = "S1";
    CHECK(to_milvus_expr(f) == "standard_id == \"S1\"");
}
```

- [x] **Step 2: Build, expect FAIL** (default still produces empty/standard-only).

- [x] **Step 3: Update `src/retrieve/retrieval_filter.h`**

```cpp
#pragma once
#include <string>

// 统一过滤条件。status 默认只召回现行；空串=不按状态过滤。
// standard_id 空=不按标准过滤。M6 扩展更多版本字段。
struct RetrievalFilter {
    std::string standard_id;
    std::string status = "现行";
};

// 拼 Milvus 标量过滤表达式：status 与 standard_id 各自非空则 AND 连接；
// 皆空 → 空串（不下推）。
std::string to_milvus_expr(const RetrievalFilter& f);
```

- [x] **Step 4: Update `src/retrieve/retrieval_filter.cpp`**

```cpp
#include "retrieve/retrieval_filter.h"

std::string to_milvus_expr(const RetrievalFilter& f) {
    // status/standard_id 均来自受控来源（"现行"/"作废"、文件路径哈希十进制串），
    // 不含引号；若来源放宽需在此加转义。
    std::string expr;
    if (!f.status.empty())
        expr = "status == \"" + f.status + "\"";
    if (!f.standard_id.empty()) {
        if (!expr.empty()) expr += " and ";
        expr += "standard_id == \"" + f.standard_id + "\"";
    }
    return expr;
}
```

- [x] **Step 5: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="to_milvus_expr*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 4 cases pass (2 updated + 2 new); full suite **128** (126 + 2 net new). The old "empty filter" and "standard_id predicate" cases are replaced, not added.

- [x] **Step 6: Commit**

```powershell
git add src/retrieve/retrieval_filter.h src/retrieve/retrieval_filter.cpp tests/test_retrieval_filter.cpp
git commit -m @'
feat(m3b): RetrievalFilter status with default current-only

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 3: Milvus Full-Text Builders

Add three pure body-builders and their `MilvusRest` methods for the full-text collection. The old 4-field `ensure_collection`/`insert` stay (unit tests + possible fallback). Additive — nothing calls the new methods until Task 5.

**Files:**
- Modify: `src/milvus/milvus_rest.h`, `src/milvus/milvus_rest.cpp`, `tests/test_milvus_body.cpp`

- [x] **Step 1: Add failing body tests** — append to `tests/test_milvus_body.cpp`:

```cpp
TEST_CASE("build_insert_full_body carries status and text, not sparse") {
    std::vector<float> vec = {1.0f, 2.0f};
    std::string body = milvus::build_insert_full_body(
        "clause_text", "c1#main", "c1", "s1", "现行", "正文文本", vec);
    auto j = nlohmann::json::parse(body);
    auto row = j["data"][0];
    CHECK(row["chunk_id"] == "c1#main");
    CHECK(row["node_id"] == "c1");
    CHECK(row["standard_id"] == "s1");
    CHECK(row["status"] == "现行");
    CHECK(row["text"] == "正文文本");
    CHECK(row["dense"].size() == 2);
    CHECK_FALSE(row.contains("sparse"));   // 由 BM25 Function 自动生成
}

TEST_CASE("build_bm25_body searches the sparse field with raw query text") {
    std::string body = milvus::build_bm25_body(
        "clause_text", "针入度 精度", 5, {"chunk_id", "node_id", "standard_id"},
        "status == \"现行\"");
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["annsField"] == "sparse");
    CHECK(j["data"][0] == "针入度 精度");   // 文本，不是向量
    CHECK(j["limit"] == 5);
    CHECK(j["filter"] == "status == \"现行\"");
    CHECK(j["outputFields"][0] == "chunk_id");
}

TEST_CASE("build_text_collection_body declares text analyzer, sparse, and BM25 function") {
    std::string body = milvus::build_text_collection_body("clause_text", 4096,
                                                          {"针入度", "压实度"});
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    auto fields = j["schema"]["fields"];
    // 收集字段名
    bool has_status = false, has_text = false, has_sparse = false, has_dense = false;
    for (auto& fdef : fields) {
        std::string name = fdef["fieldName"];
        if (name == "status") has_status = true;
        if (name == "text") has_text = true;
        if (name == "sparse") has_sparse = true;
        if (name == "dense") has_dense = true;
    }
    CHECK(has_status);
    CHECK(has_text);
    CHECK(has_sparse);
    CHECK(has_dense);
    // BM25 Function: text -> sparse
    REQUIRE(j["schema"].contains("functions"));
    auto fn = j["schema"]["functions"][0];
    CHECK(fn["type"] == "BM25");
    CHECK(fn["inputFieldNames"][0] == "text");
    CHECK(fn["outputFieldNames"][0] == "sparse");
}
```

- [x] **Step 2: Build, expect FAIL** (the three builders don't exist).

- [x] **Step 3: Declare in `src/milvus/milvus_rest.h`** — add the pure builders next to the existing ones:

```cpp
std::string build_insert_full_body(const std::string& collection,
                                   const std::string& chunk_id,
                                   const std::string& node_id,
                                   const std::string& standard_id,
                                   const std::string& status,
                                   const std::string& text,
                                   const std::vector<float>& dense);
std::string build_bm25_body(const std::string& collection,
                            const std::string& query_text,
                            int top_k,
                            const std::vector<std::string>& output_fields,
                            const std::string& filter_expr = "");
std::string build_text_collection_body(const std::string& collection, int dim,
                                       const std::vector<std::string>& user_dict);
```

and in `class MilvusRest` add:

```cpp
    // M3b 全文检索集合：text(analyzer)+sparse(BM25 Function)+status 标量。
    void ensure_collection_text(const std::string& collection, int dim,
                                const std::vector<std::string>& user_dict);
    void insert_full(const std::string& collection, const std::string& chunk_id,
                     const std::string& node_id, const std::string& standard_id,
                     const std::string& status, const std::string& text,
                     const std::vector<float>& dense);
    std::vector<Hit> search_bm25(const std::string& collection,
                                 const std::string& query_text,
                                 int top_k, const std::string& filter_expr = "");
```

- [x] **Step 4: Implement in `src/milvus/milvus_rest.cpp`** — add the builders and methods:

```cpp
std::string build_insert_full_body(const std::string& collection, const std::string& chunk_id,
                                   const std::string& node_id, const std::string& standard_id,
                                   const std::string& status, const std::string& text,
                                   const std::vector<float>& dense) {
    json row;
    row["chunk_id"] = chunk_id;
    row["node_id"] = node_id;
    row["standard_id"] = standard_id;
    row["status"] = status;
    row["text"] = text;        // sparse 由 BM25 Function 自动生成，不传
    row["dense"] = dense;
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({row});
    return body.dump();
}

std::string build_bm25_body(const std::string& collection, const std::string& query_text,
                            int top_k, const std::vector<std::string>& output_fields,
                            const std::string& filter_expr) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query_text});   // 原始文本，Milvus 分词 + BM25
    body["annsField"] = "sparse";
    body["limit"] = top_k;
    body["outputFields"] = output_fields;
    if (!filter_expr.empty()) body["filter"] = filter_expr;
    return body.dump();
}

std::string build_text_collection_body(const std::string& collection, int dim,
                                       const std::vector<std::string>& user_dict) {
    json analyzer_params;
    analyzer_params["tokenizer"] = { {"type", "jieba"}, {"dict", user_dict} };

    json schema;
    schema["autoID"] = false;
    schema["fields"] = json::array({
        { {"fieldName","chunk_id"}, {"dataType","VarChar"}, {"isPrimary",true},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","node_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","standard_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 128} }} },
        { {"fieldName","status"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 32} }} },
        { {"fieldName","text"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 8192}, {"enable_analyzer", true},
                                  {"analyzer_params", analyzer_params} }} },
        { {"fieldName","dense"}, {"dataType","FloatVector"},
          {"elementTypeParams", { {"dim", dim} }} },
        { {"fieldName","sparse"}, {"dataType","SparseFloatVector"} }
    });
    schema["functions"] = json::array({
        { {"name","bm25_fn"}, {"type","BM25"},
          {"inputFieldNames", json::array({"text"})},
          {"outputFieldNames", json::array({"sparse"})} }
    });

    json index = json::array({
        { {"fieldName","dense"}, {"indexName","dense_idx"}, {"metricType","COSINE"} },
        { {"fieldName","sparse"}, {"indexName","sparse_idx"}, {"metricType","BM25"},
          {"indexType","SPARSE_INVERTED_INDEX"} }
    });

    json body;
    body["collectionName"] = collection;
    body["schema"] = schema;
    body["indexParams"] = index;
    return body.dump();
}

void MilvusRest::ensure_collection_text(const std::string& collection, int dim,
                                        const std::vector<std::string>& user_dict) {
    {   // 已存在则直接返回
        json q; q["collectionName"] = collection;
        auto has = http::post_json(base_url_, "/v2/vectordb/collections/has", q.dump(),
                                   auth_headers(token_));
        if (has.ok()) {
            auto j = json::parse(has.body, nullptr, false);
            if (!j.is_discarded() && j.contains("data") &&
                j["data"].contains("has") && j["data"]["has"].get<bool>())
                return;
        }
    }
    auto body = build_text_collection_body(collection, dim, user_dict);
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/create", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus create text collection failed: " + res.body + res.error);
}

void MilvusRest::insert_full(const std::string& collection, const std::string& chunk_id,
                             const std::string& node_id, const std::string& standard_id,
                             const std::string& status, const std::string& text,
                             const std::vector<float>& dense) {
    auto body = build_insert_full_body(collection, chunk_id, node_id, standard_id,
                                       status, text, dense);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/insert", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus insert_full failed: " + res.body + res.error);
}

std::vector<Hit> MilvusRest::search_bm25(const std::string& collection,
                                         const std::string& query_text, int top_k,
                                         const std::string& filter_expr) {
    auto body = build_bm25_body(collection, query_text, top_k,
                                {"chunk_id", "node_id", "standard_id"}, filter_expr);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus search_bm25 failed: " + res.body + res.error);
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

> Note: if `<map>` is needed for `auth_headers` it is already included by the existing file. The `auth_headers`/`http::post_json` helpers already exist — match their current usage exactly (read the file).

- [x] **Step 5: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_insert_full_body*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_bm25_body*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_text_collection_body*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 3 new cases pass; full suite **131** (128 + 3).

- [x] **Step 6: Commit**

```powershell
git add src/milvus/milvus_rest.h src/milvus/milvus_rest.cpp tests/test_milvus_body.cpp
git commit -m @'
feat(m3b): milvus full-text collection, insert_full, bm25 search

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 4: Bm25Retriever

**Files:**
- Create: `src/retrieve/bm25_retriever.h`, `src/retrieve/bm25_retriever.cpp`
- Modify: `rag2.0/rag2.0.vcxproj` + `.filters` (main project only; tests project gets it in Task 6 when `text_search.cpp`/`answer_pipeline.cpp` link it)

- [x] **Step 1: Create `src/retrieve/bm25_retriever.h`**

```cpp
#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "query/synonyms.h"

// BM25 关键词路：查询经同义词扩展后走 Milvus 全文检索（sparse 字段）。
class Bm25Retriever : public Retriever {
public:
    Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    milvus::MilvusRest& mv_;
    const SynonymDict& syn_;
    std::string collection_;
};
```

- [x] **Step 2: Create `src/retrieve/bm25_retriever.cpp`**

```cpp
#include "retrieve/bm25_retriever.h"

Bm25Retriever::Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn,
                             std::string collection)
    : mv_(mv), syn_(syn), collection_(std::move(collection)) {}

std::vector<Candidate> Bm25Retriever::retrieve(const std::string& query,
                                               const RetrievalFilter& filter, int top_k) {
    std::string expanded = syn_.expand(query);   // 同义词扩展仅作用于 BM25 路
    auto hits = mv_.search_bm25(collection_, expanded, top_k, to_milvus_expr(filter));
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id;
        c.chunk_id = h.chunk_id;
        c.score = h.score;
        c.source = "bm25";
        out.push_back(c);
    }
    return out;
}
```

- [x] **Step 3: Register (main project only)**

`rag2.0/rag2.0.vcxproj`: `<ClCompile Include="..\src\retrieve\bm25_retriever.cpp" />`, `<ClInclude Include="..\src\retrieve\bm25_retriever.h" />`.
`rag2.0/rag2.0.vcxproj.filters`: `<ClCompile Include="..\src\retrieve\bm25_retriever.cpp"><Filter>源文件\retrieve</Filter></ClCompile>`, `<ClInclude Include="..\src\retrieve\bm25_retriever.h"><Filter>头文件\retrieve</Filter></ClInclude>`.

- [x] **Step 4: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: build succeeds, **131** (additive, no new tests; `to_milvus_expr` available via retriever.h→retrieval_filter.h).

- [x] **Step 5: Commit**

```powershell
git add src/retrieve/bm25_retriever.h src/retrieve/bm25_retriever.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters
git commit -m @'
feat(m3b): bm25 retriever over milvus full-text

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 5: Loader And Ingest Write Full Fields

Rewire ingest/chunkload to the full-text collection: write `status` + `text` via `insert_full`, build the collection via `ensure_collection_text` with the user dictionary.

**Files:**
- Modify: `src/ingest/chunk_loader.h`, `src/ingest/chunk_loader.cpp`
- Modify: `src/ingest/ingest_pipeline.h`, `src/ingest/ingest_pipeline.cpp`
- Modify: `src/main.cpp` (cmd_ingest, cmd_chunkload: load user_dict, ensure_collection_text, status)

No unit tests (live). Build-green + Task 7 live smoke cover it.

- [x] **Step 1: `load_chunks` gains a status param — `src/ingest/chunk_loader.h`**

Change the declaration:

```cpp
// chunk_cache -> PG retrieval_chunks + Milvus 全文集合。按 standard_id 先删后插。
// status 写入每个 Milvus 行（来自 standards.status）；text 字段写 embedding_text。
ChunkLoadResult load_chunks(const RetrievalChunkCache& cache,
                            PgClient& pg,
                            milvus::MilvusRest& mv,
                            EmbeddingClient& embed,
                            const std::string& collection,
                            const std::string& status);
```

- [x] **Step 2: `src/ingest/chunk_loader.cpp`** — use `insert_full`:

In `load_chunks`, change the signature to match the header and replace the insert call inside the loop:

```cpp
ChunkLoadResult load_chunks(const RetrievalChunkCache& cache, PgClient& pg,
                            milvus::MilvusRest& mv, EmbeddingClient& embed,
                            const std::string& collection, const std::string& status) {
    ChunkLoadResult result;
    result.chunk_count = static_cast<int>(cache.chunks.size());

    mv.delete_by_standard(collection, cache.standard_id);
    result.deleted_count = pg.delete_chunks_by_standard(cache.standard_id);

    for (const auto& c : cache.chunks) {
        pg.insert_chunk(chunk_to_row(c));
        try {
            std::vector<float> vec = embed.embed(c.embedding_text);
            mv.insert_full(collection, c.chunk_id, c.node_id, c.standard_id,
                           status, c.embedding_text, vec);
            ++result.embedded_count;
        } catch (const std::exception& e) {
            spdlog::warn("chunk embed/写入失败，跳过: {} ({})", c.chunk_id, e.what());
        }
    }
    return result;
}
```

- [x] **Step 3: `ingest_file` gains user_dict — `src/ingest/ingest_pipeline.h`**

Change the declaration (add `user_dict` before the defaulted `cache_dir`):

```cpp
IngestResult ingest_file(const std::string& file_path,
                         Parser& parser,
                         PgClient& pg,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         const std::string& collection,
                         const std::vector<std::string>& user_dict,
                         const std::string& cache_dir = "data/parse_cache");
```

Add `#include <vector>` if not present.

- [x] **Step 4: `src/ingest/ingest_pipeline.cpp`** — use full-text collection + pass status:

Change the signature to match, and replace the two Milvus/load lines:

```cpp
IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection,
                         const std::vector<std::string>& user_dict,
                         const std::string& cache_dir) {
    ParsedDoc doc = parser.parse(file_path);

    std::string stem = path_utf8::stem(file_path);
    std::string standard_id = make_id(file_path);
    std::string page1 = doc.pages.empty() ? std::string() : doc.pages[0].text;
    doc.standard_no = extract_standard_no(page1, stem);
    write_parse_cache(cache_dir + "/" + standard_id + ".json", doc);

    ClauseTree tree = build_clause_tree(doc, standard_id);
    write_tree_cache("data/tree_cache/" + standard_id + ".json", tree);
    RetrievalChunkCache chunks = build_retrieval_chunk_cache(tree);
    write_chunk_cache("data/chunk_cache/" + standard_id + ".json", chunks);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = tree.standard_no.empty() ? doc.standard_no : tree.standard_no;
    s.standard_name = stem;
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);

    mv.ensure_collection_text(collection, embed.dim(), user_dict);
    ChunkLoadResult r = load_chunks(chunks, pg, mv, embed, collection, s.status);

    IngestResult result;
    result.standard_id = standard_id;
    result.clause_count = r.chunk_count;
    result.embedded_count = r.embedded_count;
    spdlog::info("入库完成: chunks={} embedded={} deleted_old={} (standard_id={})",
                 r.chunk_count, r.embedded_count, r.deleted_count, standard_id);
    return result;
}
```

- [x] **Step 5: `src/main.cpp`** — add a dict-loading helper and rewire both commands.

Near the existing `read_file` helper, add:

```cpp
// 读词典文件为非空、非注释行列表（用于 jieba 自定义词典）。文件缺失返回空。
static std::vector<std::string> read_dict_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t b = line.find_last_not_of(" \t");
        out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}
```

Ensure `#include "query/synonyms.h"` and `#include <vector>` are present near the other includes.

In `cmd_ingest`, replace the `ingest_file(...)` call to pass the loaded dict:

```cpp
        std::vector<std::string> user_dict = read_dict_lines("config/user_dict.txt");
        auto r = ingest_file(cfg.doc_path, parser, pg, mv, embed, cfg.milvus_collection,
                             user_dict);
```

In `cmd_chunkload`, replace the `ensure_collection` + `load_chunks` lines:

```cpp
        std::vector<std::string> user_dict = read_dict_lines("config/user_dict.txt");
        mv.ensure_collection_text(cfg.milvus_collection, embed.dim(), user_dict);

        std::string status = "现行";
        if (auto srow = pg.get_standard(cache.standard_id)) status = srow->status;
        ChunkLoadResult r = load_chunks(cache, pg, mv, embed, cfg.milvus_collection, status);
```

(The placeholder-standard upsert block above it stays; it sets status "现行" for a new standard, which `get_standard` then returns.)

- [x] **Step 6: Create `config/user_dict.txt`**

```text
针入度
压实度
回弹模量
抗压强度
沥青混合料
延度
软化点
集料筛分
水泥净浆
JTG/T
GB/T
```

- [x] **Step 7: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, **131** (no new unit tests; `test_*` that construct `ingest_file`/`load_chunks` — there are none — so nothing breaks). Do NOT run live ingest/chunkload yet.

- [x] **Step 8: Commit**

```powershell
git add src/ingest/chunk_loader.h src/ingest/chunk_loader.cpp src/ingest/ingest_pipeline.h src/ingest/ingest_pipeline.cpp src/main.cpp config/user_dict.txt
git commit -m @'
feat(m3b): ingest writes status and text on full-text collection

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 6: Wire BM25 Into Orchestration

**Files:**
- Modify: `src/retrieve/text_search.h`, `src/retrieve/text_search.cpp`
- Modify: `src/generate/answer_pipeline.h`, `src/generate/answer_pipeline.cpp`
- Modify: `src/main.cpp` (cmd_query loads synonyms)
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj` (link `bm25_retriever.cpp` for the tests binary)

- [x] **Step 1: `src/retrieve/text_search.h`** — add `const SynonymDict&`:

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"
#include "query/synonyms.h"

// M3b 编排：查询理解 → 标准号收窄 → dense + BM25 + 方法号 → RRF → 条款号置顶。
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv,
                                     EmbeddingClient& embed,
                                     PgClient& pg,
                                     const SynonymDict& syn,
                                     const std::string& collection,
                                     int per_path_k, int top_k);
```

- [x] **Step 2: `src/retrieve/text_search.cpp`** — add the BM25 path:

```cpp
#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/bm25_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/retrieval_filter.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"
#include <spdlog/spdlog.h>

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, PgClient& pg,
                                     const SynonymDict& syn, const std::string& collection,
                                     int per_path_k, int top_k) {
    QueryAnalysis qa = analyze_query(question);

    RetrievalFilter filter;   // 默认 status==现行
    if (!qa.standard_code.empty()) {
        std::string sid = pg.find_standard_by_code(qa.standard_code);
        if (!sid.empty()) filter.standard_id = sid;
        else spdlog::warn("查询提到标准号 {} 但库中未找到，回退全库检索", qa.standard_code);
    }

    // dense 路 + BM25 路（均必跑）+ 方法号路（有方法号才跑）
    std::vector<std::vector<Candidate>> lists;
    DenseRetriever dense(mv, embed, collection);
    Bm25Retriever bm25(mv, syn, collection);
    lists.push_back(dense.retrieve(qa.clean_text, filter, per_path_k));
    lists.push_back(bm25.retrieve(qa.clean_text, filter, per_path_k));
    if (!qa.method_no.empty()) {
        PgExactRetriever exact(pg, qa.method_no);
        lists.push_back(exact.retrieve(qa.clean_text, filter, per_path_k));
    }

    std::vector<Candidate> fused = rrf_fuse(lists, /*k=*/60, top_k);

    if (!qa.clause_no.empty()) {
        std::vector<std::string> pinned = pg.chunk_ids_by_clause(qa.clause_no, filter.standard_id);
        fused = pin_exact_clause(fused, pinned, top_k);
    }
    return fused;
}
```

- [x] **Step 3: `src/generate/answer_pipeline.h`** — thread `const SynonymDict&` into `answer_query`:

Add `#include "query/synonyms.h"` and change the `answer_query` declaration to add `const SynonymDict& syn` (place it after `pg`):

```cpp
std::string answer_query(const std::string& question,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         PgClient& pg,
                         const SynonymDict& syn,
                         deepseek::DeepSeekClient& ds,
                         const std::string& collection,
                         int top_k);
```

(`fragment_from_chunk` unchanged.)

- [x] **Step 4: `src/generate/answer_pipeline.cpp`** — pass `syn` to `text_retrieve`:

Change the `answer_query` signature to match the header, and the `text_retrieve(...)` call:

```cpp
std::string answer_query(const std::string& question, milvus::MilvusRest& mv,
                         EmbeddingClient& embed, PgClient& pg, const SynonymDict& syn,
                         deepseek::DeepSeekClient& ds, const std::string& collection,
                         int top_k) {
    auto candidates = text_retrieve(question, mv, embed, pg, syn, collection,
                                    /*per_path_k=*/top_k * 4, top_k);
    // ... rest unchanged ...
```

Keep the rest of the function body (the fragment-assembly loop and refusal strings) exactly as it is.

- [x] **Step 5: `src/main.cpp` cmd_query** — load synonyms and pass:

```cpp
static int cmd_query(const Config& cfg, const std::string& question) {
    try {
        PgClient pg(cfg.pg_conninfo);
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                    cfg.deepseek_model, cfg.deepseek_key);
        SynonymDict syn;
        syn.load_from_file("config/synonyms.txt");

        std::string ans = answer_query(question, mv, embed, pg, syn, ds,
                                       cfg.milvus_collection, /*top_k=*/5);
        std::cout << "\n===== 回答 =====\n" << ans << "\n";
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] query 失败: {}", e.what());
        return 1;
    }
}
```

- [x] **Step 6: Register tests-project link.** The tests project compiles `answer_pipeline.cpp` (via `test_context.cpp`) → now pulls `text_search.cpp` → `bm25_retriever.cpp`. Add to `rag2.0.tests/rag2.0.tests.vcxproj`:

```xml
<ClCompile Include="..\src\retrieve\bm25_retriever.cpp" />
<ClCompile Include="..\src\query\synonyms.cpp" />
```

(`synonyms.cpp` was already added to the tests project in Task 1; only add it here if it is not already present — check first. `text_search.cpp`, `pg_exact_retriever.cpp`, `dense_retriever.cpp` are already in the tests project from M3a.)

- [x] **Step 7: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: build succeeds, **131** (fragment_from_chunk tests still pass; signature change to answer_query doesn't touch them). If the tests binary fails to LINK (unresolved `Bm25Retriever`/`text_retrieve`), add the missing `.cpp` to the tests project and rebuild; report which.

- [x] **Step 8: Commit**

```powershell
git add src/retrieve/text_search.h src/retrieve/text_search.cpp src/generate/answer_pipeline.h src/generate/answer_pipeline.cpp src/main.cpp rag2.0.tests/rag2.0.tests.vcxproj
git commit -m @'
feat(m3b): add bm25 path to retrieval orchestration

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 7: Live Rebuild And Verification

**Files:** none (operational + verification). This is where the full-text collection is actually created and the Milvus schema JSON is confirmed.

- [x] **Step 1: Confirm services up**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe smoke
```
If Milvus/PG/embedding/DeepSeek are not all up, STOP and report "live rebuild skipped: services unavailable". (If Milvus is down, start the Docker containers `milvus-etcd`, `milvus-minio`, then `milvus-standalone` and wait until `smoke` shows `[OK] Milvus`.)

- [x] **Step 2: Drop the old (M3a, 4-field) collection — one-time migration**

```powershell
$cfg = Get-Content config.json -Raw | ConvertFrom-Json
$headers = @{ Authorization = "Bearer $($cfg.RAG_MILVUS_TOKEN)" }
Invoke-RestMethod -Method Post -Uri "$($cfg.RAG_MILVUS_BASE_URL)/v2/vectordb/collections/drop" `
  -Headers $headers -ContentType "application/json" `
  -Body (@{ collectionName = $cfg.RAG_MILVUS_COLLECTION } | ConvertTo-Json)
```
Expected: `code: 0`.

- [x] **Step 3: Rechunkload both standards (creates the full-text collection on first call)**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\12215131224082667446.json
.\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\5797633264338373449.json
```
Expected: each prints `chunks=N embedded=N deleted_old=0`, exit 0.

**If the first chunkload throws a Milvus create-collection schema error**, the full-text JSON in `build_text_collection_body` needs adjustment for this Milvus 2.6 patch. Read the error, adjust the analyzer_params / functions / sparse-index shape in `src/milvus/milvus_rest.cpp`, update the `build_text_collection_body` body test in `tests/test_milvus_body.cpp` to match, rebuild, and retry from Step 2. Report the adjustment in the task summary and commit it as `fix(m3b): adjust full-text collection schema for milvus 2.6`.

- [x] **Step 4: BM25 / synonym verification**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe query "水泥净浆稠度怎么测"
.\rag2.0\x64\Debug\rag2.0.exe query "T0517 需要哪些仪具"
```
Expected: terminology queries return relevant 试验规程 clauses (BM25 keyword path contributing). Report the answers verbatim. (Note: the corpus holds JTC 5210-2018 + JTG 3420-2020; pick queries whose answer is actually in-corpus — cement-paste/method queries are in JTG 3420.)

- [x] **Step 5: M3a five-scenario regression (must still hold)**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe query "JTC 5210 第5.1.2条是什么规定"
.\rag2.0\x64\Debug\rag2.0.exe query "T0517 需要哪些仪具"
.\rag2.0\x64\Debug\rag2.0.exe query "JTG 3420 里水泥怎么取样"
.\rag2.0\x64\Debug\rag2.0.exe query "路基沉降怎么评定"
.\rag2.0\x64\Debug\rag2.0.exe query "JTG 9999 的规定是什么"
```
Expected: clause pin / method path / standard narrowing / plain-dense / warn+fallback all still behave as in M3a. Report verbatim.

- [x] **Step 6: status filter pipeline check (optional, manual)**

Set one standard to 作废 in PG and rechunkload it, then query one of its clauses; default query should no longer return it. This validates the pipeline (not M6's full lifecycle). If skipped, note it as pending manual verification.

```powershell
# (optional) example — adjust standard_id:
# UPDATE standards SET status='作废' WHERE standard_id='5797633264338373449';
# .\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\5797633264338373449.json
# .\rag2.0\x64\Debug\rag2.0.exe query "水泥怎么取样"   # 应不再命中该标准
# 验证后恢复： UPDATE standards SET status='现行' WHERE ...; 再 chunkload 还原
```

- [x] **Step 7: Final build + full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
git status --short
```
Expected: build green, full suite green (131, or the adjusted count if Step 3 changed a body test), only intentional M3b files changed, `logs/` + data caches untracked.

- [x] **Step 8: Commit any schema adjustment from Step 3** (if not already committed there). Otherwise nothing to commit in this task.

---

## Final Verification

```powershell
git status --short
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: MSBuild succeeds; all doctest pass; BM25 path live-verified; M3a regression intact.

---

## Self-Review Checklist

- [x] Spec §2.1 BM25 via Milvus + C++ RRF: Tasks 3/4/6.
- [x] Spec §2.2 collection rebuild: Task 7 (drop + ensure_collection_text + rechunkload).
- [x] Spec §2.3 text = embedding_text: Task 5 (`insert_full(..., c.embedding_text, ...)`).
- [x] Spec §2.4 status from standards.status: Task 5 (cmd_chunkload reads `get_standard()->status`; ingest passes `s.status`).
- [x] Spec §2.5 synonyms BM25-only, query-side: Task 4 (`syn_.expand` only in Bm25Retriever; dense uses raw `clean_text`).
- [x] Spec §3 full-text schema (status/text/sparse/BM25 Function/analyzer): Task 3 `build_text_collection_body`.
- [x] Spec §4 dictionaries: `config/synonyms.txt` (Task 1), `config/user_dict.txt` (Task 5).
- [x] Spec §5.1 RetrievalFilter status: Task 2.
- [x] Spec §5.2–5.3 SynonymDict + Bm25Retriever: Tasks 1/4.
- [x] Spec §5.4 Milvus methods: Task 3.
- [x] Spec §5.5 orchestration BM25 path: Task 6.
- [x] Spec §5.6–5.7 loader/ingest/command wiring: Task 5 + Task 6.
- [x] Spec §8 tests: SynonymDict (T1), to_milvus_expr status incl. updated M3a asserts (T2), insert_full/bm25/collection bodies (T3).
- [x] Spec §7 non-goals untouched: no access_level, no rerank, no Milvus-side hybrid, no eval tuning, no bm25_text column.
- [x] Type consistency: `RetrievalFilter{standard_id,status}`, `load_chunks(...,status)`, `ingest_file(...,user_dict,cache_dir)`, `text_retrieve(...,syn,...)`, `answer_query(...,syn,...)`, `Bm25Retriever`, `build_insert_full_body`/`build_bm25_body`/`build_text_collection_body`/`ensure_collection_text`/`insert_full`/`search_bm25` — consistent across tasks.
```
