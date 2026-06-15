# retrievecheck Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `rag2 retrievecheck "问题" [k]` CLI command that runs the same three-way recall + RRF as `query` (window widened to k, default 20) and prints the fused top-k chunks as a ranked table with source tags and UTF-8-safe text snippets — without calling the LLM.

**Architecture:** A header-only `text_utf8::truncate` (mirrors existing `path_utf8.h`) provides safe Chinese snippet truncation; a new `cmd_retrievecheck` in `main.cpp` reuses the existing `text_retrieve` (no new retrieval logic) and prints the result. `query`/`answer_query`/`text_retrieve` are untouched.

**Tech Stack:** C++20, doctest, spdlog, MSBuild/vcpkg, Visual Studio `.vcxproj` projects.

Spec: `docs/superpowers/specs/2026-06-15-retrievecheck-command-design.md`

---

## Engineering Notes

Work in `D:\vs2022 code\rag2.0`, branch `V3.1`.

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
- Baseline before this plan: **131** doctest cases. Task 1 adds 5 → 136. Task 2 adds 0.
- `text_retrieve` lives in `src/retrieve/text_search.h`: `std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg, const SynonymDict& syn, const std::string& collection, int per_path_k, int top_k);`. `Candidate` (src/retrieve/candidate.h) has `std::string standard_id, chunk_id, source; float score;`.
- `PgClient::get_chunk(chunk_id)` returns `std::optional<RetrievalChunkRow>` with fields `chunk_id, node_id, standard_id, chunk_type, clause_no, method_no, title, path_text, atomic_text, embedding_text, context_text, ...`.
- `main.cpp` already includes `retrieve/dense_retriever.h`, `generate/answer_pipeline.h`, `query/synonyms.h`, `<iostream>`, `<string>`, `<filesystem>`, `<fstream>`. It has `read_file`, `read_dict_lines`, and `cmd_query`/`cmd_chunkload` for reference.
- Live verification (Task 2 Step 8) needs Milvus/PG/embedding up (`rag2 smoke` to check). If Milvus is down, start Docker containers `milvus-etcd`, `milvus-minio`, then `milvus-standalone`, wait for `[OK] Milvus`.

---

## File Structure

Create:

| File | Responsibility |
|---|---|
| `src/util/text_utf8.h` | header-only `text_utf8::truncate` (UTF-8 char-safe truncation) |
| `tests/test_text_utf8.cpp` | truncation unit tests |

Modify:

| File | Responsibility |
|---|---|
| `src/main.cpp` | `cmd_retrievecheck` + usage line + dispatch block; include text_utf8.h + text_search.h |
| `rag2.0/rag2.0.vcxproj` + `.filters` | register `text_utf8.h` (header) |
| `rag2.0.tests/rag2.0.tests.vcxproj` + `.filters` | register `test_text_utf8.cpp` |

(No `.cpp` for text_utf8 — header-only inline, like `path_utf8.h`. `text_search.cpp` is already compiled into the main project.)

---

## Task 1: UTF-8 Safe Truncation (pure, TDD)

**Files:**
- Create: `src/util/text_utf8.h`, `tests/test_text_utf8.cpp`
- Modify: `rag2.0/rag2.0.vcxproj` + `.filters`, `rag2.0.tests/rag2.0.tests.vcxproj` + `.filters`

- [x] **Step 1: Write `tests/test_text_utf8.cpp`**

```cpp
#include <doctest/doctest.h>
#include "util/text_utf8.h"

TEST_CASE("text_utf8 truncate leaves a short ascii string unchanged") {
    CHECK(text_utf8::truncate("hello", 10) == "hello");
}

TEST_CASE("text_utf8 truncate at exactly max_chars adds no ellipsis") {
    CHECK(text_utf8::truncate("hello", 5) == "hello");
}

TEST_CASE("text_utf8 truncate cuts ascii and appends an ellipsis") {
    CHECK(text_utf8::truncate("hello world", 5) == "hello…");
}

TEST_CASE("text_utf8 truncate counts chinese as one char each and never splits a byte") {
    // "天平砝码" = 4 个汉字，每个 3 字节。截到 2 个字符 → "天平…"
    std::string out = text_utf8::truncate("天平砝码", 2);
    CHECK(out == "天平…");
    // 截断点必须落在字符边界：再解析一遍各字符首字节都不是续字节
    for (size_t i = 0; i < out.size();) {
        unsigned char b = static_cast<unsigned char>(out[i]);
        CHECK((b & 0xC0) != 0x80);   // 不以续字节开头
        if (b < 0x80) i += 1;
        else if ((b >> 5) == 0x6) i += 2;
        else if ((b >> 4) == 0xE) i += 3;
        else i += 4;
    }
}

TEST_CASE("text_utf8 truncate returns empty for empty input") {
    CHECK(text_utf8::truncate("", 5).empty());
}
```

