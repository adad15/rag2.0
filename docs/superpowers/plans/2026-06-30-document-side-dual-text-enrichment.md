# 文档侧双文本检索增强 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给每个 chunk 新增 `bm25_text`（含标准号/路径/条款/确定性检索词的富化文本），让 Milvus BM25 索引它（dense 仍用干净的 `embedding_text`），修复 `rq-081` 这类"正确 chunk 很短、BM25 漏召"的问题。

**Architecture:** `RetrievalChunk` 加 `bm25_text` 字段、chunk cache schema 升到 v2；新增确定性 `compose_bm25_text`/`compose_bm25_terms`；PG/Milvus 落库改用 `bm25_text` 作 BM25 文本（dense 输入不变）；重建数据后用目标 case + 100 题 rich eval 验收。不动 Milvus schema 字段名（`text` 字段改喂 `bm25_text`）、不改 RRF、不改查询侧。

**Tech Stack:** C++20 / MSBuild + vcpkg / nlohmann/json / libpqxx / doctest / Milvus REST v2。沿用 [[rag2-build-setup]]、[[local-services]]。

**Spec:** [`docs/superpowers/specs/2026-06-30-document-side-dual-text-enrichment-design.md`](../specs/2026-06-30-document-side-dual-text-enrichment-design.md)

**构建/测试（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

**关键事实（源码勘察）：**
- `RetrievalChunk`/`RetrievalChunkCache` 在 `src/retrieve/retrieval_chunk.h`；cache `schema_version` 默认 `1`。
- `src/retrieve/retrieval_chunk.cpp`：匿名命名空间内有 `node_label`/`path_text_for`/`method_no_for`/`compose_embedding_text`(:128)/`compose_context_text`(:187)；`chunk_from_leaf`(:215) 装配各文本；JSON 序列化(:264)/反序列化(:302)。C++ 中文串字面量**用普通 `"..."`（不要 `u8""`，C++20 下 `u8""` 是 `char8_t` 不能拼 `std::string`）**，靠工程 `/utf-8`。
- `src/ingest/chunk_loader.cpp`：`chunk_to_row`(:8) 映射；`load_chunks`(:32) 对每 chunk `embed.embed(c.embedding_text)` + `mv.insert_full(..., c.embedding_text, vec)`，**embed 失败 per-chunk catch 跳过**(:48)。
- `src/db/pg_client.cpp`：`insert_chunk`(:88) 19 列 `$1..$19`；`get_chunk`(:114) SELECT 19 列 COALESCE。`RetrievalChunkRow` 在 `src/db/pg_client.h`。
- `src/db/schema.sql`：`retrieval_chunks` CREATE TABLE(:22)。
- `src/milvus/milvus_rest.*`：`insert_full(collection, chunk_id, node_id, standard_id, status, text, dense)`——**`text` 形参即 BM25 源**，schema 字段名不改。
- 重建命令：`rag2 chunkcheck <tree_cache.json>`(读 tree cache→`build_retrieval_chunk_cache`→写 `data/chunk_cache/<sid>.json`)；`rag2 chunkload <chunk_cache.json>`(读 chunk cache→`load_chunks`→PG+Milvus，会重 embed dense)。
- **ClauseTree 无 `standard_name`**（只有 `standard_no`）。spec §5.3 的"标准名/通用硅酸盐水泥"在建 chunk 时**拿不到**，且历史上 DB `standard_name` 不可靠(=文件名)。→ **本刀 `检索词` 不含标准名**，靠 标准号+紧凑、方法号+紧凑、条款号、路径标题、意图词；`rq-081` 靠 安定性+方法+判定+合格+物理性能(path) 命中。是否够,由 Task 5 目标 case 验收兜底。

**提交纪律：** 每次提交用 pathspec（`git add <files>` + `git commit -m "…" -- <files>`），绝不 `git add -A`/裸 commit。

---

## Task 1: `bm25_text` 字段 + schema v2 + JSON round-trip

**Files:** Modify `src/retrieve/retrieval_chunk.h`, `src/retrieve/retrieval_chunk.cpp`; `tests/test_retrieval_chunk.cpp`

- [ ] **Step 1: 改 `tests/test_retrieval_chunk.cpp` 的 round-trip 用例（先红）**

