# M2c-3 Chunk Persistence And Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist M2c-2 `chunk_cache` into PostgreSQL `retrieval_chunks` and Milvus (dense vectors from `embedding_text` only), replace the M1 ingest splitting, and switch the query path to chunk-based context.

**Architecture:** Add a `chunk_loader` module shared by a new `chunkload` CLI command and the rewritten `ingest_file`. Idempotency is delete-then-insert by `standard_id` in both stores. Milvus collection schema upgrades to `chunk_id` primary key with `node_id`/`standard_id` scalars. `answer_query` looks up `retrieval_chunks` and feeds `context_text` to the LLM.

**Tech Stack:** C++20, doctest, nlohmann/json, libpqxx, spdlog, Milvus REST v2, MSBuild/vcpkg, Visual Studio `.vcxproj` projects.

Spec: `docs/superpowers/specs/2026-06-11-m2c3-chunk-persistence-design.md`

---

## Engineering Notes

Work in `D:\vs2022 code\rag2.0`, branch `V2.4`.

Before implementing, run:

```powershell
git status --short
```

Expected: clean except untracked `logs/`. This plan only edits files listed in each task.

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
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="<exact test case name>"
```

Notes:

- Source files contain Chinese string literals; the projects already compile with `/utf-8`. Save all files as UTF-8 without BOM.
- `PgClient` methods open a live PostgreSQL connection; the codebase has no PG test double, so PG methods get no unit tests (consistent with existing `pg_client.cpp`, which has none). Pure-function logic is extracted and tested instead.
- Do NOT run live `chunkload`/`ingest`/`query` smoke unless `config.json` has working PG/Milvus/embedding credentials; the final task gates this.
- The tests project (`rag2.0.tests.vcxproj`) already compiles `pg_client.cpp`, `milvus_rest.cpp`, `answer_pipeline.cpp`, `dense_retriever.cpp`, `ingest_pipeline.cpp`, so new tests that include those headers link fine.

---

## File Structure

Create:

| File | Responsibility |
|---|---|
| `src/ingest/chunk_loader.h` | `ChunkLoadResult`, `chunk_to_row()` (pure), `load_chunks()` declarations |
| `src/ingest/chunk_loader.cpp` | Delete-then-insert load into PG + Milvus; per-chunk embed with warn-and-skip |
| `tests/test_chunk_loader.cpp` | `chunk_to_row` field mapping and captions/formulas JSON serialization |

Modify:

| File | Responsibility |
|---|---|
| `src/db/schema.sql` | Add `retrieval_chunks` table + indexes |
| `src/db/pg_client.h` | `RetrievalChunkRow`, `delete_chunks_by_standard`, `insert_chunk`, `get_chunk` |
| `src/db/pg_client.cpp` | Implement the three chunk methods |
| `src/milvus/milvus_rest.h` | `Hit.chunk_id`, new `build_insert_body`/`build_delete_body`, `insert` new signature, `delete_by_standard`, `drop_collection` |
| `src/milvus/milvus_rest.cpp` | Implement; `ensure_collection` schema with `chunk_id` PK; search output fields |
| `tests/test_milvus_body.cpp` | Update insert-body test, add delete-body test |
| `src/ingest/ingest_pipeline.cpp` | Rewrite: parse → tree → chunks → caches → `load_chunks` |
| `src/ingest/ingest_pipeline.h` | Update doc comment (`clause_count` now counts chunks) |
| `src/retrieve/candidate.h` | Comment: `clause_id` now holds `retrieval_chunks.chunk_id` |
| `src/retrieve/dense_retriever.cpp` | `Candidate.clause_id = hit.chunk_id` |
| `src/generate/answer_pipeline.h` | Declare `fragment_from_chunk` (pure, testable) |
| `src/generate/answer_pipeline.cpp` | Implement `fragment_from_chunk`; `answer_query` looks up chunks |
| `tests/test_context.cpp` | Add `fragment_from_chunk` test |
| `src/main.cpp` | `cmd_chunkload`, usage line, dispatch |
| `rag2.0/rag2.0.vcxproj` + `.filters` | Compile `chunk_loader.cpp`, include header |
| `rag2.0.tests/rag2.0.tests.vcxproj` + `.filters` | Compile `chunk_loader.cpp` + `test_chunk_loader.cpp` |

---

## Task 1: PG Schema And Chunk CRUD

**Files:**
- Modify: `src/db/schema.sql`
- Modify: `src/db/pg_client.h`
- Modify: `src/db/pg_client.cpp`

PG methods need a live database, so this task has no unit tests (consistent with the rest of `pg_client.cpp`). Correctness is covered by the Task 6 live smoke and by compile-time use in Tasks 3/5.

- [x] **Step 1: Append the table to `src/db/schema.sql`**

Append at end of file:

```sql

