# M2c-2 Retrieval Chunks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the pure C++ M2c-2 conversion layer that reads a `ClauseTree`, produces controlled retrieval chunks with `atomic_text`, `embedding_text`, `context_text`, and writes `data/chunk_cache/<id>.json`.

**Architecture:** Add a focused `src/retrieve/retrieval_chunk.{h,cpp}` module that consumes M2c-1 `ClauseTree` data without touching parsing, PostgreSQL, Milvus, or embedding APIs. The module emits `RetrievalChunkCache` JSON with dense-safe `embedding_text` and locator metadata (`standard_no`, `method_no`, `clause_no`, `path_text`, pages) kept out of dense text. Add a `chunkcheck` CLI command to inspect and cache the generated chunks.

**Tech Stack:** C++20, doctest, nlohmann/json, spdlog, MSBuild/vcpkg, existing Visual Studio `.vcxproj` projects.

---

## Engineering Notes

Work in `D:\vs2022 code\rag2.0`.

Before implementing, run:

```powershell
git status --short
```

Expected currently: there may be unrelated M2c-1 edits in `src/parse/*`, `src/structure/tree_builder.cpp`, and `tests/test_*`. Do not revert them. This plan only edits files listed in each task.

Build command:

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Run all tests:

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Run one doctest case:

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval_chunk JSON round trip preserves cache and chunk fields"
```

---

## File Structure

Create:

| File | Responsibility |
|---|---|
| `src/retrieve/retrieval_chunk.h` | `RetrievalChunk`, `RetrievalChunkCache`, and public function declarations |
| `src/retrieve/retrieval_chunk.cpp` | Chunk generation, text composition, JSON serialization, cache writing |
| `tests/test_retrieval_chunk.cpp` | Unit tests for JSON, controlled dense text, metadata, T methods, context |

Modify:

| File | Responsibility |
|---|---|
| `src/main.cpp` | Add `chunkcheck <tree_cache.json>` command |
| `rag2.0/rag2.0.vcxproj` | Compile `retrieval_chunk.cpp`, include `retrieval_chunk.h` |
| `rag2.0/rag2.0.vcxproj.filters` | Show new source/header under retrieve filters |
| `rag2.0.tests/rag2.0.tests.vcxproj` | Compile module and `test_retrieval_chunk.cpp` |
| `rag2.0.tests/rag2.0.tests.vcxproj.filters` | Show new test/source/header in VS filters |

---

## Task 1: Data Model And JSON Round Trip

**Files:**
- Create: `src/retrieve/retrieval_chunk.h`
- Create: `src/retrieve/retrieval_chunk.cpp`
- Create: `tests/test_retrieval_chunk.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`
- Modify: `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing JSON round-trip test**

Create `tests/test_retrieval_chunk.cpp`:

```cpp
#include <doctest/doctest.h>
#include "retrieve/retrieval_chunk.h"

TEST_CASE("retrieval_chunk JSON round trip preserves cache and chunk fields") {
    RetrievalChunkCache cache;
    cache.standard_id = "sid";
    cache.standard_no = "JTC 5210-2018";

    RetrievalChunk c;
    c.chunk_id = "sid:5/5.1/5.1.2#main";
    c.node_id = "sid:5/5.1/5.1.2";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5.1.2";
    c.method_no = "";
    c.title = "路基沉降";
    c.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    c.atomic_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.embedding_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。\n相关图表题：\n图5.1.2 路基沉降示意图";
    c.context_text = "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.captions = {"图5.1.2 路基沉降示意图"};
    c.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    c.page_start = 12;
    c.page_end = 13;
    c.has_table = true;
    c.has_formula = true;
    c.has_figure = true;
    c.suspect = "seq";
    cache.chunks.push_back(c);

    RetrievalChunkCache round_trip =
        retrieval_chunk_cache_from_json(retrieval_chunk_cache_to_json(cache));

    CHECK(round_trip.schema_version == 1);
    CHECK(round_trip.standard_id == "sid");
    CHECK(round_trip.standard_no == "JTC 5210-2018");
    REQUIRE(round_trip.chunks.size() == 1);

    const RetrievalChunk& r = round_trip.chunks[0];
    CHECK(r.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(r.node_id == "sid:5/5.1/5.1.2");
    CHECK(r.standard_id == "sid");
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.chunk_type == "body");
    CHECK(r.clause_no == "5.1.2");
    CHECK(r.method_no == "");
    CHECK(r.title == "路基沉降");
    CHECK(r.path_text.find("5.1 路基") != std::string::npos);
    CHECK(r.atomic_text.find("路基沉降") != std::string::npos);
    CHECK(r.embedding_text.find("图5.1.2") != std::string::npos);
    CHECK(r.context_text.find("路径：") != std::string::npos);
    REQUIRE(r.captions.size() == 1);
    CHECK(r.captions[0] == "图5.1.2 路基沉降示意图");
    REQUIRE(r.formulas.size() == 1);
    CHECK(r.formulas[0] == "MQI = SCI + PQI + BCI + TCI");
    CHECK(r.page_start == 12);
    CHECK(r.page_end == 13);
    CHECK(r.has_table);
    CHECK(r.has_formula);
    CHECK(r.has_figure);
    CHECK(r.suspect == "seq");
}
```