在第一个 TEST_CASE（"retrieval_chunk JSON round trip…"）里：
1. 在 `c.context_text = ...;` 之后加一行 fixture：
```cpp
    c.bm25_text = "标准：JTC 5210-2018\n路径：5 技术状况评定 > 5.1 路基\n检索词：判定 要求";
```
2. 把 `CHECK(round_trip.schema_version == 1);` 改为 `CHECK(round_trip.schema_version == 2);`
3. 在 `CHECK(r.context_text.find("路径：") != std::string::npos);` 之后加：
```cpp
    CHECK(r.bm25_text.find("标准：JTC 5210-2018") != std::string::npos);
    CHECK(r.bm25_text.find("检索词：判定 要求") != std::string::npos);
```

- [ ] **Step 2: 构建确认失败**

Run MSBuild + 测试 exe。Expected：`schema_version == 2` 断言失败（当前默认 1）+ `bm25_text` 未编译/未保留。

- [ ] **Step 3: 改 `src/retrieve/retrieval_chunk.h`**

在 `RetrievalChunk` 的 `std::string context_text;` 之后加：
```cpp
    std::string bm25_text;
```
把 `RetrievalChunkCache` 的 `int schema_version = 1;` 改为：
```cpp
    int schema_version = 2;
```

- [ ] **Step 4: 改 `src/retrieve/retrieval_chunk.cpp` 的 JSON 序列化/反序列化**

`retrieval_chunk_cache_to_json` 里，在 `{"context_text", c.context_text},` 之后加：
```cpp
            {"bm25_text", c.bm25_text},
```
`retrieval_chunk_cache_from_json` 里，在 `c.context_text = string_value(e, "context_text");` 之后加：
```cpp
            c.bm25_text = string_value(e, "bm25_text");
```
（反序列化默认 `schema_version = j.value("schema_version", 1)` 保持不变：旧 cache 无该键→1，用于 Task 4 拦截。）

- [ ] **Step 5: 构建 + 测试**

Run MSBuild + `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`。
Expected：round-trip 用例通过（schema_version==2、bm25_text 往返）；其余全绿。

- [ ] **Step 6: Commit**
```powershell
git add src/retrieve/retrieval_chunk.h src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
git commit -m @'
feat(chunk): RetrievalChunk 加 bm25_text + cache schema v2 + JSON 往返

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/retrieve/retrieval_chunk.h src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
```

---

## Task 2: `compose_bm25_text` + 检索词规则 + 装配

**Files:** Modify `src/retrieve/retrieval_chunk.cpp`; `tests/test_retrieval_chunk.cpp`

- [ ] **Step 1: 加 build-cache 用例的 bm25_text 断言（先红）**

在 `tests/test_retrieval_chunk.cpp` 的 "build_retrieval_chunk_cache creates one dense-safe chunk per leaf" 用例末尾（`CHECK(c.formulas[0] == ...)` 之后）加：
```cpp
    // bm25_text 富化：含标准号、路径、条款，但 embedding_text 仍不含
    CHECK(c.bm25_text.find("JTC 5210-2018") != std::string::npos);
    CHECK(c.bm25_text.find("技术状况评定 >") != std::string::npos);
    CHECK(c.bm25_text.find("路基沉降") != std::string::npos);
    CHECK(c.embedding_text.find("JTC 5210") == std::string::npos);
```

在 "retrieval chunks keep T method numbers…" 用例里（构建 cache 后；该用例的 leaf 正文是 "天平、标准筛、烘箱。"），断言方法号进 bm25_text：先确认该用例已 `build_retrieval_chunk_cache(tree)` 拿到 `c`（若变量名不同按实际），加：
```cpp
    CHECK(c.bm25_text.find("T0302-2024") != std::string::npos);   // 方法号进 bm25_text
    CHECK(c.bm25_text.find("仪具与材料") != std::string::npos);
    CHECK(c.embedding_text.find("T0302") == std::string::npos);    // embedding_text 仍不含
```
（若该用例尚未取 `const RetrievalChunk& c = cache.chunks[0];`，先补一行取出。）