CREATE TABLE IF NOT EXISTS retrieval_chunks (
    chunk_id       TEXT PRIMARY KEY,
    node_id        TEXT,
    standard_id    TEXT REFERENCES standards(standard_id),
    chunk_type     TEXT,
    clause_no      TEXT,
    method_no      TEXT,
    title          TEXT,
    path_text      TEXT,
    atomic_text    TEXT,
    embedding_text TEXT,
    context_text   TEXT,
    captions       JSONB DEFAULT '[]',
    formulas       JSONB DEFAULT '[]',
    page_start     INT,
    page_end       INT,
    has_table      BOOLEAN DEFAULT FALSE,
    has_formula    BOOLEAN DEFAULT FALSE,
    has_figure     BOOLEAN DEFAULT FALSE,
    suspect        TEXT
);

CREATE INDEX IF NOT EXISTS idx_chunks_standard ON retrieval_chunks(standard_id);
CREATE INDEX IF NOT EXISTS idx_chunks_node ON retrieval_chunks(node_id);
```

- [x] **Step 2: Add the row struct and method declarations to `src/db/pg_client.h`**

After the `StandardRow` struct, add:

```cpp
// retrieval_chunks 表的一行。captions_json/formulas_json 是 JSON 数组文本，
// 序列化在 chunk_loader 的纯函数里做，db 层只透传。
struct RetrievalChunkRow {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    std::string chunk_type;
    std::string clause_no;
    std::string method_no;
    std::string title;
    std::string path_text;
    std::string atomic_text;
    std::string embedding_text;
    std::string context_text;
    std::string captions_json = "[]";
    std::string formulas_json = "[]";
    int page_start = 0;
    int page_end = 0;
    bool has_table = false;
    bool has_formula = false;
    bool has_figure = false;
    std::string suspect;
};
```

Inside `class PgClient`, after `get_standard`, add:

```cpp
    // M2c-3：retrieval_chunks 落库与回查。先删后插的幂等键是 standard_id。
    int delete_chunks_by_standard(const std::string& standard_id);  // 返回删除行数
    void insert_chunk(const RetrievalChunkRow& row);
    std::optional<RetrievalChunkRow> get_chunk(const std::string& chunk_id);
```

- [x] **Step 3: Implement the three methods in `src/db/pg_client.cpp`**

Append at end of file:

```cpp
int PgClient::delete_chunks_by_standard(const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec("DELETE FROM retrieval_chunks WHERE standard_id=$1",
                     pqxx::params{standard_id});
    tx.commit();
    return static_cast<int>(r.affected_rows());
}

void PgClient::insert_chunk(const RetrievalChunkRow& c) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    tx.exec(
        "INSERT INTO retrieval_chunks(chunk_id,node_id,standard_id,chunk_type,"
        "clause_no,method_no,title,path_text,atomic_text,embedding_text,context_text,"
        "captions,formulas,page_start,page_end,has_table,has_formula,has_figure,suspect) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12::jsonb,$13::jsonb,$14,$15,$16,$17,$18,$19) "
        "ON CONFLICT (chunk_id) DO UPDATE SET "
        "node_id=EXCLUDED.node_id, standard_id=EXCLUDED.standard_id, "
        "chunk_type=EXCLUDED.chunk_type, clause_no=EXCLUDED.clause_no, "
        "method_no=EXCLUDED.method_no, title=EXCLUDED.title, path_text=EXCLUDED.path_text, "
        "atomic_text=EXCLUDED.atomic_text, embedding_text=EXCLUDED.embedding_text, "
        "context_text=EXCLUDED.context_text, captions=EXCLUDED.captions, "
        "formulas=EXCLUDED.formulas, page_start=EXCLUDED.page_start, "
        "page_end=EXCLUDED.page_end, has_table=EXCLUDED.has_table, "
        "has_formula=EXCLUDED.has_formula, has_figure=EXCLUDED.has_figure, "
        "suspect=EXCLUDED.suspect",
        pqxx::params{c.chunk_id, c.node_id, c.standard_id, c.chunk_type,
                     c.clause_no, c.method_no, c.title, c.path_text,
                     c.atomic_text, c.embedding_text, c.context_text,
                     c.captions_json, c.formulas_json, c.page_start, c.page_end,
                     c.has_table, c.has_formula, c.has_figure, c.suspect});
    tx.commit();
}