- [x] **Step 2: Add project entries before running the test**

In `rag2.0/rag2.0.vcxproj`, add:

```xml
<ClCompile Include="..\src\retrieve\retrieval_chunk.cpp" />
```

near the other `src\retrieve` source entries, and:

```xml
<ClInclude Include="..\src\retrieve\retrieval_chunk.h" />
```

near the other `src\retrieve` headers.

In `rag2.0.tests/rag2.0.tests.vcxproj`, add:

```xml
<ClCompile Include="..\tests\test_retrieval_chunk.cpp" />
<ClCompile Include="..\src\retrieve\retrieval_chunk.cpp" />
```

and:

```xml
<ClInclude Include="..\src\retrieve\retrieval_chunk.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`, add:

```xml
<ClCompile Include="..\src\retrieve\retrieval_chunk.cpp"><Filter>源文件\retrieve</Filter></ClCompile>
```

and:

```xml
<ClInclude Include="..\src\retrieve\retrieval_chunk.h"><Filter>头文件\retrieve</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters`, add:

```xml
<ClCompile Include="..\tests\test_retrieval_chunk.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\retrieve\retrieval_chunk.cpp"><Filter>被测源码</Filter></ClCompile>
```

and:

```xml
<ClInclude Include="..\src\retrieve\retrieval_chunk.h"><Filter>头文件</Filter></ClInclude>
```

- [x] **Step 3: Run the test and verify it fails**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build fails because `retrieve/retrieval_chunk.h` does not exist.

- [x] **Step 4: Create the header**

Create `src/retrieve/retrieval_chunk.h`:

```cpp
#pragma once

#include "structure/clause_tree.h"

#include <string>
#include <vector>

struct RetrievalChunk {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    std::string standard_no;
    std::string chunk_type;

    std::string clause_no;
    std::string method_no;
    std::string title;
    std::string path_text;

    std::string atomic_text;
    std::string embedding_text;
    std::string context_text;

    std::vector<std::string> captions;
    std::vector<std::string> formulas;
    int page_start = 0;
    int page_end = 0;

    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::string suspect;
};

struct RetrievalChunkCache {
    int schema_version = 1;
    std::string standard_id;
    std::string standard_no;
    std::vector<RetrievalChunk> chunks;
};

RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree);
std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache);
RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text);
void write_chunk_cache(const std::string& cache_path, const RetrievalChunkCache& cache);
```

- [x] **Step 5: Create the minimal implementation for JSON and cache writing**

Create `src/retrieve/retrieval_chunk.cpp`:

```cpp
#include "retrieve/retrieval_chunk.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree) {
    RetrievalChunkCache cache;
    cache.standard_id = tree.standard_id;
    cache.standard_no = tree.standard_no;
    return cache;
}

std::string retrieval_chunk_cache_to_json(const RetrievalChunkCache& cache) {
    json j;
    j["schema_version"] = cache.schema_version;
    j["standard_id"] = cache.standard_id;
    j["standard_no"] = cache.standard_no;
    j["chunks"] = json::array();

    for (const auto& c : cache.chunks) {
        j["chunks"].push_back({
            {"chunk_id", c.chunk_id},
            {"node_id", c.node_id},
            {"standard_id", c.standard_id},
            {"standard_no", c.standard_no},
            {"chunk_type", c.chunk_type},
            {"clause_no", c.clause_no},
            {"method_no", c.method_no},
            {"title", c.title},
            {"path_text", c.path_text},
            {"atomic_text", c.atomic_text},
            {"embedding_text", c.embedding_text},
            {"context_text", c.context_text},
            {"captions", c.captions},
            {"formulas", c.formulas},
            {"page_start", c.page_start},
            {"page_end", c.page_end},
            {"has_table", c.has_table},
            {"has_formula", c.has_formula},
            {"has_figure", c.has_figure},
            {"suspect", c.suspect},
        });
    }

    return j.dump(2);
}

RetrievalChunkCache retrieval_chunk_cache_from_json(const std::string& json_text) {
    RetrievalChunkCache cache;
    auto j = json::parse(json_text, nullptr, false);
    if (!j.is_object()) return cache;

    cache.schema_version = j.value("schema_version", 1);
    cache.standard_id = j.value("standard_id", "");
    cache.standard_no = j.value("standard_no", "");

    if (!j.contains("chunks") || !j["chunks"].is_array()) return cache;

    for (const auto& item : j["chunks"]) {
        RetrievalChunk c;
        c.chunk_id = item.value("chunk_id", "");
        c.node_id = item.value("node_id", "");
        c.standard_id = item.value("standard_id", "");
        c.standard_no = item.value("standard_no", "");
        c.chunk_type = item.value("chunk_type", "");
        c.clause_no = item.value("clause_no", "");
        c.method_no = item.value("method_no", "");
        c.title = item.value("title", "");
        c.path_text = item.value("path_text", "");
        c.atomic_text = item.value("atomic_text", "");
        c.embedding_text = item.value("embedding_text", "");
        c.context_text = item.value("context_text", "");
        c.page_start = item.value("page_start", 0);
        c.page_end = item.value("page_end", 0);
        c.has_table = item.value("has_table", false);
        c.has_formula = item.value("has_formula", false);
        c.has_figure = item.value("has_figure", false);
        c.suspect = item.value("suspect", "");

        if (item.contains("captions") && item["captions"].is_array()) {
            for (const auto& v : item["captions"]) {
                if (v.is_string()) c.captions.push_back(v.get<std::string>());
            }
        }
        if (item.contains("formulas") && item["formulas"].is_array()) {
            for (const auto& v : item["formulas"]) {
                if (v.is_string()) c.formulas.push_back(v.get<std::string>());
            }
        }

        cache.chunks.push_back(std::move(c));
    }

    return cache;
}

void write_chunk_cache(const std::string& cache_path, const RetrievalChunkCache& cache) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());

    std::ofstream f(cache_path, std::ios::binary);
    if (!f) {
        spdlog::warn("chunk cache write failed: {}", cache_path);
        return;
    }
    f << retrieval_chunk_cache_to_json(cache);
}
```

- [x] **Step 6: Build and run the JSON test**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval_chunk JSON round trip preserves cache and chunk fields"
```

Expected: PASS.

- [x] **Step 7: Commit Task 1**

```powershell
git add src/retrieve/retrieval_chunk.h src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2c2): add retrieval chunk cache model and json"
```

---

## Task 2: Build Chunks For Leaf Nodes With Controlled Text

**Files:**
- Modify: `tests/test_retrieval_chunk.cpp`
- Modify: `src/retrieve/retrieval_chunk.cpp`

- [x] **Step 1: Add the failing leaf-generation test**

Append to `tests/test_retrieval_chunk.cpp`:

```cpp
static TreeNode make_node(const std::string& id, const std::string& number,
                          const std::string& title, const std::string& text,
                          bool leaf, const std::string& parent = "") {
    TreeNode n;
    n.node_id = id;
    n.number = number;
    n.title = title;
    n.text = text;
    n.is_leaf = leaf;
    n.parent_id = parent;
    n.page_start = 10;
    n.page_end = 11;
    return n;
}