新增一个意图词用例（验证 §5.3 规则,用 rq-081 同型正文）：
```cpp
TEST_CASE("bm25_text adds intent terms from clause body rules") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "GB 175-2023";
    TreeNode root = make_node("sid:7", "7", "技术要求", "", false);
    root.child_ids = {"sid:7/7.4"};
    TreeNode parent = make_node("sid:7/7.4", "7.4", "物理性能", "", false, "sid:7");
    parent.child_ids = {"sid:7/7.4/7.4.2"};
    TreeNode leaf = make_node("sid:7/7.4/7.4.2", "7.4.2", "安定性",
                              "沸煮法合格。压蒸法合格。", true, "sid:7/7.4");
    tree.nodes = {root, parent, leaf};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);
    REQUIRE(cache.chunks.size() == 1);
    const std::string& b = cache.chunks[0].bm25_text;
    CHECK(b.find("GB175-2023") != std::string::npos);   // 紧凑标准号
    CHECK(b.find("安定性") != std::string::npos);
    CHECK(b.find("物理性能") != std::string::npos);       // 路径
    CHECK(b.find("方法") != std::string::npos);           // 法+合格 → 方法
    CHECK(b.find("判定") != std::string::npos);           // 合格 → 判定
    CHECK(b.find("要求") != std::string::npos);
}
```

- [ ] **Step 2: 构建确认失败**

Run MSBuild + 测试 exe。Expected：bm25_text 断言失败（`chunk_from_leaf` 还没填 bm25_text，为空）。

- [ ] **Step 3: 在 `src/retrieve/retrieval_chunk.cpp` 实现 compose 函数**

在 `compose_context_text`(结束于 :213) 之后、`chunk_from_leaf`(:215) 之前，插入（同一匿名命名空间内；`std::find` 已随 `<algorithm>` 可用）：
```cpp
std::string compact_code(const std::string& s) {
    std::string out;
    for (char ch : s) if (ch != ' ') out += ch;   // "GB 175-2023" -> "GB175-2023"
    return out;
}

std::vector<std::string> compose_bm25_terms(const ClauseTree& tree, const TreeNode& n,
                                            const std::string& method_no) {
    std::vector<std::string> terms;
    auto add = [&](const std::string& t) {
        if (t.empty()) return;
        if (std::find(terms.begin(), terms.end(), t) == terms.end()) terms.push_back(t);
    };
    if (!tree.standard_no.empty()) add(compact_code(tree.standard_no));
    if (!method_no.empty()) { add(method_no); add(compact_code(method_no)); }

    const std::string& body = n.text;   // 意图词规则只看当前叶子正文
    auto has = [&](const char* kw) { return body.find(kw) != std::string::npos; };
    bool has_qualify = has("合格") || has("不合格");
    bool has_require = has("应") || has("不得") || has("不应") || has("应符合");
    bool has_method  = has("法") && has("合格");
    if (has_qualify) { add("判定"); add("要求"); add("合格"); }
    if (has_require) { add("要求"); add("规定"); }
    if (has_method)  { add("方法"); add("试验方法"); add("判定"); }
    return terms;
}

std::string compose_bm25_text(const ClauseTree& tree, const TreeNode& n,
                              const std::map<std::string, const TreeNode*>& by_id,
                              const std::string& method_no) {
    std::string out;
    if (!tree.standard_no.empty()) {
        append_line(out, "标准：" + tree.standard_no);
        append_line(out, "标准代号：" + tree.standard_no + " " + compact_code(tree.standard_no));
    }
    std::string path = path_text_for(n, by_id);
    if (!path.empty()) append_line(out, "路径：" + path);
    std::string clause = node_label(n);
    if (!clause.empty()) append_line(out, "条款：" + clause);

    std::vector<std::string> terms = compose_bm25_terms(tree, n, method_no);
    if (!terms.empty()) {
        std::string joined;
        for (const auto& t : terms) { if (!joined.empty()) joined += " "; joined += t; }
        append_line(out, "检索词：" + joined);
    }
    // 正文：复用 embedding_text 同源正文，追加在末尾（超长时优先被裁，符合 spec §8）
    std::string body = compose_embedding_text(n);
    if (!body.empty()) {
        if (!out.empty()) out += "\n";
        out += "正文：\n" + body;
    }
    constexpr size_t kMaxChars = 3500;   // 硬上限 < Milvus text.max_length=8192
    if (out.size() > kMaxChars) out = out.substr(0, kMaxChars);
    return out;
}
```