std::optional<RetrievalChunkRow> PgClient::get_chunk(const std::string& chunk_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec(
        "SELECT chunk_id,node_id,standard_id,COALESCE(chunk_type,''),"
        "COALESCE(clause_no,''),COALESCE(method_no,''),COALESCE(title,''),"
        "COALESCE(path_text,''),COALESCE(atomic_text,''),COALESCE(embedding_text,''),"
        "COALESCE(context_text,''),COALESCE(captions::text,'[]'),"
        "COALESCE(formulas::text,'[]'),COALESCE(page_start,0),COALESCE(page_end,0),"
        "COALESCE(has_table,FALSE),COALESCE(has_formula,FALSE),"
        "COALESCE(has_figure,FALSE),COALESCE(suspect,'') "
        "FROM retrieval_chunks WHERE chunk_id=$1", pqxx::params{chunk_id});
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    RetrievalChunkRow c;
    c.chunk_id = row[0].c_str(); c.node_id = row[1].c_str();
    c.standard_id = row[2].c_str(); c.chunk_type = row[3].c_str();
    c.clause_no = row[4].c_str(); c.method_no = row[5].c_str();
    c.title = row[6].c_str(); c.path_text = row[7].c_str();
    c.atomic_text = row[8].c_str(); c.embedding_text = row[9].c_str();
    c.context_text = row[10].c_str(); c.captions_json = row[11].c_str();
    c.formulas_json = row[12].c_str(); c.page_start = row[13].as<int>();
    c.page_end = row[14].as<int>(); c.has_table = row[15].as<bool>();
    c.has_formula = row[16].as<bool>(); c.has_figure = row[17].as<bool>();
    c.suspect = row[18].c_str();
    return c;
}
```

- [x] **Step 4: Build and run all tests (must stay green)**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, all existing tests pass (additive change, no callers yet).

- [x] **Step 5: Commit Task 1**

```powershell
git add src/db/schema.sql src/db/pg_client.h src/db/pg_client.cpp
git commit -m "feat(m2c3): add retrieval_chunks table and pg chunk crud"
```

---

## Task 2: Milvus Chunk Schema, Insert, Delete

**Files:**
- Modify: `tests/test_milvus_body.cpp`
- Modify: `src/milvus/milvus_rest.h`
- Modify: `src/milvus/milvus_rest.cpp`
- Modify: `src/ingest/ingest_pipeline.cpp` (call-site adaptation only; full rewrite is Task 4)

- [x] **Step 1: Update the failing body-builder tests**

In `tests/test_milvus_body.cpp`, replace the existing `build_insert_body` test case with:

```cpp
TEST_CASE("build_insert_body wraps one row with chunk scalars and dense vector") {
    std::vector<float> vec = {1.0f, 2.0f};
    std::string body = milvus::build_insert_body("clause_text", "n1#main", "n1", "s1", vec);
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["data"][0]["chunk_id"] == "n1#main");
    CHECK(j["data"][0]["node_id"] == "n1");
    CHECK(j["data"][0]["standard_id"] == "s1");
    CHECK(j["data"][0]["dense"].size() == 2);
}
```

Append a new test case:

```cpp
TEST_CASE("build_delete_body filters by standard id") {
    std::string body = milvus::build_delete_body("clause_text", "s1");
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["filter"] == "standard_id == \"s1\"");
}
```

Leave the `build_search_body` test unchanged (its signature does not change).

- [x] **Step 2: Build and verify the new tests fail**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `build_insert_body` has no 5-argument overload and `build_delete_body` does not exist.

- [x] **Step 3: Update `src/milvus/milvus_rest.h`**

Replace the `Hit` struct with:

```cpp
struct Hit {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    float score = 0.0f;
};
```

Replace the `build_insert_body` declaration with:

```cpp
std::string build_insert_body(const std::string& collection,
                              const std::string& chunk_id,
                              const std::string& node_id,
                              const std::string& standard_id,
                              const std::vector<float>& dense);
std::string build_delete_body(const std::string& collection,
                              const std::string& standard_id);
```

In `class MilvusRest`, replace the `insert` declaration and add two methods:

```cpp
    void insert(const std::string& collection, const std::string& chunk_id,
                const std::string& node_id, const std::string& standard_id,
                const std::vector<float>& dense);
    // 按 standard_id 删除该标准的全部向量（先删后插幂等的 Milvus 侧）
    void delete_by_standard(const std::string& collection, const std::string& standard_id);
    // 一次性升级/测试清理用
    void drop_collection(const std::string& collection);
```

- [x] **Step 4: Update `src/milvus/milvus_rest.cpp`**

Replace `build_insert_body` with:

```cpp
std::string build_insert_body(const std::string& collection, const std::string& chunk_id,
                              const std::string& node_id, const std::string& standard_id,
                              const std::vector<float>& dense) {
    json row;
    row["chunk_id"] = chunk_id;
    row["node_id"] = node_id;
    row["standard_id"] = standard_id;
    row["dense"] = dense;
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({row});
    return body.dump();
}
```

After `build_search_body`, add:

```cpp
std::string build_delete_body(const std::string& collection, const std::string& standard_id) {
    json body;
    body["collectionName"] = collection;
    body["filter"] = "standard_id == \"" + standard_id + "\"";
    return body.dump();
}
```

In `ensure_collection`, replace the `schema["fields"]` array so the primary key is `chunk_id`:

```cpp
    schema["fields"] = json::array({
        { {"fieldName","chunk_id"}, {"dataType","VarChar"}, {"isPrimary",true},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","node_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","standard_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 128} }} },
        { {"fieldName","dense"}, {"dataType","FloatVector"},
          {"elementTypeParams", { {"dim", dim} }} }
    });