- [x] **Step 2: Register project files**

In `rag2.0/rag2.0.vcxproj`, near other `src\util` headers add:
```xml
<ClInclude Include="..\src\util\text_utf8.h" />
```
In `rag2.0/rag2.0.vcxproj.filters` add (mirror the filter used by `path_utf8.h` — find its entry and reuse the same `<Filter>`; if `path_utf8.h` is under `头文件\util`, use that; if it has no util filter, use `头文件`):
```xml
<ClInclude Include="..\src\util\text_utf8.h"><Filter>头文件\util</Filter></ClInclude>
```
In `rag2.0.tests/rag2.0.tests.vcxproj` add:
```xml
<ClCompile Include="..\tests\test_text_utf8.cpp" />
```
In `rag2.0.tests/rag2.0.tests.vcxproj.filters` add:
```xml
<ClCompile Include="..\tests\test_text_utf8.cpp"><Filter>测试</Filter></ClCompile>
```

> If `头文件\util` filter does not exist in `rag2.0.vcxproj.filters`, check what filter `..\src\util\path_utf8.h` uses and reuse it verbatim. Do not invent a new filter folder if path_utf8.h already establishes one.

- [x] **Step 3: Build, expect FAIL** (`util/text_utf8.h` missing).

- [x] **Step 4: Create `src/util/text_utf8.h`**

```cpp
#pragma once
#include <string>

// UTF-8 安全的文本截断：按可见字符数（一个汉字算 1）截断，绝不从多字节
// 字符中间切断；发生截断时追加省略号 "…"(U+2026)。纯函数，可单测。
// 仿 path_utf8.h：UTF-8 续字节恒为 0x80~0xBF，据此识别字符边界。
namespace text_utf8 {

inline std::string truncate(const std::string& s, size_t max_chars) {
    size_t i = 0;        // 字节游标
    size_t chars = 0;    // 已计字符数
    while (i < s.size() && chars < max_chars) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = (b < 0x80) ? 1 : ((b >> 5) == 0x6) ? 2 : ((b >> 4) == 0xE) ? 3 : 4;
        i += step;
        ++chars;
    }
    if (i >= s.size()) return s;          // 未截断
    return s.substr(0, i) + "\xE2\x80\xA6";  // U+2026 省略号
}

}  // namespace text_utf8
```

- [x] **Step 5: Build + tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="text_utf8*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 5 new cases pass; full suite **136** (131 + 5).

- [x] **Step 6: Commit**

```powershell
git add src/util/text_utf8.h tests/test_text_utf8.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(retrievecheck): utf8-safe text truncation helper

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

## Task 2: retrievecheck Command And Live Verification

**Files:**
- Modify: `src/main.cpp`

- [x] **Step 1: Add includes**

In `src/main.cpp`, near the other project includes, add:
```cpp
#include "retrieve/text_search.h"
#include "util/text_utf8.h"
```
(`<algorithm>` for `std::max`/`std::atoi`'s `<cstdlib>` — add `#include <algorithm>` and `#include <cstdlib>` if not already present.)

- [x] **Step 2: Add `cmd_retrievecheck` after `cmd_chunkload`**

Place this immediately before `int main(...)`:

```cpp
// 检索可观测性：跑与 query 相同的三路召回+RRF（窗口放大到 k），打印融合 top-k
// 排名表（来源标签 + 定位 + 文本片段）。不调用 LLM。
static int cmd_retrievecheck(const Config& cfg, const std::string& question, int k) {
    try {
        PgClient pg(cfg.pg_conninfo);
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        SynonymDict syn;
        syn.load_from_file("config/synonyms.txt");

        auto cands = text_retrieve(question, mv, embed, pg, syn, cfg.milvus_collection,
                                   /*per_path_k=*/k * 4, /*top_k=*/k);

        std::cout << "\nretrievecheck \"" << question << "\" | 召回 "
                  << cands.size() << " 条 (k=" << k << ")\n\n";
        std::cout << "#   rrf      source        clause/method        title\n";

        int rank = 1;
        for (const auto& c : cands) {
            auto chunk = pg.get_chunk(c.chunk_id);
            std::string method = chunk ? (chunk->method_no.empty() ? "-" : chunk->method_no) : "?";
            std::string clause = chunk ? chunk->clause_no : "?";
            std::string title = chunk ? chunk->title : "[缺失]";

            char head[256];
            std::snprintf(head, sizeof(head), "%-3d %-8.4f %-12s  %s / %s",
                          rank, c.score, c.source.c_str(),
                          method.c_str(), clause.c_str());
            std::cout << head << "  " << title << "\n";

            if (chunk) {
                // 压平换行/制表符为空格，再 UTF-8 安全截断到 120 字符
                std::string flat = chunk->atomic_text;
                for (char& ch : flat) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                std::cout << "    片段：" << text_utf8::truncate(flat, 120) << "\n";
            }
            ++rank;
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] retrievecheck: {}", e.what());
        return 1;
    }
}
```