- [ ] **Step 4: 在 `chunk_from_leaf` 装配 bm25_text**

`chunk_from_leaf` 里，在 `c.context_text = compose_context_text(n, by_id);`(:229) 之后加：
```cpp
    c.bm25_text = compose_bm25_text(tree, n, by_id, c.method_no);
```
（`c.method_no` 已于上面 `c.method_no = method_no_for(n, by_id);` 赋值。）

- [ ] **Step 5: 构建 + 测试**

Run MSBuild + `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`。
Expected：新 3 处 bm25_text 断言全绿；`embedding_text` 仍不含标准号/路径（既有断言不回归）；全量绿。

- [ ] **Step 6: Commit**
```powershell
git add src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
git commit -m @'
feat(chunk): compose_bm25_text + 确定性检索词规则 (标准号/路径/条款/意图词)

embedding_text 保持干净;bm25_text 富化供 BM25。检索词=紧凑标准号/方法号+意图词
(合格→判定/要求;法+合格→方法/试验方法)。长度控长 3500,正文末尾优先裁。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/retrieve/retrieval_chunk.cpp tests/test_retrieval_chunk.cpp
```

---

## Task 3: PG row/insert/get + schema.sql

**Files:** Modify `src/db/pg_client.h`, `src/db/pg_client.cpp`, `src/db/schema.sql`

- [ ] **Step 1: `RetrievalChunkRow` 加字段（`src/db/pg_client.h`）**

在 `RetrievalChunkRow` 的 `std::string context_text;` 之后加：
```cpp
    std::string bm25_text;
```

- [ ] **Step 2: `insert_chunk` 写入（`src/db/pg_client.cpp`）**

把 `insert_chunk` 的 SQL 与 params 改为含 `bm25_text`（新增列放末尾 = `$20`）：
- 列清单：在 `...has_figure,suspect)` 改成 `...has_figure,suspect,bm25_text)`。
- VALUES：把 `...$18,$19) ` 改成 `...$18,$19,$20) `。
- ON CONFLICT：在 `suspect=EXCLUDED.suspect` 后加 `, bm25_text=EXCLUDED.bm25_text`。
- params：在 `c.has_figure, c.suspect}` 改成 `c.has_figure, c.suspect, c.bm25_text}`。

改后核对（关键片段）：
```cpp
        "clause_no,method_no,title,path_text,atomic_text,embedding_text,context_text,"
        "captions,formulas,page_start,page_end,has_table,has_formula,has_figure,suspect,bm25_text) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12::jsonb,$13::jsonb,$14,$15,$16,$17,$18,$19,$20) "
        ...
        "has_formula=EXCLUDED.has_formula, has_figure=EXCLUDED.has_figure, "
        "suspect=EXCLUDED.suspect, bm25_text=EXCLUDED.bm25_text",
        pqxx::params{c.chunk_id, c.node_id, c.standard_id, c.chunk_type,
                     c.clause_no, c.method_no, c.title, c.path_text,
                     c.atomic_text, c.embedding_text, c.context_text,
                     c.captions_json, c.formulas_json, c.page_start, c.page_end,
                     c.has_table, c.has_formula, c.has_figure, c.suspect, c.bm25_text});
```

- [ ] **Step 3: `get_chunk` 读出（`src/db/pg_client.cpp`）**

在 SELECT 末列 `COALESCE(suspect,'') ` 后加 `,COALESCE(bm25_text,'') `（成为索引 19）：
```cpp
        "COALESCE(has_figure,FALSE),COALESCE(suspect,''),COALESCE(bm25_text,'') "
        "FROM retrieval_chunks WHERE chunk_id=$1",
```
在赋值段 `c.has_figure = row[17].as<bool>();`（即读完 suspect=row[18] 之后）加：
```cpp
    c.bm25_text = row[19].c_str();
```
（确认 suspect 当前是 `row[18]`；bm25_text 为新末列 `row[19]`。）

- [ ] **Step 4: `schema.sql` 加列 + 幂等迁移**

在 `retrieval_chunks` CREATE TABLE 的 `context_text   TEXT,` 之后加一行：
```sql
    bm25_text      TEXT,
```
在该 CREATE TABLE 语句（及其后两条 CREATE INDEX）之后追加幂等迁移（兼容老库）：
```sql
ALTER TABLE retrieval_chunks ADD COLUMN IF NOT EXISTS bm25_text TEXT;
```