```

Replace `MilvusRest::insert` with:

```cpp
void MilvusRest::insert(const std::string& collection, const std::string& chunk_id,
                        const std::string& node_id, const std::string& standard_id,
                        const std::vector<float>& dense) {
    auto body = build_insert_body(collection, chunk_id, node_id, standard_id, dense);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/insert", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus insert failed: " + res.body + res.error);
}
```

After `insert`, add:

```cpp
void MilvusRest::delete_by_standard(const std::string& collection,
                                    const std::string& standard_id) {
    auto body = build_delete_body(collection, standard_id);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/delete", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus delete failed: " + res.body + res.error);
}

void MilvusRest::drop_collection(const std::string& collection) {
    json body;
    body["collectionName"] = collection;
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/drop", body.dump(),
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus drop collection failed: " + res.body + res.error);
}
```

In `MilvusRest::search`, replace the output-fields call and hit parsing:

```cpp
    auto body = build_search_body(collection, query, top_k,
                                  {"chunk_id", "node_id", "standard_id"});
```

and inside the result loop:

```cpp
        Hit h;
        h.chunk_id = item.value("chunk_id", "");
        h.node_id = item.value("node_id", "");
        h.standard_id = item.value("standard_id", "");
        h.score = item.value("distance", 0.0f);
        hits.push_back(h);
```

- [x] **Step 5: Adapt the old ingest call site (placeholder until Task 4)**

In `src/ingest/ingest_pipeline.cpp`, the line:

```cpp
            mv.insert(collection, node_id, standard_id, vec);
```

becomes (chunk_id 暂用 node_id 占位，Task 4 整体重写该文件):

```cpp
            mv.insert(collection, node_id, node_id, standard_id, vec);
```

`src/retrieve/dense_retriever.cpp` still reads `h.node_id`, which still exists — no change needed in this task.

- [x] **Step 6: Build and run the milvus body tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_insert_body wraps one row with chunk scalars and dense vector"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="build_delete_body filters by standard id"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: both new cases PASS, full suite PASS.

- [x] **Step 7: Commit Task 2**

```powershell
git add tests/test_milvus_body.cpp src/milvus/milvus_rest.h src/milvus/milvus_rest.cpp src/ingest/ingest_pipeline.cpp
git commit -m "feat(m2c3): milvus chunk schema with delete by standard"
```

---

## Task 3: Chunk Loader Module

**Files:**
- Create: `src/ingest/chunk_loader.h`
- Create: `src/ingest/chunk_loader.cpp`
- Create: `tests/test_chunk_loader.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`
- Modify: `rag2.0/rag2.0.vcxproj.filters`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj`
- Modify: `rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [x] **Step 1: Write the failing `chunk_to_row` test**

Create `tests/test_chunk_loader.cpp`:

```cpp
#include <doctest/doctest.h>
#include "ingest/chunk_loader.h"

#include <nlohmann/json.hpp>