TEST_CASE("build_retrieval_chunk_cache creates one dense-safe chunk per leaf") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "JTC 5210-2018";
    tree.format_profile = "A_decimal";

    TreeNode root = make_node("sid:5", "5", "技术状况评定", "", false);
    root.child_ids = {"sid:5/5.1"};

    TreeNode parent = make_node("sid:5/5.1", "5.1", "路基", "", false, "sid:5");
    parent.child_ids = {"sid:5/5.1/5.1.2"};

    TreeNode leaf = make_node("sid:5/5.1/5.1.2", "5.1.2", "路基沉降",
                              "路基沉降应根据沉降深度和影响范围评定。", true, "sid:5/5.1");
    leaf.captions = {"图5.1.2 路基沉降示意图"};
    leaf.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    leaf.has_figure = true;
    leaf.has_formula = true;
    leaf.has_table = true;

    tree.nodes = {root, parent, leaf};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

    CHECK(cache.standard_id == "sid");
    CHECK(cache.standard_no == "JTC 5210-2018");
    REQUIRE(cache.chunks.size() == 1);

    const RetrievalChunk& c = cache.chunks[0];
    CHECK(c.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(c.node_id == "sid:5/5.1/5.1.2");
    CHECK(c.standard_id == "sid");
    CHECK(c.standard_no == "JTC 5210-2018");
    CHECK(c.chunk_type == "body");
    CHECK(c.clause_no == "5.1.2");
    CHECK(c.title == "路基沉降");
    CHECK(c.path_text == "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降");

    CHECK(c.atomic_text.find("5.1.2 路基沉降") != std::string::npos);
    CHECK(c.atomic_text.find("路基沉降应根据沉降深度") != std::string::npos);
    CHECK(c.atomic_text.find("MQI = SCI") != std::string::npos);
    CHECK(c.atomic_text.find("图5.1.2") == std::string::npos);
    CHECK(c.atomic_text.find("JTC 5210") == std::string::npos);
    CHECK(c.atomic_text.find("技术状况评定 >") == std::string::npos);

    CHECK(c.embedding_text.find("5.1.2 路基沉降") != std::string::npos);
    CHECK(c.embedding_text.find("路基沉降应根据沉降深度") != std::string::npos);
    CHECK(c.embedding_text.find("图5.1.2 路基沉降示意图") != std::string::npos);
    CHECK(c.embedding_text.find("MQI = SCI") != std::string::npos);
    CHECK(c.embedding_text.find("JTC 5210") == std::string::npos);
    CHECK(c.embedding_text.find("技术状况评定 >") == std::string::npos);
    CHECK(c.embedding_text.find("<table") == std::string::npos);

    CHECK(c.page_start == 10);
    CHECK(c.page_end == 11);
    CHECK(c.has_table);
    CHECK(c.has_formula);
    CHECK(c.has_figure);
    REQUIRE(c.captions.size() == 1);
    CHECK(c.captions[0] == "图5.1.2 路基沉降示意图");
    REQUIRE(c.formulas.size() == 1);
    CHECK(c.formulas[0] == "MQI = SCI + PQI + BCI + TCI");
}
```

- [x] **Step 2: Run the test and verify it fails**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_retrieval_chunk_cache creates one dense-safe chunk per leaf"
```

Expected: FAIL because `cache.chunks.size()` is `0`.

- [x] **Step 3: Implement leaf chunk generation and text composition**

Replace the top of `src/retrieve/retrieval_chunk.cpp` so the file includes these helpers before `build_retrieval_chunk_cache`:

```cpp
#include <algorithm>
#include <map>
#include <sstream>
```

Add these helpers inside an anonymous namespace:

```cpp
namespace {

std::string trim_ascii_space(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) ++begin;
    size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) --end;
    return s.substr(begin, end - begin);
}

void append_line(std::string& out, const std::string& line) {
    std::string text = trim_ascii_space(line);
    if (text.empty()) return;
    if (!out.empty()) out += "\n";
    out += text;
}

void append_section(std::string& out, const std::string& heading, const std::vector<std::string>& lines,
                    size_t max_count, size_t max_chars) {
    std::vector<std::string> kept;
    size_t chars = 0;
    for (const auto& raw : lines) {
        std::string line = trim_ascii_space(raw);
        if (line.empty()) continue;
        if (std::find(kept.begin(), kept.end(), line) != kept.end()) continue;
        if (kept.size() >= max_count) break;
        if (chars + line.size() > max_chars) break;
        chars += line.size();
        kept.push_back(line);
    }
    if (kept.empty()) return;
    if (!out.empty()) out += "\n\n";
    out += heading;
    for (const auto& line : kept) {
        out += "\n";
        out += line;
    }
}

std::string node_label(const TreeNode& n) {
    std::string label;
    append_line(label, n.number + (n.title.empty() ? "" : " " + n.title));
    if (label.empty()) append_line(label, n.title);
    if (label.empty()) append_line(label, n.number);
    return label;
}

std::map<std::string, const TreeNode*> index_nodes(const ClauseTree& tree) {
    std::map<std::string, const TreeNode*> by_id;
    for (const auto& n : tree.nodes) by_id[n.node_id] = &n;
    return by_id;
}

std::vector<const TreeNode*> ancestor_chain(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::vector<const TreeNode*> reversed;
    std::string current = n.parent_id;
    while (!current.empty()) {
        auto it = by_id.find(current);
        if (it == by_id.end()) break;
        reversed.push_back(it->second);
        current = it->second->parent_id;
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::string path_text_for(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::vector<std::string> labels;
    for (const TreeNode* parent : ancestor_chain(n, by_id)) {
        std::string label = node_label(*parent);
        if (!label.empty()) labels.push_back(label);
    }
    std::string self = node_label(n);
    if (!self.empty()) labels.push_back(self);

    std::string out;
    for (const auto& label : labels) {
        if (!out.empty()) out += " > ";
        out += label;
    }
    return out;
}

std::string compose_atomic_text(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    append_section(out, "公式：", n.formulas, 5, 1200);
    return out;
}

std::string compose_embedding_text(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    append_section(out, "相关图表题：", n.captions, 3, 1200);
    append_section(out, "公式：", n.formulas, 5, 1200);
    return out;
}

std::string chunk_type_for(const TreeNode& n) {
    if (n.node_id.find(":appendix:") != std::string::npos) return "appendix";
    if (n.node_id.find(":explanation:") != std::string::npos) return "explanation";
    return "body";
}

RetrievalChunk chunk_from_leaf(const ClauseTree& tree, const TreeNode& n,
                               const std::map<std::string, const TreeNode*>& by_id) {
    RetrievalChunk c;
    c.chunk_id = n.node_id + "#main";
    c.node_id = n.node_id;
    c.standard_id = tree.standard_id;
    c.standard_no = tree.standard_no;
    c.chunk_type = chunk_type_for(n);
    c.clause_no = n.number;
    c.title = n.title;
    c.path_text = path_text_for(n, by_id);
    c.atomic_text = compose_atomic_text(n);
    c.embedding_text = compose_embedding_text(n);
    c.context_text = c.atomic_text;
    c.captions = n.captions;
    c.formulas = n.formulas;
    c.page_start = n.page_start;
    c.page_end = n.page_end;
    c.has_table = n.has_table;
    c.has_formula = n.has_formula;
    c.has_figure = n.has_figure;
    c.suspect = n.suspect;
    return c;
}

}  // namespace
```