- [ ] **Step 5: 构建 + 测试**

Run MSBuild + `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`。
Expected：0 编译错误；全量 doctest 绿（PG 改动无单测，靠 Task 5 集成验证：chunkload 后 `get_chunk` 能读回 bm25_text）。

- [ ] **Step 6: Commit**
```powershell
git add src/db/pg_client.h src/db/pg_client.cpp src/db/schema.sql
git commit -m @'
feat(db): retrieval_chunks 加 bm25_text (insert/get/schema + 幂等迁移)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/db/pg_client.h src/db/pg_client.cpp src/db/schema.sql
```

---

## Task 4: chunk_loader 用 bm25_text 喂 Milvus BM25

**Files:** Modify `src/ingest/chunk_loader.cpp`; `tests/test_chunk_loader.cpp`

- [ ] **Step 1: 扩 `chunk_to_row` 单测（先红）**

在 `tests/test_chunk_loader.cpp` 第一个用例里：
1. fixture `c.context_text = "路径：…\n正文";` 之后加 `c.bm25_text = "标准：JTC 5210-2018\n检索词：判定";`
2. `CHECK(row.context_text == c.context_text);` 之后加 `CHECK(row.bm25_text == c.bm25_text);`

- [ ] **Step 2: 构建确认失败**

Run MSBuild + 测试 exe。Expected：`row.bm25_text` 未编译/不相等。

- [ ] **Step 3: `chunk_to_row` 映射（`src/ingest/chunk_loader.cpp`）**

在 `row.context_text = c.context_text;` 之后加：
```cpp
    row.bm25_text = c.bm25_text;
```

- [ ] **Step 4: `load_chunks` 改用 bm25_text + schema 守门（`src/ingest/chunk_loader.cpp`）**

在 `load_chunks` 函数体最前（`ChunkLoadResult result;` 之后）加 schema 守门：
```cpp
    if (cache.schema_version < 2) {
        spdlog::error("chunk cache schema_version={} < 2，缺 bm25_text，请用 chunkcheck 重新生成 cache 再 chunkload",
                      cache.schema_version);
        return result;   // 不写入，embedded_count=0
    }
```
把循环体内 `mv.insert_full(...)` 一行的 `c.embedding_text`（第 6 个实参 = BM25 文本）改为 `c.bm25_text`，dense 输入 `embed.embed(c.embedding_text)` **保持不变**。并在 embed 前对空 bm25_text 告警跳过：
```cpp
    for (const auto& c : cache.chunks) {
        pg.insert_chunk(chunk_to_row(c));
        if (c.bm25_text.empty()) {
            spdlog::warn("chunk bm25_text 为空，跳过 Milvus 写入: {}", c.chunk_id);
            continue;
        }
        try {
            std::vector<float> vec = embed.embed(c.embedding_text);   // dense 仍用 embedding_text
            mv.insert_full(collection, c.chunk_id, c.node_id, c.standard_id,
                           status, c.bm25_text, vec);                  // BM25 用 bm25_text
            ++result.embedded_count;
        } catch (const std::exception& e) {
            spdlog::warn("chunk embed/写入失败，跳过: {} ({})", c.chunk_id, e.what());
        }
    }
```

- [ ] **Step 5: 构建 + 测试**

Run MSBuild + `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`。
Expected：`chunk_to_row` bm25_text 断言绿；全量绿。（`load_chunks` 的 insert_full 路由 / schema 守门无单测——`load_chunks` 历来无 fake；由 Task 5 集成验证：rq-081 BM25 命中即证明 bm25_text 进了 BM25 索引。）

- [ ] **Step 6: Commit**
```powershell
git add src/ingest/chunk_loader.cpp tests/test_chunk_loader.cpp
git commit -m @'
feat(ingest): Milvus BM25 文本改用 bm25_text (dense 仍用 embedding_text) + schema v2 守门

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@ -- src/ingest/chunk_loader.cpp tests/test_chunk_loader.cpp
```

---

## Task 5: 重建数据 + 目标 case + 100 题 rich eval 验收

**Files:** 无代码改动（重建 + 验收）。需 Milvus + PG + embedding 在线（见 [[local-services]]）。