TEST_CASE("chunk_to_row maps all fields and serializes captions and formulas to json") {
    RetrievalChunk c;
    c.chunk_id = "sid:5/5.1/5.1.2#main";
    c.node_id = "sid:5/5.1/5.1.2";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5.1.2";
    c.method_no = "T0302-2024";
    c.title = "路基沉降";
    c.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    c.atomic_text = "5.1.2 路基沉降\n正文";
    c.embedding_text = "5.1.2 路基沉降\n正文\n图题";
    c.context_text = "路径：…\n正文";
    c.captions = {"图5.1.2 路基沉降示意图"};
    c.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    c.page_start = 12;
    c.page_end = 13;
    c.has_table = true;
    c.has_formula = true;
    c.has_figure = true;
    c.suspect = "seq";

    RetrievalChunkRow row = chunk_to_row(c);

    CHECK(row.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(row.node_id == "sid:5/5.1/5.1.2");
    CHECK(row.standard_id == "sid");
    CHECK(row.chunk_type == "body");
    CHECK(row.clause_no == "5.1.2");
    CHECK(row.method_no == "T0302-2024");
    CHECK(row.title == "路基沉降");
    CHECK(row.path_text == c.path_text);
    CHECK(row.atomic_text == c.atomic_text);
    CHECK(row.embedding_text == c.embedding_text);
    CHECK(row.context_text == c.context_text);
    CHECK(row.page_start == 12);
    CHECK(row.page_end == 13);
    CHECK(row.has_table);
    CHECK(row.has_formula);
    CHECK(row.has_figure);
    CHECK(row.suspect == "seq");

    auto caps = nlohmann::json::parse(row.captions_json);
    REQUIRE(caps.is_array());
    REQUIRE(caps.size() == 1);
    CHECK(caps[0] == "图5.1.2 路基沉降示意图");

    auto forms = nlohmann::json::parse(row.formulas_json);
    REQUIRE(forms.is_array());
    REQUIRE(forms.size() == 1);
    CHECK(forms[0] == "MQI = SCI + PQI + BCI + TCI");
}

TEST_CASE("chunk_to_row serializes empty captions and formulas as empty json arrays") {
    RetrievalChunk c;
    c.chunk_id = "sid:1#main";
    RetrievalChunkRow row = chunk_to_row(c);
    CHECK(row.captions_json == "[]");
    CHECK(row.formulas_json == "[]");
}
```

- [x] **Step 2: Add project entries**

In `rag2.0/rag2.0.vcxproj`, next to `<ClCompile Include="..\src\ingest\ingest_pipeline.cpp" />` add:

```xml
<ClCompile Include="..\src\ingest\chunk_loader.cpp" />
```

and next to the other `src\ingest` `ClInclude` entries add:

```xml
<ClInclude Include="..\src\ingest\chunk_loader.h" />
```

In `rag2.0/rag2.0.vcxproj.filters`, add next to the existing `源文件\ingest` / `头文件\ingest` entries (filter names verified against `ingest_pipeline.cpp` entries):

```xml
<ClCompile Include="..\src\ingest\chunk_loader.cpp"><Filter>源文件\ingest</Filter></ClCompile>
```

```xml
<ClInclude Include="..\src\ingest\chunk_loader.h"><Filter>头文件\ingest</Filter></ClInclude>
```

In `rag2.0.tests/rag2.0.tests.vcxproj`, add:

```xml
<ClCompile Include="..\src\ingest\chunk_loader.cpp" />
<ClCompile Include="..\tests\test_chunk_loader.cpp" />
```

In `rag2.0.tests/rag2.0.tests.vcxproj.filters`, add (reusing the filters that `test_retrieval_chunk.cpp` and `retrieval_chunk.cpp` use):

```xml
<ClCompile Include="..\tests\test_chunk_loader.cpp"><Filter>测试</Filter></ClCompile>
<ClCompile Include="..\src\ingest\chunk_loader.cpp"><Filter>被测源码</Filter></ClCompile>
```

- [x] **Step 3: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `ingest/chunk_loader.h` does not exist.

- [x] **Step 4: Create `src/ingest/chunk_loader.h`**

```cpp
#pragma once

#include "db/pg_client.h"
#include "embedding/embedding_client.h"
#include "milvus/milvus_rest.h"
#include "retrieve/retrieval_chunk.h"

#include <string>

struct ChunkLoadResult {
    int chunk_count = 0;     // cache 中 chunk 总数
    int embedded_count = 0;  // 成功 embed 并写入 Milvus 的数量
    int deleted_count = 0;   // 先删后插阶段删除的 PG 旧 chunk 行数
};

// 纯函数：RetrievalChunk -> PG 行。captions/formulas 序列化为 JSON 数组文本。
RetrievalChunkRow chunk_to_row(const RetrievalChunk& c);

// chunk_cache -> PG retrieval_chunks + Milvus。按 standard_id 先删后插。
// dense 向量只 embed embedding_text。embed 单条失败记警告跳过，
// 调用方用 embedded_count < chunk_count 判断是否需要重跑。
ChunkLoadResult load_chunks(const RetrievalChunkCache& cache,
                            PgClient& pg,
                            milvus::MilvusRest& mv,
                            EmbeddingClient& embed,
                            const std::string& collection);
```

- [x] **Step 5: Create `src/ingest/chunk_loader.cpp`**

```cpp
#include "ingest/chunk_loader.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

RetrievalChunkRow chunk_to_row(const RetrievalChunk& c) {
    RetrievalChunkRow row;
    row.chunk_id = c.chunk_id;
    row.node_id = c.node_id;
    row.standard_id = c.standard_id;
    row.chunk_type = c.chunk_type;
    row.clause_no = c.clause_no;
    row.method_no = c.method_no;
    row.title = c.title;
    row.path_text = c.path_text;
    row.atomic_text = c.atomic_text;
    row.embedding_text = c.embedding_text;
    row.context_text = c.context_text;
    row.captions_json = json(c.captions).dump();
    row.formulas_json = json(c.formulas).dump();
    row.page_start = c.page_start;
    row.page_end = c.page_end;
    row.has_table = c.has_table;
    row.has_formula = c.has_formula;
    row.has_figure = c.has_figure;
    row.suspect = c.suspect;
    return row;
}

ChunkLoadResult load_chunks(const RetrievalChunkCache& cache, PgClient& pg,
                            milvus::MilvusRest& mv, EmbeddingClient& embed,
                            const std::string& collection) {
    ChunkLoadResult result;
    result.chunk_count = static_cast<int>(cache.chunks.size());

    // 幂等：先删两个库里这份标准的旧数据，再插入
    result.deleted_count = pg.delete_chunks_by_standard(cache.standard_id);
    mv.delete_by_standard(collection, cache.standard_id);

    for (const auto& c : cache.chunks) {
        pg.insert_chunk(chunk_to_row(c));
        try {
            std::vector<float> vec = embed.embed(c.embedding_text);
            mv.insert(collection, c.chunk_id, c.node_id, c.standard_id, vec);
            ++result.embedded_count;
        } catch (const std::exception& e) {
            spdlog::warn("chunk embed/写入失败，跳过: {} ({})", c.chunk_id, e.what());
        }
    }

    return result;
}
```

- [x] **Step 6: Build and run the chunk loader tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="chunk_to_row*"
```

Expected: both `chunk_to_row` cases PASS.

- [x] **Step 7: Commit Task 3**

```powershell
git add src/ingest/chunk_loader.h src/ingest/chunk_loader.cpp tests/test_chunk_loader.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat(m2c3): add chunk loader with delete-then-insert"
```

---

## Task 4: Rewrite `ingest_file`

**Files:**
- Modify: `src/ingest/ingest_pipeline.h`
- Modify: `src/ingest/ingest_pipeline.cpp`

No existing test references `ingest_file` (verified), so the rewrite is covered by compilation plus the full suite staying green. `clause_splitter` and its tests stay untouched.

- [x] **Step 1: Update the doc comment in `src/ingest/ingest_pipeline.h`**

Replace:

```cpp
// 解析 1 份文件 → 切分条款 → 写 PG → 生成向量 → 写 Milvus。
// standard_no/standard_name 在 M1 用文件名占位（M2 由元数据抽取替换）。
```

with:

```cpp
// M2c-3：解析 1 份文件（带 parse_cache）→ 建条款树 → 生成受控 chunk
// → 写 PG retrieval_chunks → embed embedding_text → 写 Milvus。
// 同时落 tree_cache/chunk_cache 副产品，与 treecheck/chunkcheck 产物一致。
// IngestResult.clause_count 自本刀起表示 chunk 数。
```

- [x] **Step 2: Rewrite `src/ingest/ingest_pipeline.cpp`**

Replace the whole file with:

```cpp
#include "ingest/ingest_pipeline.h"
#include "ingest/chunk_loader.h"
#include "ingest/standard_meta.h"
#include "parse/parse_cache.h"
#include "retrieve/retrieval_chunk.h"
#include "structure/clause_tree.h"
#include "structure/tree_builder.h"
#include "util/path_utf8.h"
#include <spdlog/spdlog.h>
#include <functional>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection, const std::string& cache_dir) {
    ParsedDoc doc = parser.parse(file_path);

    std::string stem = path_utf8::stem(file_path);
    std::string standard_id = make_id(file_path);
    std::string page1 = doc.pages.empty() ? std::string() : doc.pages[0].text;
    doc.standard_no = extract_standard_no(page1, stem);
    write_parse_cache(cache_dir + "/" + standard_id + ".json", doc);

    // M2c-1/M2c-2：建树 + 受控 chunk，顺手落缓存便于 treecheck/chunkcheck 排查
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

    mv.ensure_collection(collection, embed.dim());
    ChunkLoadResult r = load_chunks(chunks, pg, mv, embed, collection);

    IngestResult result;
    result.standard_id = standard_id;
    result.clause_count = r.chunk_count;
    spdlog::info("入库完成: chunks={} embedded={} deleted_old={} (standard_id={})",
                 r.chunk_count, r.embedded_count, r.deleted_count, standard_id);
    return result;
}
```

- [x] **Step 3: Build and run all tests**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, all tests pass (`split_clauses` is no longer called by ingest but `clause_splitter` tests still pass on the module itself).

- [x] **Step 4: Commit Task 4**

```powershell
git add src/ingest/ingest_pipeline.h src/ingest/ingest_pipeline.cpp
git commit -m "feat(m2c3): ingest via tree and controlled chunks"
```

---

## Task 5: Query Wiring To Chunks

**Files:**
- Modify: `tests/test_context.cpp`
- Modify: `src/generate/answer_pipeline.h`
- Modify: `src/generate/answer_pipeline.cpp`
- Modify: `src/retrieve/dense_retriever.cpp`
- Modify: `src/retrieve/candidate.h`

- [x] **Step 1: Write the failing `fragment_from_chunk` test**

Append to `tests/test_context.cpp`:

```cpp
#include "generate/answer_pipeline.h"

TEST_CASE("fragment_from_chunk maps chunk row to llm context fragment") {
    RetrievalChunkRow chunk;
    chunk.chunk_id = "sid:5/5.1/5.1.2#main";
    chunk.standard_id = "sid";
    chunk.clause_no = "5.1.2";
    chunk.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    chunk.atomic_text = "5.1.2 路基沉降\n正文";
    chunk.context_text = "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n\n5.1.2 路基沉降\n正文";

    StandardRow s;
    s.standard_no = "JTC 5210-2018";
    s.standard_name = "公路技术状况评定标准";
    s.status = "现行";

    ContextFragment f = fragment_from_chunk(3, chunk, s);
    CHECK(f.source_id == "S3");
    CHECK(f.standard_no == "JTC 5210-2018");
    CHECK(f.standard_name == "公路技术状况评定标准");
    CHECK(f.status == "现行");
    CHECK(f.clause_no == "5.1.2");
    CHECK(f.path == chunk.path_text);
    CHECK(f.text == chunk.context_text);   // LLM 上下文必须是 small-to-big 的 context_text
    CHECK_FALSE(f.is_mandatory);
}

TEST_CASE("fragment_from_chunk tolerates missing standard row") {
    RetrievalChunkRow chunk;
    chunk.clause_no = "2";
    chunk.context_text = "正文";
    ContextFragment f = fragment_from_chunk(1, chunk, std::nullopt);
    CHECK(f.source_id == "S1");
    CHECK(f.standard_no == "");
    CHECK(f.standard_name == "");
    CHECK(f.status == "");
    CHECK(f.text == "正文");
}
```

- [x] **Step 2: Build and verify failure**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: build FAILS — `fragment_from_chunk` is not declared.

- [x] **Step 3: Declare `fragment_from_chunk` in `src/generate/answer_pipeline.h`**

Replace the file content with:

```cpp
#pragma once
#include <optional>
#include <string>
#include "retrieve/retriever.h"
#include "db/pg_client.h"
#include "generate/context.h"
#include "generate/deepseek_client.h"

// 纯函数：chunk 行 + standards 行 -> LLM 上下文片段。
// text 取 context_text（small-to-big 回填），引用元数据取 chunk 定位字段。
ContextFragment fragment_from_chunk(int idx,
                                    const RetrievalChunkRow& chunk,
                                    const std::optional<StandardRow>& std_row);

// query → 检索候选（clause_id 即 chunk_id）→ 回查 PG retrieval_chunks
// → 组装 ContextFragment → DeepSeek 生成带溯源回答。
// 候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         Retriever& retriever,
                         PgClient& pg,
                         deepseek::DeepSeekClient& ds,
                         int top_k);
```

- [x] **Step 4: Rewrite `src/generate/answer_pipeline.cpp`**

Replace the file content with:

```cpp
#include "generate/answer_pipeline.h"
#include "generate/prompt_builder.h"
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
    f.text = chunk.context_text;
    return f;
}

std::string answer_query(const std::string& question, Retriever& retriever, PgClient& pg,
                         deepseek::DeepSeekClient& ds, int top_k) {
    auto candidates = retriever.retrieve(question, top_k);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        // 底座原则：以 PG 回查为权威源（M2c-3 起权威源是 retrieval_chunks）
        auto chunk = pg.get_chunk(c.clause_id);
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

- [x] **Step 5: Switch `DenseRetriever` to chunk ids**

In `src/retrieve/dense_retriever.cpp`, replace:

```cpp
        c.clause_id = h.node_id;   // 归一化键
```

with:

```cpp
        c.clause_id = h.chunk_id;  // M2c-3 起归一化键为 retrieval_chunks.chunk_id
```

In `src/retrieve/candidate.h`, replace the comment lines:

```cpp
// 所有召回候选统一归一到 standard_id + clause_id（node_id），作为去重键。
struct Candidate {
    std::string standard_id;
    std::string clause_id;   // == clause_nodes.node_id
```

with:

```cpp
// 所有召回候选统一归一到 standard_id + clause_id，作为去重键。
struct Candidate {
    std::string standard_id;
    std::string clause_id;   // == retrieval_chunks.chunk_id（M3 统一改名）
```

- [x] **Step 6: Build and run the new tests plus full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="fragment_from_chunk*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: both `fragment_from_chunk` cases PASS, full suite PASS.

- [x] **Step 7: Commit Task 5**

```powershell
git add tests/test_context.cpp src/generate/answer_pipeline.h src/generate/answer_pipeline.cpp src/retrieve/dense_retriever.cpp src/retrieve/candidate.h
git commit -m "feat(m2c3): answer pipeline reads retrieval chunks"
```

---

## Task 6: Chunkload CLI And Final Verification

**Files:**
- Modify: `src/main.cpp`

- [x] **Step 1: Add the include**

In `src/main.cpp`, near `#include "ingest/ingest_pipeline.h"` add:

```cpp
#include "ingest/chunk_loader.h"
```

(`retrieve/retrieval_chunk.h` is already included via Task M2c-2.)

- [x] **Step 2: Add `cmd_chunkload` after `cmd_chunkcheck`**

```cpp
// 落库命令：读 chunk_cache，按 standard_id 先删后插写入 PG + Milvus。
// embedding 只吃 embedding_text。部分失败返回 1，重跑即可全量修复。
static int cmd_chunkload(const Config& cfg, const std::string& chunk_cache_path) {
    try {
        if (!std::filesystem::exists(chunk_cache_path)) {
            spdlog::error("chunk 缓存文件不存在: {}", chunk_cache_path);
            return 1;
        }
        std::string js = read_file(chunk_cache_path);
        if (js.empty()) {
            spdlog::error("chunk 缓存文件为空: {}", chunk_cache_path);
            return 1;
        }
        RetrievalChunkCache cache = retrieval_chunk_cache_from_json(js);
        if (cache.chunks.empty()) {
            spdlog::error("chunk 缓存中没有 chunk: {}", chunk_cache_path);
            return 1;
        }

        PgClient pg(cfg.pg_conninfo);
        pg.apply_schema(read_file("src/db/schema.sql"));   // 幂等建表
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);

        // standards 占位行，保证 retrieval_chunks 外键成立（ingest 全链路会写真行覆盖）
        if (!pg.get_standard(cache.standard_id)) {
            StandardRow s;
            s.standard_id = cache.standard_id;
            s.standard_no = cache.standard_no;
            s.standard_name = cache.standard_no;
            s.status = "现行";
            pg.upsert_standard(s);
        }

        mv.ensure_collection(cfg.milvus_collection, embed.dim());
        ChunkLoadResult r = load_chunks(cache, pg, mv, embed, cfg.milvus_collection);

        spdlog::info("chunkload {} | standard_no={}", cache.standard_id, cache.standard_no);
        spdlog::info("  chunks={} embedded={} deleted_old={}",
                     r.chunk_count, r.embedded_count, r.deleted_count);
        if (r.embedded_count < r.chunk_count) {
            spdlog::warn("部分 chunk 未完成 embedding，重跑 chunkload 可全量修复");
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] chunkload: {}", e.what());
        return 1;
    }
}
```

- [x] **Step 3: Wire the command in `main`**

Replace the usage line:

```cpp
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck> [args]\n";
```

with:

```cpp
        std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck|chunkcheck|chunkload> [args]\n";
```

After the `chunkcheck` command block, add:

```cpp
    if (cmd == "chunkload") {
        if (argc < 3) { std::cout << "usage: rag2 chunkload <chunk_cache.json>\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        return cmd_chunkload(cfg, argv[2]);
    }
```

- [x] **Step 4: Build and run the full suite**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

Expected: build succeeds, all tests pass.

- [x] **Step 5: Live smoke (only if services are configured)**

First check connectivity:

```powershell
.\rag2.0\x64\Debug\rag2.0.exe smoke
```

If smoke fails (PG/Milvus/API not available), STOP here, report "live smoke skipped: services unavailable", and continue to Step 6. Unit tests and build cover the code paths.

If smoke passes, run the one-time collection upgrade. The existing collection (if any) has `node_id` as primary key and must be dropped once — the new code creates the `chunk_id` schema on the next run. Read `milvus_base_url`, `milvus_token`, `milvus_collection` from `config.json` and run:

```powershell
$cfg = Get-Content config.json -Raw | ConvertFrom-Json
$headers = @{ Authorization = "Bearer $($cfg.milvus_token)" }
Invoke-RestMethod -Method Post -Uri "$($cfg.milvus_base_url)/v2/vectordb/collections/drop" -Headers $headers -ContentType "application/json" -Body (@{ collectionName = $cfg.milvus_collection } | ConvertTo-Json)
```

Then load both real caches and query:

```powershell
.\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\12215131224082667446.json
.\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\5797633264338373449.json
.\rag2.0\x64\Debug\rag2.0.exe chunkload data\chunk_cache\12215131224082667446.json
.\rag2.0\x64\Debug\rag2.0.exe query "路基沉降怎么评定"
.\rag2.0\x64\Debug\rag2.0.exe query "T0302 需要哪些仪具"
```

Expected:
- First two chunkload runs: `chunks=157 embedded=157 deleted_old=0` and `chunks=537 embedded=537 deleted_old=0`, exit 0.
- Third run (re-load of the first cache): `deleted_old=157`, `embedded=157` — delete-then-insert idempotency confirmed.
- Both queries return answers citing `clause_no`/`path` from chunk metadata.

- [x] **Step 6: Commit Task 6**

```powershell
git add src/main.cpp
git commit -m "feat(m2c3): add chunkload command"
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
Only intentional M2c-3 files changed; logs/ and data caches stay untracked.
```

Spec acceptance items not verifiable without live services (chunkload idempotency, query answers) are covered by Task 6 Step 5 when services are available; otherwise report them as pending manual verification.

---

## Self-Review Checklist

- [x] Spec §2 decisions all implemented: chunkload+ingest shared loader (Tasks 3/4/6), delete-then-insert (Task 3), reused collection name with chunk_id PK (Task 2), in-place query rewiring (Task 5), clause_nodes kept but unwritten (Task 4).
- [x] Spec §3 schema matches Task 1 SQL column-for-column.
- [x] Spec §4 Milvus changes covered by Task 2 (insert/delete/drop/search/ensure).
- [x] Spec §5 loader API matches Task 3 (`ChunkLoadResult` with deleted_count, `chunk_to_row` pure).
- [x] Spec §6 ingest rewrite matches Task 4 step list.
- [x] Spec §7 query wiring matches Task 5 (`context_text` as LLM text).
- [x] Spec §8 CLI behavior matches Task 6 (placeholder standards row, exit codes).
- [x] Spec §9 error handling: cache errors exit 1, per-chunk embed warn-and-skip, partial load returns 1.
- [x] Spec §10 test strategy mapped: milvus bodies (Task 2), chunk_to_row (Task 3), fragment_from_chunk (Task 5); loader call-order logic is exercised structurally (delete called before inserts in a 40-line function) — no live-network unit test by design.
- [x] Spec §11 non-goals untouched: no BM25, no batching, no spec_tables, no version management.