Replace `build_retrieval_chunk_cache` with:

```cpp
RetrievalChunkCache build_retrieval_chunk_cache(const ClauseTree& tree) {
    RetrievalChunkCache cache;
    cache.standard_id = tree.standard_id;
    cache.standard_no = tree.standard_no;

    auto by_id = index_nodes(tree);
    for (const auto& n : tree.nodes) {
        if (!n.is_leaf) continue;
        cache.chunks.push_back(chunk_from_leaf(tree, n, by_id));
    }

    return cache;
}
```

- [x] **Step 4: Build and run the leaf-generation test**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_retrieval_chunk_cache creates one dense-safe chunk per leaf"
```

Expected: PASS.

- [x] **Step 5: Commit Task 2**

```powershell
git add src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
git commit -m "feat(m2c2): build controlled chunks from leaf nodes"
```

---

## Task 3: T Method Locator And Chunk Type

**Files:**
- Modify: `tests/test_retrieval_chunk.cpp`
- Modify: `src/retrieve/retrieval_chunk.cpp`

- [x] **Step 1: Add the failing T-method test**

Append to `tests/test_retrieval_chunk.cpp`:

```cpp
TEST_CASE("retrieval chunks keep T method numbers as metadata outside embedding text") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "JTG 3432-2024";
    tree.format_profile = "B_testno";

    TreeNode chapter = make_node("sid:4", "4", "集料试验", "", false);
    chapter.child_ids = {"sid:4/T0302-2024"};

    TreeNode method = make_node("sid:4/T0302-2024", "T 0302-2024", "集料筛分试验", "", false, "sid:4");
    method.child_ids = {"sid:4/T0302-2024/2"};

    TreeNode leaf = make_node("sid:4/T0302-2024/2", "2", "仪具与材料",
                              "天平、标准筛、烘箱。", true, "sid:4/T0302-2024");

    tree.nodes = {chapter, method, leaf};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

    REQUIRE(cache.chunks.size() == 1);
    const RetrievalChunk& c = cache.chunks[0];
    CHECK(c.method_no == "T0302-2024");
    CHECK(c.path_text == "4 集料试验 > T 0302-2024 集料筛分试验 > 2 仪具与材料");
    CHECK(c.embedding_text.find("仪具与材料") != std::string::npos);
    CHECK(c.embedding_text.find("天平") != std::string::npos);
    CHECK(c.embedding_text.find("T0302") == std::string::npos);
    CHECK(c.embedding_text.find("T 0302") == std::string::npos);
    CHECK(c.embedding_text.find("集料筛分试验") == std::string::npos);
    CHECK(c.embedding_text.find("JTG 3432") == std::string::npos);
}

TEST_CASE("retrieval chunks classify appendix and explanation chunks from node id") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "JTC 5210-2018";
    tree.format_profile = "A_decimal";

    TreeNode appendix = make_node("sid:appendix:B/B.0.1", "B.0.1", "路面跳车计算方法",
                                  "路面跳车应根据纵断面高差确定。", true);
    TreeNode explanation = make_node("sid:explanation:5/5.1/5.1.2", "5.1.2", "条文说明",
                                     "本条说明路基沉降评定依据。", true);
    tree.nodes = {appendix, explanation};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

    REQUIRE(cache.chunks.size() == 2);
    CHECK(cache.chunks[0].chunk_type == "appendix");
    CHECK(cache.chunks[1].chunk_type == "explanation");
}
```

- [x] **Step 2: Run the T-method tests and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval chunks keep T method numbers as metadata outside embedding text"
```