- [ ] **Step 1: 确认服务在线**
```powershell
docker ps --filter "name=milvus-standalone" --format "{{.Names}} {{.Status}}"
python -c "import requests;print(requests.post('http://localhost:19530/v2/vectordb/collections/list',headers={'Authorization':'Bearer root:Milvus'},json={},timeout=8).json().get('code'))"
```
Expected：milvus healthy、code 0。PG 在 localhost:5432。`config.json` 的 `RAG_EMBED_KEY` 已填且 SiliconFlow 可用。

- [ ] **Step 2: 应用 schema 迁移（老库加列）**

确保 `bm25_text` 列存在（schema.sql 已含迁移；若有独立 schema apply 命令则跑它，否则用 psql/python 执行）：
```powershell
python -c "import json,psycopg2;cfg=json.load(open('config.json',encoding='utf-8'));c=psycopg2.connect(cfg['RAG_PG_CONNINFO']);cur=c.cursor();cur.execute('ALTER TABLE retrieval_chunks ADD COLUMN IF NOT EXISTS bm25_text TEXT');c.commit();print('bm25_text column ensured')"
```

- [ ] **Step 3: 定位 tree cache 并重建全部标准的 chunk cache**

先找 tree cache 目录：
```powershell
Get-ChildItem data -Recurse -Filter *.json | Where-Object { $_.FullName -match 'tree' } | Select-Object FullName
```
对每个 tree cache 跑 chunkcheck（重新生成带 bm25_text 的 `data/chunk_cache/<sid>.json`，schema v2，不重 embed）：
```powershell
Get-ChildItem data\tree_cache -Filter *.json | ForEach-Object {
    .\rag2.0\x64\Debug\rag2.0.exe chunkcheck $_.FullName
}
```
（若 tree cache 不在 `data\tree_cache`，用 Step 3 第一条找到的实际目录替换。）
抽查一个新 chunk cache 确含 bm25_text 与 schema v2：
```powershell
Select-String -Path (Get-ChildItem data\chunk_cache -Filter *.json | Select-Object -First 1).FullName -Pattern '"schema_version": 2|bm25_text' | Select-Object -First 3
```

- [ ] **Step 4: 重灌全部标准（写 PG + Milvus，BM25 索引重建）**
```powershell
Get-ChildItem data\chunk_cache -Filter *.json | ForEach-Object {
    .\rag2.0\x64\Debug\rag2.0.exe chunkload $_.FullName 2>&1 | Select-String -Pattern "chunkload|embed|count|失败"
}
```
**关键校验**：`load_chunks` 对 embed 失败是 per-chunk 跳过（SiliconFlow 闪崩会静默丢 chunk）。每本标准跑完确认 `embedded_count == chunk_count`；若某本短缺，**对该本重跑 chunkload**（按 standard_id 先删后插，幂等）直到齐全。可事后核对：
```powershell
python -c "import json,psycopg2;cfg=json.load(open('config.json',encoding='utf-8'));c=psycopg2.connect(cfg['RAG_PG_CONNINFO']);cur=c.cursor();cur.execute(\"SELECT count(*), count(bm25_text) FILTER (WHERE bm25_text<>'') FROM retrieval_chunks\");print('rows, with_bm25:',cur.fetchone())"
```
Expected：两数相等（每行都有 bm25_text）。

- [ ] **Step 5: 目标 case 验证（rq-081）**
```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe retrievecheck "通用硅酸盐水泥安定性需要通过哪两种方法判定合格？" 20 2>&1 | Out-String
```
Expected（spec §10.5）：top20 出现 `GB 175-2023 / 7.4.2`（安定性）对应 chunk；BM25 路或 RRF top20 稳定召回它。
**若仍 MISS**：说明确定性检索词不足（如确需"通用硅酸盐水泥"标准名）。记录现象,作为是否给 `compose_bm25_text` 传入干净 standard_name 的依据（回 Task 2 增量,不在本刀强行加）。