- [x] **Step 3: Update the usage line**

Replace:
```cpp
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck|chunkload> [args]\n";
```
with:
```cpp
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck|chunkload|retrievecheck> [args]\n";
```

- [x] **Step 4: Add the dispatch block**

After the `chunkload` dispatch block (before `std::cout << "unknown command: "`), add:
```cpp
    if (cmd == "retrievecheck") {
        if (argc < 3) { std::cout << "usage: rag2 retrievecheck \"问题\" [k]\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        int k = (argc >= 4) ? std::max(1, std::atoi(argv[3])) : 20;
        return cmd_retrievecheck(cfg, argv[2], k);
    }
```

- [x] **Step 5: Build + full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: build succeeds, **136** (no new unit tests; command is I/O+network).

- [x] **Step 6: Confirm query is unchanged**

Grep `cmd_query` in `src/main.cpp` and confirm it is byte-identical to before (still constructs DeepSeekClient, calls `answer_query(question, mv, embed, pg, syn, ds, cfg.milvus_collection, 5)`). The new command must not have altered it.

- [x] **Step 7: Live smoke (only if services up)**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe smoke
```
If Milvus/PG/embedding not all up, STOP and report "live smoke skipped: services unavailable" — build + unit tests cover the code. If up, run:

```powershell
.\rag2.0\x64\Debug\rag2.0.exe retrievecheck "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平" 20
```
Expected: prints up to 20 rows; header shows `召回 N 条 (k=20)`; multiple rows have `source` containing `bm25`; several distinct `method_no` "仪具与材料" chunks appear; Chinese snippets render without mojibake. Report the table verbatim.

Also confirm `query` still works and is unaffected:
```powershell
.\rag2.0\x64\Debug\rag2.0.exe query "T0517 需要哪些仪具"
```
Expected: same kind of LLM answer as before (regression check).

- [x] **Step 8: Commit**

```powershell
git add src/main.cpp
git commit -m @'
feat(retrievecheck): add retrieval observability command

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
Expected: build green; **136** doctest pass; only `src/main.cpp`, `src/util/text_utf8.h`, `tests/test_text_utf8.cpp`, and the 4 project files changed; `logs/` untracked.

---

## Self-Review Checklist

- [x] Spec §2.1 new CLI command parallel to query: Task 2 (dispatch + usage).
- [x] Spec §2.2 reuse text_retrieve, top_k=k, per_path_k=k*4: Task 2 Step 2.
- [x] Spec §2.3 fused single ranked table + source tags: Task 2 Step 2 (one loop over `cands`, prints `c.source`).
- [x] Spec §2.4 UTF-8 safe snippet: Task 1 `text_utf8::truncate`, used in Task 2.
- [x] Spec §3 output columns (#/rrf/source/clause-method/title/片段): Task 2 Step 2.
- [x] Spec §4.1 header-only text_utf8.h mirroring path_utf8.h: Task 1.
- [x] Spec §4.2 cmd_retrievecheck no DeepSeekClient, missing_required check, k default 20: Task 2 Steps 2/4.
- [x] Spec §5 tests: text_utf8 5 cases (Task 1); live verification (Task 2 Step 7).
- [x] Spec §6 non-goals: query/answer_query/text_retrieve untouched (Task 2 Step 6 confirms), no HTTP/frontend, no per-path split, no top_k default change.
- [x] Type consistency: `text_utf8::truncate(string, size_t)`, `text_retrieve(...,k*4,k)`, `Candidate.{chunk_id,score,source}`, `get_chunk(...)->{method_no,clause_no,title,atomic_text}` — consistent across tasks.