Expected: FAIL because `method_no` is empty.

- [x] **Step 3: Implement method number extraction**

In `src/retrieve/retrieval_chunk.cpp`, add:

```cpp
#include <cctype>
#include <regex>
```

Inside the anonymous namespace add:

```cpp
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

std::string extract_method_no_from_text(const std::string& s) {
    static const std::regex re(R"(T\s*\d{4}\s*-\s*\d{4})");
    std::smatch m;
    std::string normalized = normalize_dashes(s);
    if (!std::regex_search(normalized, m, re)) return "";
    return remove_ascii_spaces(m.str(0));
}

std::string method_no_for(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    std::string self = extract_method_no_from_text(n.number + " " + n.title + " " + n.node_id);
    if (!self.empty()) return self;
    std::vector<const TreeNode*> ancestors = ancestor_chain(n, by_id);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        std::string found = extract_method_no_from_text((*it)->number + " " + (*it)->title + " " + (*it)->node_id);
        if (!found.empty()) return found;
    }
    return "";
}
```

In `chunk_from_leaf`, set:

```cpp
c.method_no = method_no_for(n, by_id);
```

after `c.clause_no = n.number;`.

- [x] **Step 4: Build and run the T-method and type tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval chunks keep T method numbers as metadata outside embedding text"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval chunks classify appendix and explanation chunks from node id"
```

Expected: PASS.

- [x] **Step 5: Commit Task 3**

```powershell
git add src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
git commit -m "feat(m2c2): keep method numbers as locator metadata"
```

---

## Task 4: Context Text Small-To-Big

**Files:**
- Modify: `tests/test_retrieval_chunk.cpp`
- Modify: `src/retrieve/retrieval_chunk.cpp`

- [x] **Step 1: Add the failing context test**

Append to `tests/test_retrieval_chunk.cpp`:

```cpp
TEST_CASE("retrieval chunks build context from the parent leaf group") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "JTC 5210-2018";
    tree.format_profile = "A_decimal";

    TreeNode chapter = make_node("sid:5", "5", "技术状况评定", "", false);
    chapter.child_ids = {"sid:5/5.1"};

    TreeNode parent = make_node("sid:5/5.1", "5.1", "路基", "", false, "sid:5");
    parent.child_ids = {"sid:5/5.1/5.1.1", "sid:5/5.1/5.1.2", "sid:5/5.1/5.1.3"};

    TreeNode before = make_node("sid:5/5.1/5.1.1", "5.1.1", "损坏类型",
                                "路基损坏包括沉陷、坍塌、冲刷。", true, "sid:5/5.1");
    TreeNode current = make_node("sid:5/5.1/5.1.2", "5.1.2", "路基沉降",
                                 "路基沉降应根据沉降深度评定。", true, "sid:5/5.1");
    TreeNode after = make_node("sid:5/5.1/5.1.3", "5.1.3", "边坡坍塌",
                               "边坡坍塌应按坍塌规模评定。", true, "sid:5/5.1");

    tree.nodes = {chapter, parent, before, current, after};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

    REQUIRE(cache.chunks.size() == 3);
    const RetrievalChunk& c = cache.chunks[1];
    CHECK(c.node_id == "sid:5/5.1/5.1.2");
    CHECK(c.context_text.find("路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降") != std::string::npos);
    CHECK(c.context_text.find("5.1.1 损坏类型") != std::string::npos);
    CHECK(c.context_text.find("路基损坏包括沉陷") != std::string::npos);
    CHECK(c.context_text.find("5.1.2 路基沉降") != std::string::npos);
    CHECK(c.context_text.find("路基沉降应根据沉降深度") != std::string::npos);
    CHECK(c.context_text.find("5.1.3 边坡坍塌") != std::string::npos);
    CHECK(c.context_text.find("边坡坍塌应按坍塌规模") != std::string::npos);
    CHECK(c.embedding_text.find("5.1.1 损坏类型") == std::string::npos);
    CHECK(c.embedding_text.find("5.1.3 边坡坍塌") == std::string::npos);
}
```

- [x] **Step 2: Run the context test and verify it fails**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval chunks build context from the parent leaf group"
```

Expected: FAIL because `context_text` only equals the current chunk's atomic text.

- [x] **Step 3: Implement context collection**

Inside the anonymous namespace in `src/retrieve/retrieval_chunk.cpp`, add:

```cpp
std::string leaf_text_for_context(const TreeNode& n) {
    std::string out;
    append_line(out, node_label(n));
    append_line(out, n.text);
    return out;
}

void collect_leaf_descendants(const TreeNode& parent,
                              const std::map<std::string, const TreeNode*>& by_id,
                              std::vector<const TreeNode*>& out) {
    for (const auto& child_id : parent.child_ids) {
        auto it = by_id.find(child_id);
        if (it == by_id.end()) continue;
        const TreeNode* child = it->second;
        if (child->is_leaf) {
            out.push_back(child);
        } else {
            collect_leaf_descendants(*child, by_id, out);
        }
    }
}

std::vector<const TreeNode*> context_leaves_for(const TreeNode& n,
                                                const std::map<std::string, const TreeNode*>& by_id) {
    auto parent_it = by_id.find(n.parent_id);
    if (parent_it == by_id.end()) return {&n};

    std::vector<const TreeNode*> leaves;
    collect_leaf_descendants(*parent_it->second, by_id, leaves);
    if (leaves.empty()) return {&n};
    return leaves;
}

std::string compose_context_text(const TreeNode& n, const std::map<std::string, const TreeNode*>& by_id) {
    constexpr size_t max_chars = 6000;
    std::string out = "路径：" + path_text_for(n, by_id);

    std::vector<const TreeNode*> leaves = context_leaves_for(n, by_id);
    for (const TreeNode* leaf : leaves) {
        std::string part = leaf_text_for_context(*leaf);
        if (part.empty()) continue;
        if (out.size() + part.size() + 2 > max_chars && leaf->node_id != n.node_id) continue;
        out += "\n\n";
        out += part;
    }

    if (out.find(n.text) == std::string::npos) {
        std::string current = leaf_text_for_context(n);
        if (!current.empty()) {
            out += "\n\n";
            out += current;
        }
    }

    return out;
}
```

In `chunk_from_leaf`, replace:

```cpp
c.context_text = c.atomic_text;
```

with:

```cpp
c.context_text = compose_context_text(n, by_id);
```

- [x] **Step 4: Build and run the context test**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval chunks build context from the parent leaf group"
```

Expected: PASS.

- [x] **Step 5: Commit Task 4**

```powershell
git add src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
git commit -m "feat(m2c2): build small-to-big context text"
```

---

## Task 5: Chunkcheck CLI And Full Verification

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/test_retrieval_chunk.cpp`

- [x] **Step 1: Add a write-cache unit test**

Append to `tests/test_retrieval_chunk.cpp`:

```cpp
#include <filesystem>
#include <fstream>

TEST_CASE("write_chunk_cache creates parent directories and writes readable JSON") {
    RetrievalChunkCache cache;
    cache.standard_id = "sid";
    cache.standard_no = "JTC 5210-2018";

    RetrievalChunk c;
    c.chunk_id = "sid:5#main";
    c.node_id = "sid:5";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5";
    c.title = "技术状况评定";
    c.atomic_text = "5 技术状况评定\n正文";
    c.embedding_text = "5 技术状况评定\n正文";
    c.context_text = "路径：5 技术状况评定\n正文";
    cache.chunks.push_back(c);

    std::filesystem::path out = std::filesystem::temp_directory_path() /
        "rag2_m2c2_chunk_cache_test" / "sid.json";
    std::filesystem::remove(out);

    write_chunk_cache(out.string(), cache);

    REQUIRE(std::filesystem::exists(out));
    std::ifstream f(out, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    RetrievalChunkCache round_trip = retrieval_chunk_cache_from_json(ss.str());
    CHECK(round_trip.standard_id == "sid");
    REQUIRE(round_trip.chunks.size() == 1);
    CHECK(round_trip.chunks[0].chunk_id == "sid:5#main");

    std::filesystem::remove(out);
}
```