- [ ] **Step 6: 100 题 rich eval 非回归**
```powershell
.\rag2.0\x64\Debug\rag2.0.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich 2>&1 | Tee-Object -FilePath logs/eval100_rich_after_bm25.txt | Select-String -Pattern "Group Recall|Complete@|nDCG@|Distractor|Redundancy|rq-081|risk"
```
对照当前 baseline 验收（spec §10.6 + 富指标守门）：
- `rq-081` 从 MISS → HIT（看 `[miss]`/`[risk]` 行不再出现 rq-081，或 complete 改善）。
- `Group Recall@20` ≥ baseline(≈0.987)。
- `Complete@20` ≥ baseline(≈0.98)。
- `nDCG@20` 下降 ≤ 0.01（baseline≈0.78）。
- `Distractor-before-gold` 上升 ≤ 0.02（baseline≈0.08）。
- **`Redundancy@10` 不明显上升**（≤ baseline+0.05;baseline≈0.24）——守 §11.1"加标准号/路径致同标准刷屏"风险。
若任一守门破线，记录数字 + 触发题，回 Task 2 收紧检索词/长度（不在本步硬调 RRF）。

- [ ] **Step 7: 记录结果（不提交大日志）**

`logs/` 已 gitignore。把关键前后对比（rq-081 状态 + 6 项指标）整理成一段，供 README/记忆更新与后续决策（本 Task 不改 README;如需同步 README baseline 另起小改）。

---

## Self-Review

**1. Spec 覆盖：**
- §5.1 新增 `bm25_text` → Task 1。§5.2 `compose_bm25_text` 格式 → Task 2。§5.3 检索词三来源 → Task 2 `compose_bm25_terms`（**标准名来源因 ClauseTree 无 standard_name 且 DB 不可靠而舍弃，已在 Header/§Task5-Step5 标注并由目标 case 兜底**）。§5.4 Milvus 不改名、调用侧传 bm25_text → Task 4。
- §7.1 schema v2 + struct → Task 1。§7.2 compose + chunk_from_leaf + JSON → Task 1/2。§7.3 chunk_loader → Task 4。§7.4 schema.sql + ALTER → Task 3。§7.5 pg row/insert/get、chunks_containing 仍查 embedding_text(不动) → Task 3。§7.6 Milvus 调用侧传 bm25_text → Task 4。
- §8 控长(3500、正文末尾优先裁) → Task 2 `compose_bm25_text` 尾部 substr。
- §9 重建策略(chunkcheck→chunkload) → Task 5 Step 3-4，含 embed 失败重跑校验。
- §10.1 chunk 单测 → Task 1/2。§10.2 round-trip schema v2 → Task 1。§10.3 chunk loader(chunk_to_row 单测;insert_full/embed 路由由集成验) → Task 4。§10.4 PG(集成验) → Task 3/Task5。§10.5 目标 case → Task 5 Step 5。§10.6 全量 eval 阈值 → Task 5 Step 6（+ Redundancy 守门）。
- §11 风险 → Task 4(空 bm25_text 跳过/schema 守门)、Task 5(embed 失败重跑、Redundancy 守门、控长)。§12 验收 → Task 5。

**2. Placeholder 扫描：** 无 TBD/TODO；各 code step 给完整片段与确切命令。Task 5 的 tree cache 目录用"先发现再循环"消除路径不确定性（给了发现命令 + 默认 `data\tree_cache`）。

**3. 类型一致性：** `bm25_text` 贯穿 `RetrievalChunk`(h)→JSON(cpp)→`chunk_from_leaf`→`RetrievalChunkRow`(h)→`chunk_to_row`→`insert_chunk`/`get_chunk`→`load_chunks` insert_full 实参，名称一致。`compose_bm25_text(tree,n,by_id,method_no)`/`compose_bm25_terms(tree,n,method_no)`/`compact_code(s)` 在 Task 2 定义=调用一致，均在 `chunk_from_leaf` 之前的匿名命名空间。中文字面量统一普通 `"..."`（非 u8）。

**4. 已知限制：** ① 检索词无标准名（数据所限）；rq-081 能否 HIT 由 Task 5 Step5 验，不行则回 Task 2 增量传 standard_name。② 重建重 embed 全量 dense（embedding_text 未变、向量同值，但 insert_full 需整行重插）→ 成本 + SiliconFlow 闪崩暴露面；靠 Step4 embed_count 校验 + 重跑兜底。③ `load_chunks` 无 fake，insert_full 路由/ schema 守门靠集成验（与既有测试架构一致）。