- [x] **Step 2: Run the write-cache test**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="write_chunk_cache creates parent directories and writes readable JSON"
```

Expected: PASS.

- [x] **Step 3: Add `chunkcheck` includes**

In `src/main.cpp`, add:

```cpp
#include "retrieve/retrieval_chunk.h"
```

near the other project includes.

- [x] **Step 4: Add the `cmd_chunkcheck` function**

In `src/main.cpp`, place this after `cmd_treecheck`:

```cpp
static int cmd_chunkcheck(const std::string& tree_cache_path) {
    try {
        if (!std::filesystem::exists(tree_cache_path)) {
            spdlog::error("树缓存文件不存在: {}", tree_cache_path);
            return 1;
        }

        std::string js = read_file(tree_cache_path);
        if (js.empty()) {
            spdlog::error("树缓存文件为空: {}", tree_cache_path);
            return 1;
        }

        ClauseTree tree = clause_tree_from_json(js);
        RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

        size_t atomic_chars = 0;
        size_t embedding_chars = 0;
        size_t context_chars = 0;
        int with_caption = 0;
        int with_formula = 0;
        int with_table = 0;
        int suspects = 0;
        int empty_embedding = 0;
        int long_embedding = 0;

        for (const auto& c : cache.chunks) {
            atomic_chars += c.atomic_text.size();
            embedding_chars += c.embedding_text.size();
            context_chars += c.context_text.size();
            if (!c.captions.empty()) ++with_caption;
            if (c.has_formula || !c.formulas.empty()) ++with_formula;
            if (c.has_table) ++with_table;
            if (!c.suspect.empty()) ++suspects;
            if (c.embedding_text.empty()) ++empty_embedding;
            if (c.embedding_text.size() > 2000) ++long_embedding;
        }

        size_t count = cache.chunks.size();
        auto avg = [count](size_t total) -> size_t {
            return count == 0 ? 0 : total / count;
        };

        std::string sid = tree.standard_id.empty() ? path_utf8::stem(tree_cache_path) : tree.standard_id;
        std::string out = "data/chunk_cache/" + sid + ".json";
        write_chunk_cache(out, cache);

        spdlog::info("chunkcheck {} | standard_no={} format={}", sid, tree.standard_no, tree.format_profile);
        spdlog::info("  chunks={}", count);
        spdlog::info("  avg_atomic_chars={}", avg(atomic_chars));
        spdlog::info("  avg_embedding_chars={}", avg(embedding_chars));
        spdlog::info("  avg_context_chars={}", avg(context_chars));
        spdlog::info("  with_caption={} with_formula={} with_table={}", with_caption, with_formula, with_table);
        spdlog::info("  suspect={} empty_embedding={} long_embedding_over_2000={}",
                     suspects, empty_embedding, long_embedding);
        spdlog::info("  已写 {}", out);
        return empty_embedding == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] chunkcheck: {}", e.what());
        return 1;
    }
}
```

- [x] **Step 5: Wire the command in `main`**

In the usage line, replace:

```cpp
std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck> [args]\n";
```

with:

```cpp
std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck> [args]\n";
```

After the `treecheck` command block, add:

```cpp
if (cmd == "chunkcheck") {
    if (argc < 3) {
        std::cout << "usage: rag2 chunkcheck <tree_cache.json>\n";
        return 1;
    }
    return cmd_chunkcheck(argv[2]);
}
```

- [x] **Step 6: Build and run all retrieval chunk tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="retrieval_chunk*"
```

Expected: all retrieval chunk tests pass.

- [x] **Step 7: Run full tests**

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: all tests pass. If unrelated M2c-1 dirty work has a failing test, stop and report the exact failing test instead of changing unrelated files.

- [x] **Step 8: Run CLI smoke if a tree cache exists**

Run:

```powershell
$treeCache = Get-ChildItem -Path data\tree_cache -Filter *.json | Select-Object -First 1
if ($null -ne $treeCache) {
    .\rag2.0\x64\Debug\rag2.0.exe chunkcheck $treeCache.FullName
} else {
    Write-Host "No data/tree_cache/*.json fixture exists for CLI smoke; unit tests and build passed."
}
```

Expected when a tree cache exists: logs show a positive `chunks` count, `empty_embedding=0`, and `已写 data/chunk_cache/<id>.json`.

- [x] **Step 9: Commit Task 5**

```powershell
git add src/main.cpp src/retrieve/retrieval_chunk.cpp src/retrieve/retrieval_chunk.h tests/test_retrieval_chunk.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2c2): add chunkcheck command and verification"
```

---

## Final Verification

Run:

```powershell
git status --short
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected:

```text
MSBuild succeeds.
All doctest tests pass.
Only intentional M2c-2 files are changed by this plan.
Existing unrelated dirty files are left untouched.
```

If `data/chunk_cache/<id>.json` was generated during CLI smoke, leave it untracked unless the user explicitly wants fixture data committed.

---

## Self-Review Checklist

- [x] Spec §1 goal is covered by Tasks 1-5.
- [x] Spec §2 controlled `embedding_text` is covered by Tasks 2-3.
- [x] Spec §3 data model and top-level cache JSON are covered by Task 1.
- [x] Spec §4 atomic/embedding/context generation is covered by Tasks 2 and 4.
- [x] Spec §5 examples are represented by the A_decimal and B_testno tests.
- [x] Spec §6 `chunkcheck` is covered by Task 5.
- [x] Spec §8 test strategy is mapped to doctest cases.
- [x] Spec §9 non-goals remain untouched: no PG, no Milvus, no embedding API, no ingest replacement.
