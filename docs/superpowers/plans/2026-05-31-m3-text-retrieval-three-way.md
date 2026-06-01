# M3 文本路三路召回 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 M1/M2 的单路 dense 召回升级为文档正规的文本路：查询理解前置（标准号/条款号归一化 + 同义词扩展 + 自定义分词词典）、dense + BM25 + PG 精确三路召回、RRF 融合、状态+密级统一过滤下推、small-to-big 父级回填。

**Architecture:** 在 M2 结构化底座上扩展召回侧。查询理解、同义词扩展、RRF 融合、过滤表达式构造均为纯逻辑（TDD）。三路召回各实现契约③（Retriever），输出统一归一到契约④（Candidate）。Milvus `clause_text` 集合升级为带 BM25 全文检索（jieba+自定义词典）、稀疏向量、status/access_level 标量过滤字段的完整 schema；状态+密级在 Milvus 标量过滤与 PG 查询双向下推，密级再经 PG 二次校验（§9.6 安全底线）。

**Tech Stack:** 延续 M0–M2。新增依赖于 Milvus 2.5 内置全文检索（BM25 Function + jieba analyzer + 自定义词典），通过 REST v2 配置。同义词/分词词典为受版本管理的文本文件。

---

## 前置条件（来自 M2，必须已完成）

- M2 全部任务完成：`rag2 ingest` 走 v2 管道，PG 中 `clause_nodes`（含 parent/path/node_type）、`retrieval_chunks`、`page_clause_map`、`spec_tables`/`spec_table_cells` 正确写入。
- 已存在：`src/retrieve/candidate.h`（`Candidate`）、`src/retrieve/retriever.h`（`Retriever`）、`src/retrieve/dense_retriever.{h,cpp}`、`src/milvus/milvus_rest.{h,cpp}`、`src/generate/answer_pipeline.{h,cpp}`、`src/ingest/metadata_extractor.{h,cpp}`、`src/ingest/ingest_pipeline.{h,cpp}`、`src/db/pg_client.{h,cpp}`。
- Milvus 为 2.5.x（支持全文检索 BM25 Function 与 analyzer）。

## 关键设计决策（执行前若不同意请先改）

1. **契约③演进（加性参数）：** `Retriever::retrieve` 增加 `RetrievalFilter` 参数（status/access_level/standard_no/clause_no）。这是召回必备的过滤下推（§9.6），属合理演进；同步更新 M1 的 `DenseRetriever` 与 `answer_pipeline` 调用点。
2. **Milvus `clause_text` 升级需 drop+recreate + 重灌。** 新 schema 增 `status`/`access_level` 标量、`text` VarChar（带 jieba analyzer）、`sparse` SparseFloatVector + BM25 Function。执行 M3 时需重建集合并重新 `ingest`。
3. **BM25 用 Milvus 2.5 内置全文检索**（§8.2 首选路径），jieba 分词器加载自定义词典（§9.0.3 硬前置）。
4. **标准号/条款号不进向量召回**（§9.0.1）：查询理解解析出的标准号/条款号作为结构化过滤 + PG 精确匹配，不喂给 dense/BM25。
5. **密级安全底线（§9.6 要点3）：** access_level 既下推 Milvus 标量过滤，又在召回后用 PG 最新值二次校验，任一不通过即剔除。

## 文件结构（本计划新建/修改）

```
修改:
  src/retrieve/retriever.h            契约③加 RetrievalFilter 参数
  src/retrieve/dense_retriever.h/.cpp 适配新签名 + 过滤下推
  src/milvus/milvus_rest.h/.cpp       集合升级、insert_full、dense/bm25 带 filter 搜索
  src/ingest/ingest_pipeline.cpp      写入 status/access_level/text 到 Milvus
  src/db/pg_client.h/.cpp             标准号→id、按条款号精确查、context_text、access_level 校验
  src/generate/answer_pipeline.h/.cpp 接 text 三路融合管道
  src/main.cpp                        query 子命令接新管道
  CMakeLists.txt                      登记新源与测试
新建:
  src/query/query_analysis.h/.cpp     查询理解：解析标准号/条款号/查询类型（纯逻辑）
  src/query/synonyms.h/.cpp           同义词词典加载 + 查询扩展（纯逻辑）
  src/retrieve/retrieval_filter.h     RetrievalFilter 结构 + Milvus 过滤表达式构造（纯逻辑）
  src/retrieve/bm25_retriever.h/.cpp  BM25 召回（契约③）
  src/retrieve/pg_exact_retriever.h/.cpp PG 精确召回（契约③）
  src/retrieve/rrf.h/.cpp             RRF 融合 + 去重（纯逻辑）
  src/retrieve/text_search.h/.cpp     text 模式编排：查询理解→三路→RRF→过滤→small-to-big
  config/synonyms.txt                 领域同义词词典（§9.0.2）
  config/user_dict.txt                jieba 自定义分词词典（§9.0.3）
  tests/test_query_analysis.cpp
  tests/test_synonyms.cpp
  tests/test_retrieval_filter.cpp
  tests/test_rrf.cpp
```

---

### Task 1: 查询理解（解析标准号/条款号/查询类型，纯逻辑 TDD）

**Files:**
- Create: `src/query/query_analysis.h`, `src/query/query_analysis.cpp`
- Test: `tests/test_query_analysis.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_query_analysis.cpp`**

```cpp
#include <doctest/doctest.h>
#include "query/query_analysis.h"

TEST_CASE("analyze_query extracts standard_no and clause_no") {
    QueryAnalysis a = analyze_query("JTG D60-2015 第4.2.1条对桥涵设计有什么要求？");
    CHECK(a.standard_no == "JTG D60-2015");
    CHECK(a.clause_no == "4.2.1");
    CHECK(a.has_exact_key());
}

TEST_CASE("analyze_query detects table/limit intent") {
    QueryAnalysis a = analyze_query("高速公路路基压实度的限值是多少？");
    CHECK(a.standard_no.empty());
    CHECK(a.wants_table);          // 含"限值"
    CHECK_FALSE(a.has_exact_key());
}

TEST_CASE("analyze_query keeps original text for semantic recall") {
    QueryAnalysis a = analyze_query("桥梁伸缩缝的构造要求");
    CHECK(a.clean_text == "桥梁伸缩缝的构造要求");
    CHECK_FALSE(a.wants_table);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/query/query_analysis.h`**

```cpp
#pragma once
#include <string>

struct QueryAnalysis {
    std::string clean_text;    // 原始问题（用于 dense/BM25 语义召回）
    std::string standard_no;   // 归一化后的标准号（空=未指定）
    std::string clause_no;     // 归一化后的条款号（空=未指定）
    bool wants_table = false;  // 含"表/限值/指标/单位"等，倾向表格子流程

    bool has_exact_key() const { return !standard_no.empty() || !clause_no.empty(); }
};

// 查询理解（§9.0.1）：抽取并归一化标准号/条款号，判定表格意图。
QueryAnalysis analyze_query(const std::string& question);
```

- [ ] **Step 4: 写 `src/query/query_analysis.cpp`**

```cpp
#include "query/query_analysis.h"
#include "ingest/metadata_extractor.h"
#include <regex>

QueryAnalysis analyze_query(const std::string& question) {
    QueryAnalysis a;
    a.clean_text = question;

    // 复用 M2 的标准号抽取
    a.standard_no = extract_standard_no(question);

    // 条款号：支持 "第4.2.1条" / "4.2.1"
    std::smatch m;
    if (std::regex_search(question, m,
        std::regex(R"((?:第)?\s*(\d+\.\d+(?:\.\d+)?(?:-[0-9a-zA-Z]+)?)\s*条?)"))) {
        std::string c = m[1].str();
        if (is_valid_clause_no(c)) a.clause_no = c;
    }

    // 表格/限值意图
    static const std::regex table_kw(R"(表|限值|指标|单位|数值|取值|不应小于|不应大于)");
    a.wants_table = std::regex_search(question, table_kw);

    return a;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/query/query_analysis.cpp`；`rag_tests` 加 `tests/test_query_analysis.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/query/query_analysis.h src/query/query_analysis.cpp tests/test_query_analysis.cpp CMakeLists.txt
git commit -m "feat(m3): query understanding (standard/clause no + table intent) (TDD)"
```

---

### Task 2: 同义词词典 + 查询扩展（纯逻辑 TDD）

**Files:**
- Create: `src/query/synonyms.h`, `src/query/synonyms.cpp`
- Create: `config/synonyms.txt`
- Test: `tests/test_synonyms.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_synonyms.cpp`**

```cpp
#include <doctest/doctest.h>
#include "query/synonyms.h"

TEST_CASE("SynonymDict expands query terms with known aliases") {
    SynonymDict dict;
    dict.load_from_lines({
        "沥青混合料,AC,沥青砼",
        "压实度,密实度"
    });
    std::string expanded = dict.expand("测量压实度和沥青混合料");
    CHECK(expanded.find("密实度") != std::string::npos);
    CHECK(expanded.find("AC") != std::string::npos);
    // 原词保留
    CHECK(expanded.find("压实度") != std::string::npos);
}

TEST_CASE("SynonymDict expand is a no-op when no alias matches") {
    SynonymDict dict;
    dict.load_from_lines({"压实度,密实度"});
    CHECK(dict.expand("桥梁支座") == "桥梁支座");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/query/synonyms.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>

// 领域同义词词典（§9.0.2）。每行：词1,词2,词3...（互为同义）。
// 用于 BM25 路查询扩展，与入库术语库共用同一份（§9.0.2 要点3）。
class SynonymDict {
public:
    void load_from_lines(const std::vector<std::string>& lines);
    void load_from_file(const std::string& path);
    // 在 query 后追加命中的同义词（空格分隔），原词保留。
    std::string expand(const std::string& query) const;
private:
    std::map<std::string, std::vector<std::string>> alias_;  // 词 -> 同组其他词
};
```

- [ ] **Step 4: 写 `src/query/synonyms.cpp`**

```cpp
#include "query/synonyms.h"
#include <fstream>
#include <sstream>

static std::vector<std::string> split_commas(const std::string& line) {
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
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) lines.push_back(line);
    load_from_lines(lines);
}

std::string SynonymDict::expand(const std::string& query) const {
    std::string extra;
    for (const auto& kv : alias_) {
        if (query.find(kv.first) != std::string::npos) {
            for (const auto& syn : kv.second)
                if (query.find(syn) == std::string::npos) extra += " " + syn;
        }
    }
    return extra.empty() ? query : query + extra;
}
```

- [ ] **Step 5: 写 `config/synonyms.txt`（初始词典，可后续扩充）**

```
# 每行一组同义词，逗号分隔。与入库术语库共用。
沥青混合料,AC,沥青砼
压实度,密实度
回弹模量,弹性模量
抗压强度,立方体抗压强度
```

- [ ] **Step 6: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/query/synonyms.cpp`；`rag_tests` 加 `tests/test_synonyms.cpp`。

- [ ] **Step 7: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 8: Commit**

```
git add src/query/synonyms.h src/query/synonyms.cpp config/synonyms.txt tests/test_synonyms.cpp CMakeLists.txt
git commit -m "feat(m3): synonym dictionary and query expansion (§9.0.2) (TDD)"
```

---

### Task 3: 过滤器结构 + Milvus 过滤表达式构造（纯逻辑 TDD）

**Files:**
- Create: `src/retrieve/retrieval_filter.h`, `src/retrieve/retrieval_filter.cpp`
- Test: `tests/test_retrieval_filter.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_retrieval_filter.cpp`**

```cpp
#include <doctest/doctest.h>
#include "retrieve/retrieval_filter.h"

TEST_CASE("default filter restricts to current status and public access") {
    RetrievalFilter f;
    CHECK(f.status == "现行");
    CHECK(f.access_level == "public");
}

TEST_CASE("to_milvus_expr builds status and access_level predicate") {
    RetrievalFilter f;
    f.status = "现行";
    f.access_level = "public";
    std::string expr = to_milvus_expr(f);
    CHECK(expr.find("status == \"现行\"") != std::string::npos);
    CHECK(expr.find("access_level == \"public\"") != std::string::npos);
    CHECK(expr.find(" and ") != std::string::npos);
}

TEST_CASE("to_milvus_expr narrows by standard_id when provided") {
    RetrievalFilter f;
    f.standard_id = "STD1";
    std::string expr = to_milvus_expr(f);
    CHECK(expr.find("standard_id == \"STD1\"") != std::string::npos);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/retrieve/retrieval_filter.h`**

```cpp
#pragma once
#include <string>

// §9.6 统一过滤条件：状态 + 密级为不可绕过基线；标准/条款用于精确收窄。
struct RetrievalFilter {
    std::string status = "现行";        // 默认只召回现行
    std::string access_level = "public"; // 请求方密级
    std::string standard_id;            // 可选：限定标准
    std::string clause_no;              // 可选：限定条款号（PG 精确路用）
};

// 构造 Milvus 标量过滤表达式（status + access_level [+ standard_id]）。
std::string to_milvus_expr(const RetrievalFilter& f);
```

- [ ] **Step 4: 写 `src/retrieve/retrieval_filter.cpp`**

```cpp
#include "retrieve/retrieval_filter.h"

std::string to_milvus_expr(const RetrievalFilter& f) {
    std::string expr = "status == \"" + f.status + "\""
                       " and access_level == \"" + f.access_level + "\"";
    if (!f.standard_id.empty())
        expr += " and standard_id == \"" + f.standard_id + "\"";
    return expr;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/retrieve/retrieval_filter.cpp`；`rag_tests` 加 `tests/test_retrieval_filter.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/retrieve/retrieval_filter.h src/retrieve/retrieval_filter.cpp tests/test_retrieval_filter.cpp CMakeLists.txt
git commit -m "feat(m3): RetrievalFilter + Milvus scalar filter expr (§9.6) (TDD)"
```

---

### Task 4: RRF 融合 + 去重（纯逻辑 TDD）

**Files:**
- Create: `src/retrieve/rrf.h`, `src/retrieve/rrf.cpp`
- Test: `tests/test_rrf.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_rrf.cpp`**

```cpp
#include <doctest/doctest.h>
#include "retrieve/rrf.h"

static Candidate cand(const std::string& sid, const std::string& cid,
                      float score, const std::string& src) {
    Candidate c; c.standard_id=sid; c.clause_id=cid; c.score=score; c.source=src;
    return c;
}

TEST_CASE("rrf_fuse merges per-list ranks and dedups by clause_id") {
    std::vector<Candidate> dense = {
        cand("S","S:4.2.1",0.9f,"dense"), cand("S","S:4.3.1",0.8f,"dense") };
    std::vector<Candidate> bm25 = {
        cand("S","S:4.3.1",5.0f,"bm25"), cand("S","S:1.0.1",4.0f,"bm25") };

    auto fused = rrf_fuse({dense, bm25}, /*k=*/60, /*top_k=*/10);

    // 4.3.1 同时出现在两路，RRF 分应最高，排第一
    REQUIRE(fused.size() == 3);
    CHECK(fused[0].clause_id == "S:4.3.1");
    // 去重后每个 clause_id 唯一
    CHECK(fused[0].source.find("dense") != std::string::npos);
    CHECK(fused[0].source.find("bm25") != std::string::npos);
}

TEST_CASE("rrf_fuse respects top_k truncation") {
    std::vector<Candidate> a = {
        cand("S","S:1",1,"dense"), cand("S","S:2",1,"dense"), cand("S","S:3",1,"dense") };
    auto fused = rrf_fuse({a}, 60, 2);
    CHECK(fused.size() == 2);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/retrieve/rrf.h`**

```cpp
#pragma once
#include <vector>
#include "retrieve/candidate.h"

// §10.1 RRF：对多路候选按各自排名融合（score = Σ 1/(k+rank)），
// 按 clause_id 去重合并，合并来源标记，截断 top_k。
std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k);
```

- [ ] **Step 4: 写 `src/retrieve/rrf.cpp`**

```cpp
#include "retrieve/rrf.h"
#include <map>
#include <algorithm>

std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k) {
    struct Acc { Candidate c; double score = 0.0; std::string sources; };
    std::map<std::string, Acc> by_id;   // clause_id -> 累积

    for (const auto& list : lists) {
        for (size_t rank = 0; rank < list.size(); ++rank) {
            const Candidate& c = list[rank];
            auto& a = by_id[c.clause_id];
            if (a.sources.empty()) { a.c = c; a.c.source = ""; }
            a.score += 1.0 / (k + (int)rank + 1);
            if (a.sources.find(c.source) == std::string::npos)
                a.sources += (a.sources.empty() ? "" : "+") + c.source;
        }
    }

    std::vector<Candidate> out;
    for (auto& kv : by_id) {
        Candidate c = kv.second.c;
        c.score = (float)kv.second.score;
        c.source = kv.second.sources;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(),
              [](const Candidate& a, const Candidate& b){ return a.score > b.score; });
    if ((int)out.size() > top_k) out.resize(top_k);
    return out;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/retrieve/rrf.cpp`；`rag_tests` 加 `tests/test_rrf.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/retrieve/rrf.h src/retrieve/rrf.cpp tests/test_rrf.cpp CMakeLists.txt
git commit -m "feat(m3): RRF fusion with clause_id dedup and source merge (§10.1) (TDD)"
```

---

### Task 5: 契约③演进 + Milvus 集合升级（全文检索 + 标量 + 带 filter 搜索）

**Files:**
- Modify: `src/retrieve/retriever.h`
- Modify: `src/milvus/milvus_rest.h`, `src/milvus/milvus_rest.cpp`
- Create: `config/user_dict.txt`

- [ ] **Step 1: 演进契约③ `src/retrieve/retriever.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "retrieve/retrieval_filter.h"

// 契约③（M3 演进）：query + 过滤条件 → 候选列表。
// dense / bm25 / pg_exact 均实现本接口，RRF 在其输出上融合。
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<Candidate> retrieve(const std::string& query,
                                            const RetrievalFilter& filter,
                                            int top_k) = 0;
};
```

- [ ] **Step 2: 写 `config/user_dict.txt`（jieba 自定义分词词典，§9.0.3）**

```
JTG/T
GB/T
JTG
压实度
沥青混合料
回弹模量
抗压强度
伸缩缝
桥涵
```

- [ ] **Step 3: 在 `src/milvus/milvus_rest.h` 增加升级版集合与带 filter 搜索声明**

在 `class MilvusRest` public 段追加：
```cpp
    // M3：带全文检索(BM25)+标量过滤+稀疏向量的完整 clause_text 集合。
    void drop_collection(const std::string& collection);
    void ensure_collection_text(const std::string& collection, int dim,
                                const std::vector<std::string>& user_dict);
    void insert_full(const std::string& collection, const std::string& node_id,
                     const std::string& standard_id, const std::string& status,
                     const std::string& access_level, const std::string& text,
                     const std::vector<float>& dense);
    std::vector<Hit> search_dense(const std::string& collection,
                                  const std::vector<float>& query,
                                  const std::string& filter_expr, int top_k);
    std::vector<Hit> search_bm25(const std::string& collection,
                                 const std::string& query_text,
                                 const std::string& filter_expr, int top_k);
```

- [ ] **Step 4: 在 `src/milvus/milvus_rest.cpp` 实现（追加到文件末尾）**

```cpp
void MilvusRest::drop_collection(const std::string& collection) {
    json q; q["collectionName"] = collection;
    http::post_json(base_url_, "/v2/vectordb/collections/drop", q.dump(),
                    auth_headers(token_));
}

void MilvusRest::ensure_collection_text(const std::string& collection, int dim,
                                        const std::vector<std::string>& user_dict) {
    { // 已存在则返回
        json q; q["collectionName"] = collection;
        auto has = http::post_json(base_url_, "/v2/vectordb/collections/has", q.dump(),
                                   auth_headers(token_));
        if (has.ok()) {
            auto j = json::parse(has.body, nullptr, false);
            if (!j.is_discarded() && j.contains("data") &&
                j["data"].value("has", false)) return;
        }
    }
    json analyzer_params;
    analyzer_params["tokenizer"] = { {"type","jieba"}, {"dict", user_dict} };

    json fields = json::array({
        { {"fieldName","node_id"}, {"dataType","VarChar"}, {"isPrimary",true},
          {"elementTypeParams", { {"max_length",256} }} },
        { {"fieldName","standard_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length",128} }} },
        { {"fieldName","status"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length",32} }} },
        { {"fieldName","access_level"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length",32} }} },
        { {"fieldName","text"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length",8192}, {"enable_analyzer",true},
                                  {"analyzer_params", analyzer_params} }} },
        { {"fieldName","dense"}, {"dataType","FloatVector"},
          {"elementTypeParams", { {"dim",dim} }} },
        { {"fieldName","sparse"}, {"dataType","SparseFloatVector"} }
    });
    json functions = json::array({
        { {"name","bm25_fn"}, {"type","BM25"},
          {"inputFieldNames", json::array({"text"})},
          {"outputFieldNames", json::array({"sparse"})} }
    });
    json index = json::array({
        { {"fieldName","dense"}, {"indexName","dense_idx"}, {"metricType","COSINE"} },
        { {"fieldName","sparse"}, {"indexName","sparse_idx"}, {"metricType","BM25"} }
    });
    json schema;
    schema["autoID"] = false;
    schema["fields"] = fields;
    schema["functions"] = functions;

    json body;
    body["collectionName"] = collection;
    body["schema"] = schema;
    body["indexParams"] = index;
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/create", body.dump(),
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("create text collection failed: " + res.body + res.error);
}

void MilvusRest::insert_full(const std::string& collection, const std::string& node_id,
                             const std::string& standard_id, const std::string& status,
                             const std::string& access_level, const std::string& text,
                             const std::vector<float>& dense) {
    json row;
    row["node_id"] = node_id;
    row["standard_id"] = standard_id;
    row["status"] = status;
    row["access_level"] = access_level;
    row["text"] = text;          // sparse 由 BM25 Function 自动生成，无需传
    row["dense"] = dense;
    json body; body["collectionName"] = collection; body["data"] = json::array({row});
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/insert", body.dump(),
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("insert_full failed: " + res.body + res.error);
}

static std::vector<milvus::Hit> parse_hits(const std::string& body) {
    auto j = nlohmann::json::parse(body);
    std::vector<milvus::Hit> hits;
    for (auto& item : j["data"]) {
        milvus::Hit h;
        h.node_id = item.value("node_id", "");
        h.standard_id = item.value("standard_id", "");
        h.score = item.value("distance", 0.0f);
        hits.push_back(h);
    }
    return hits;
}

std::vector<Hit> MilvusRest::search_dense(const std::string& collection,
                                          const std::vector<float>& query,
                                          const std::string& filter_expr, int top_k) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query});
    body["annsField"] = "dense";
    body["limit"] = top_k;
    body["filter"] = filter_expr;
    body["outputFields"] = json::array({"node_id","standard_id"});
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body.dump(),
                               auth_headers(token_));
    if (!res.ok()) throw std::runtime_error("search_dense failed: " + res.body + res.error);
    return parse_hits(res.body);
}

std::vector<Hit> MilvusRest::search_bm25(const std::string& collection,
                                         const std::string& query_text,
                                         const std::string& filter_expr, int top_k) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query_text});   // 原始文本，BM25 Function 处理
    body["annsField"] = "sparse";
    body["limit"] = top_k;
    body["filter"] = filter_expr;
    body["outputFields"] = json::array({"node_id","standard_id"});
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body.dump(),
                               auth_headers(token_));
    if (!res.ok()) throw std::runtime_error("search_bm25 failed: " + res.body + res.error);
    return parse_hits(res.body);
}
```

- [ ] **Step 5: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 编译失败——M1 的 `DenseRetriever` 仍用旧 `retrieve(query, top_k)` 签名。下一个任务修复。

- [ ] **Step 6: Commit（先提交接口与 Milvus 升级，编译修复在 Task 6）**

```
git add src/retrieve/retriever.h src/milvus/milvus_rest.h src/milvus/milvus_rest.cpp config/user_dict.txt
git commit -m "feat(m3): evolve Retriever contract + Milvus full-text collection (BM25+jieba+scalars)"
```

---

### Task 6: 三个检索器实现（dense 适配 / BM25 / PG 精确，契约③）

**Files:**
- Modify: `src/retrieve/dense_retriever.h`, `src/retrieve/dense_retriever.cpp`
- Create: `src/retrieve/bm25_retriever.h`, `src/retrieve/bm25_retriever.cpp`
- Create: `src/retrieve/pg_exact_retriever.h`, `src/retrieve/pg_exact_retriever.cpp`
- Modify: `src/db/pg_client.h`, `src/db/pg_client.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 修改 `src/retrieve/dense_retriever.h`（适配新签名）**

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

- [ ] **Step 2: 修改 `src/retrieve/dense_retriever.cpp`**

```cpp
#include "retrieve/dense_retriever.h"

DenseRetriever::DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed,
                               std::string collection)
    : mv_(mv), embed_(embed), collection_(std::move(collection)) {}

std::vector<Candidate> DenseRetriever::retrieve(const std::string& query,
                                                const RetrievalFilter& filter, int top_k) {
    std::vector<float> qv = embed_.embed(query);
    auto hits = mv_.search_dense(collection_, qv, to_milvus_expr(filter), top_k);
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id; c.clause_id = h.node_id;
        c.score = h.score; c.source = "dense";
        out.push_back(c);
    }
    return out;
}
```

- [ ] **Step 3: 写 `src/retrieve/bm25_retriever.h` 与 `.cpp`**

`bm25_retriever.h`:
```cpp
#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "query/synonyms.h"

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
`bm25_retriever.cpp`:
```cpp
#include "retrieve/bm25_retriever.h"

Bm25Retriever::Bm25Retriever(milvus::MilvusRest& mv, const SynonymDict& syn,
                             std::string collection)
    : mv_(mv), syn_(syn), collection_(std::move(collection)) {}

std::vector<Candidate> Bm25Retriever::retrieve(const std::string& query,
                                               const RetrievalFilter& filter, int top_k) {
    std::string expanded = syn_.expand(query);   // §9.0.2 同义词扩展仅作用于 BM25 路
    auto hits = mv_.search_bm25(collection_, expanded, to_milvus_expr(filter), top_k);
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id; c.clause_id = h.node_id;
        c.score = h.score; c.source = "bm25";
        out.push_back(c);
    }
    return out;
}
```

- [ ] **Step 4: 在 `src/db/pg_client.h` 增加精确查询方法声明**

在 `class PgClient` public 段追加：
```cpp
    // 标准号 → standard_id（取现行优先；找不到返回空）
    std::string standard_id_by_no(const std::string& standard_no);
    // 按 standard_id + clause_no 精确取条款 node_id 列表
    std::vector<std::string> node_ids_by_clause(const std::string& standard_id,
                                                const std::string& clause_no);
    // 取条款的父级上下文文本（retrieval_chunks.context_text）
    std::string context_text_of(const std::string& node_id);
    // 取标准的 access_level（密级二次校验用）
    std::string access_level_of(const std::string& standard_id);
```

- [ ] **Step 5: 在 `src/db/pg_client.cpp` 实现（追加到末尾）**

```cpp
std::string PgClient::standard_id_by_no(const std::string& standard_no) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT standard_id FROM standards WHERE standard_no=$1 "
        "ORDER BY (status='现行') DESC LIMIT 1", standard_no);
    return r.empty() ? "" : std::string(r[0][0].c_str());
}

std::vector<std::string> PgClient::node_ids_by_clause(const std::string& standard_id,
                                                      const std::string& clause_no) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT node_id FROM clause_nodes WHERE standard_id=$1 AND clause_no=$2",
        standard_id, clause_no);
    std::vector<std::string> ids;
    for (auto row : r) ids.push_back(row[0].c_str());
    return ids;
}

std::string PgClient::context_text_of(const std::string& node_id) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT context_text FROM retrieval_chunks WHERE node_id=$1 LIMIT 1", node_id);
    return r.empty() ? "" : std::string(r[0][0].c_str());
}

std::string PgClient::access_level_of(const std::string& standard_id) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT COALESCE(access_level,'public') FROM standards WHERE standard_id=$1",
        standard_id);
    return r.empty() ? "public" : std::string(r[0][0].c_str());
}
```

- [ ] **Step 6: 写 `src/retrieve/pg_exact_retriever.h` 与 `.cpp`**

`pg_exact_retriever.h`:
```cpp
#pragma once
#include "retrieve/retriever.h"
#include "db/pg_client.h"

// PG 精确召回（§9.2）：当 filter 带 standard_no/clause_no 时，直接命中条款节点。
// 注意：标准号/条款号不进向量召回（§9.0.1），仅走此路。
class PgExactRetriever : public Retriever {
public:
    PgExactRetriever(PgClient& pg, std::string standard_no, std::string clause_no);
    std::vector<Candidate> retrieve(const std::string& query,
                                    const RetrievalFilter& filter, int top_k) override;
private:
    PgClient& pg_;
    std::string standard_no_, clause_no_;
};
```
`pg_exact_retriever.cpp`:
```cpp
#include "retrieve/pg_exact_retriever.h"

PgExactRetriever::PgExactRetriever(PgClient& pg, std::string standard_no,
                                   std::string clause_no)
    : pg_(pg), standard_no_(std::move(standard_no)), clause_no_(std::move(clause_no)) {}

std::vector<Candidate> PgExactRetriever::retrieve(const std::string&,
                                                  const RetrievalFilter&, int) {
    std::vector<Candidate> out;
    if (standard_no_.empty() && clause_no_.empty()) return out;

    std::string sid = standard_no_.empty() ? "" : pg_.standard_id_by_no(standard_no_);
    if (!clause_no_.empty()) {
        // 有条款号：在指定标准（或全部标准）内精确匹配
        std::vector<std::string> ids =
            sid.empty() ? std::vector<std::string>{} : pg_.node_ids_by_clause(sid, clause_no_);
        for (auto& id : ids) {
            Candidate c; c.standard_id = sid; c.clause_id = id;
            c.score = 1.0f; c.source = "pg_exact"; out.push_back(c);
        }
    }
    return out;
}
```

- [ ] **Step 7: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/retrieve/bm25_retriever.cpp`、`src/retrieve/pg_exact_retriever.cpp`。

- [ ] **Step 8: 构建验证（编译应恢复正常）**

Run: `cmake --build build --config Debug`
Expected: 成功（含 M1 `answer_pipeline` 调用 `DenseRetriever`——下一个任务才改它的调用，但旧 `answer_query` 用的是 `retriever.retrieve(question, top_k)` 旧签名，现已不存在，故此处会报错）。

> 注：若此步因 `answer_pipeline.cpp` 调用旧签名而编译失败，先继续 Task 7（它重写 answer 管道）再统一构建。两任务一起提交也可。

- [ ] **Step 9: Commit**

```
git add src/retrieve/dense_retriever.h src/retrieve/dense_retriever.cpp src/retrieve/bm25_retriever.h src/retrieve/bm25_retriever.cpp src/retrieve/pg_exact_retriever.h src/retrieve/pg_exact_retriever.cpp src/db/pg_client.h src/db/pg_client.cpp CMakeLists.txt
git commit -m "feat(m3): dense(adapted)+BM25+PG-exact retrievers (contract 3)"
```

---

### Task 7: text 模式编排（查询理解→三路→RRF→密级二次校验→small-to-big→生成）

**Files:**
- Create: `src/retrieve/text_search.h`, `src/retrieve/text_search.cpp`
- Modify: `src/generate/answer_pipeline.h`, `src/generate/answer_pipeline.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写 `src/retrieve/text_search.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"
#include "retrieve/retrieval_filter.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "query/synonyms.h"
#include "db/pg_client.h"

// text 模式三路召回 + RRF + 密级二次校验，返回融合后候选（已过 PG 密级校验）。
std::vector<Candidate> text_retrieve(const std::string& question,
                                     milvus::MilvusRest& mv,
                                     EmbeddingClient& embed,
                                     const SynonymDict& syn,
                                     PgClient& pg,
                                     const std::string& collection,
                                     const RetrievalFilter& base_filter,
                                     int per_path_k, int fused_k);
```

- [ ] **Step 2: 写 `src/retrieve/text_search.cpp`**

```cpp
#include "retrieve/text_search.h"
#include "retrieve/dense_retriever.h"
#include "retrieve/bm25_retriever.h"
#include "retrieve/pg_exact_retriever.h"
#include "retrieve/rrf.h"
#include "query/query_analysis.h"

std::vector<Candidate> text_retrieve(const std::string& question, milvus::MilvusRest& mv,
                                     EmbeddingClient& embed, const SynonymDict& syn,
                                     PgClient& pg, const std::string& collection,
                                     const RetrievalFilter& base_filter,
                                     int per_path_k, int fused_k) {
    QueryAnalysis qa = analyze_query(question);

    // 过滤：标准号若解析出，收窄到对应 standard_id（§9.0.1 标准号走标量过滤）
    RetrievalFilter f = base_filter;
    if (!qa.standard_no.empty()) {
        std::string sid = pg.standard_id_by_no(qa.standard_no);
        if (!sid.empty()) f.standard_id = sid;
    }

    DenseRetriever dense(mv, embed, collection);
    Bm25Retriever bm25(mv, syn, collection);
    PgExactRetriever exact(pg, qa.standard_no, qa.clause_no);

    std::vector<std::vector<Candidate>> lists;
    lists.push_back(dense.retrieve(qa.clean_text, f, per_path_k));
    lists.push_back(bm25.retrieve(qa.clean_text, f, per_path_k));
    lists.push_back(exact.retrieve(qa.clean_text, f, per_path_k));

    auto fused = rrf_fuse(lists, /*k=*/60, fused_k);

    // §9.6 密级二次校验：用 PG 最新 access_level 复核，不通过即剔除
    std::vector<Candidate> safe;
    for (auto& c : fused) {
        std::string al = pg.access_level_of(c.standard_id);
        if (al == f.access_level || f.access_level == "admin")
            safe.push_back(c);
    }
    return safe;
}
```

- [ ] **Step 3: 重写 `src/generate/answer_pipeline.h`**

```cpp
#pragma once
#include <string>
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "query/synonyms.h"
#include "db/pg_client.h"
#include "generate/deepseek_client.h"
#include "retrieve/retrieval_filter.h"

// text 模式问答：三路召回融合 → PG 回查权威字段 → small-to-big 父级回填
// → ContextFragment → DeepSeek 带溯源回答。候选为空直接拒答。
std::string answer_query_text(const std::string& question,
                              milvus::MilvusRest& mv, EmbeddingClient& embed,
                              const SynonymDict& syn, PgClient& pg,
                              deepseek::DeepSeekClient& ds,
                              const std::string& collection,
                              const RetrievalFilter& filter, int top_k);
```

- [ ] **Step 4: 重写 `src/generate/answer_pipeline.cpp`**

```cpp
#include "generate/answer_pipeline.h"
#include "generate/context.h"
#include "generate/prompt_builder.h"
#include "retrieve/text_search.h"
#include <vector>

std::string answer_query_text(const std::string& question, milvus::MilvusRest& mv,
                              EmbeddingClient& embed, const SynonymDict& syn, PgClient& pg,
                              deepseek::DeepSeekClient& ds, const std::string& collection,
                              const RetrievalFilter& filter, int top_k) {
    auto candidates = text_retrieve(question, mv, embed, syn, pg, collection,
                                    filter, /*per_path_k=*/top_k * 4, top_k);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        auto clause = pg.get_clause(c.clause_id);       // 底座原则：PG 权威
        if (!clause) continue;
        auto std_row = pg.get_standard(clause->standard_id);

        ContextFragment f;
        f.source_id = "S" + std::to_string(idx++);
        f.standard_no = std_row ? std_row->standard_no : "";
        f.standard_name = std_row ? std_row->standard_name : "";
        f.status = std_row ? std_row->status : "";
        f.clause_no = clause->clause_no;
        f.path = clause->path;
        f.is_mandatory = false;
        f.text = clause->text;
        // small-to-big：父级上下文并入 text（§10.4）
        std::string ctx = pg.context_text_of(c.clause_id);
        if (!ctx.empty() && ctx != clause->text)
            f.text += "\n【父级上下文】" + ctx;
        fragments.push_back(std::move(f));
    }
    if (fragments.empty())
        return "检索命中但回查规范原文为空，无法作答。";

    return ds.chat(build_system_prompt(), build_user_prompt(question, fragments));
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/retrieve/text_search.cpp`。

- [ ] **Step 6: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功（旧 `answer_query` 已被 `answer_query_text` 取代；下一个任务更新 main 调用）。

- [ ] **Step 7: Commit**

```
git add src/retrieve/text_search.h src/retrieve/text_search.cpp src/generate/answer_pipeline.h src/generate/answer_pipeline.cpp CMakeLists.txt
git commit -m "feat(m3): text-mode orchestration (3-way + RRF + access recheck + small-to-big)"
```

---

### Task 8: 入库写入 Milvus 完整字段 + 升级集合

**Files:**
- Modify: `src/ingest/ingest_pipeline.cpp`

- [ ] **Step 1: 修改 `ingest_file_v2` 中 Milvus 相关两处**

把"通过门禁"分支里的：
```cpp
    mv.ensure_collection(collection, embed.dim());
    for (auto& c : chunks) {
        std::vector<float> vec = embed.embed(c.retrieval_text);
        mv.insert(collection, c.node_id, c.standard_id, vec);
    }
```
替换为：
```cpp
    // M3：升级集合（含 BM25/标量/稀疏）。user_dict 从 config 读取。
    extern std::vector<std::string> load_user_dict();   // 见 Step 2
    mv.ensure_collection_text(collection, embed.dim(), load_user_dict());
    std::string access_level = s.access_level.empty() ? "public" : s.access_level;
    for (auto& c : chunks) {
        std::vector<float> vec = embed.embed(c.retrieval_text);
        // text 字段写入 retrieval_text 供 BM25；status/access_level 供过滤
        mv.insert_full(collection, c.node_id, c.standard_id, "现行",
                       access_level, c.retrieval_text, vec);
    }
```

> 注：`StandardRow` 需有 `access_level` 字段。若 M2 未加，在 `src/db/pg_client.h` 的 `StandardRow` 增 `std::string access_level;`，并在 `upsert_standard` 写入（默认 "public"）。本步若发现缺失则补上。

- [ ] **Step 2: 在 `ingest_pipeline.cpp` 顶部加 user_dict 读取辅助**

在文件 include 区后加入：
```cpp
#include <fstream>
std::vector<std::string> load_user_dict() {
    std::vector<std::string> dict;
    std::ifstream f("config/user_dict.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t b = line.find_last_not_of(" \t\r");
        dict.push_back(line.substr(a, b - a + 1));
    }
    return dict;
}
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/ingest/ingest_pipeline.cpp src/db/pg_client.h src/db/pg_client.cpp
git commit -m "feat(m3): ingest writes full Milvus fields (status/access/text) on upgraded collection"
```

---

### Task 9: 更新 query 子命令 + 端到端验证（M3 验收）

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: 在 `main.cpp` include 区追加**

```cpp
#include "query/synonyms.h"
#include "retrieve/retrieval_filter.h"
```

- [ ] **Step 2: 替换 `cmd_query` 为三路融合版**

```cpp
static int cmd_query(const Config& cfg, const std::string& question) {
    PgClient pg(cfg.pg_conninfo);
    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    SynonymDict syn;
    syn.load_from_file("config/synonyms.txt");
    deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                cfg.deepseek_model, cfg.deepseek_key);

    RetrievalFilter filter;   // 默认 status=现行, access_level=public
    std::string ans = answer_query_text(question, mv, embed, syn, pg, ds,
                                        cfg.milvus_collection, filter, /*top_k=*/5);
    std::cout << "\n===== 回答 =====\n" << ans << "\n";
    return 0;
}
```

> 同时删除 `main.cpp` 中对旧 `answer_query` / `DenseRetriever retriever(...)` 的引用（若有），改为上面的实现。`#include "generate/answer_pipeline.h"` 已在 M1 引入，保留。

- [ ] **Step 3: 重建 Milvus 集合并重灌（schema 变更，必须重建）**

Run（PowerShell；先删旧集合再入库）:
```
cmake --build build --config Debug
# 用一次性小程序或在 ingest 前删除：这里通过重新 ingest 触发 ensure_collection_text；
# 若旧 clause_text 是 M1 简单 schema，需先手动 drop：
# 用 REST 删除（或在 Milvus 控制台删除 clause_text 集合）
Invoke-RestMethod -Method Post -Uri "$($env:RAG_MILVUS_BASE_URL)/v2/vectordb/collections/drop" `
  -Headers @{ Authorization = "Bearer $($env:RAG_MILVUS_TOKEN)" } `
  -ContentType "application/json" -Body '{"collectionName":"clause_text"}'
.\build\Debug\rag2.exe ingest
```
Expected: 入库日志显示已索引；新集合带 BM25/标量字段。

- [ ] **Step 4: 三路召回端到端验证**

Run（分别覆盖三类查询）:
```
.\build\Debug\rag2.exe query "JTG <你的标准号> 第<X.X.X>条的规定是什么？"   # 命中 PG 精确路
.\build\Debug\rag2.exe query "<某材料><某指标>的要求"                        # 命中 BM25/同义词
.\build\Debug\rag2.exe query "<语义化问题，不含编号>"                        # 命中 dense
```
Expected：三类都能返回带条款号溯源的回答；精确编号查询稳定命中对应条款；含同义词的问题召回不塌陷。

- [ ] **Step 5: 状态过滤验证**

把某标准在 PG 置为作废：`UPDATE standards SET status='作废' WHERE ...;` 并同步 Milvus 标量（重灌或手动），再查询该标准条款。
Expected：默认（status=现行）不召回该作废标准条款。

- [ ] **Step 6: 跑全部单测确认无回归**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: 全部 PASS（M1/M2 既有测试 + M3 四个新测试文件）。

- [ ] **Step 7: Commit**

```
git add src/main.cpp
git commit -m "feat(m3): query subcommand uses 3-way fused text retrieval (M3 acceptance)"
```

> **M3 完成判据：** `rag2 query` 走"查询理解→dense+BM25+PG精确三路→RRF→密级二次校验→small-to-big→生成"；标准号/条款号精确查询稳定命中；含同义词/术语的查询召回不塌陷（BM25+jieba+自定义词典生效）；默认只召回现行；全部单测 PASS。

---

## 自检结果（Spec 覆盖核对）

对照 M3 在总览 spec §3.1 与技术文档 §9.0/§9.2/§9.6/§10.1/§10.4 的范围：

- **查询理解前置：标准号/条款号解析归一化（§9.0.1）** → Task 1 `analyze_query`（复用 M2 `extract_standard_no`/`is_valid_clause_no`）✅
- **领域同义词词典 + 查询扩展（§9.0.2）** → Task 2 `SynonymDict` + `config/synonyms.txt`，BM25 路 expand ✅
- **中文自定义分词词典（§9.0.3）** → `config/user_dict.txt` + Task 5 jieba analyzer 加载 + Task 8 入库装载 ✅
- **dense 召回（§9.2）** → Task 6 `DenseRetriever`（适配 filter）✅
- **BM25 召回（§9.2）** → Task 5 Milvus 全文检索 + Task 6 `Bm25Retriever` ✅
- **PG 精确匹配（§9.2）** → Task 6 `PgExactRetriever` + PgClient 精确查询方法 ✅
- **RRF 融合（§10.1）** → Task 4 `rrf_fuse`（去重合并来源）✅
- **状态+密级统一过滤下推 Milvus 标量 + PG（§9.6）** → Task 3 `to_milvus_expr` + Task 5 schema 标量 + Task 7 PG 二次校验 ✅
- **small-to-big 回填（§10.4）** → Task 7 `context_text_of` 并入 ContextFragment ✅
- **标准号不进向量召回（§9.0.1）** → Task 7 标准号转 standard_id 走标量过滤，PgExact 单独处理 ✅
- **契约④归一化键贯穿** → 三路候选均 `standard_id+clause_id`，RRF 按 clause_id 去重 ✅
- **契约⑤不破坏** → 生成端仍注入 ContextFragment ✅

**未纳入 M3（按设计归属后续，非缺口）：**
- 表格 cell 级限值定位（§9.7）→ M5；M3 只用 `wants_table` 标记意图，未做 cell 定位。
- reranker（§10.3）→ M5；M3 是 RRF baseline（§10.3 "MVP 无 reranker"）。
- 一跳引用扩展（§10.5）→ M5。
- 完整版本一致性/作废同步自动化（§13.4）→ M6；M3 仅落地"默认现行过滤"与密级二次校验。

无占位符遗留；跨任务类型一致（`RetrievalFilter`/`Candidate`/`Retriever::retrieve(query,filter,top_k)` 新签名在 dense/bm25/pgexact/text_search/answer_pipeline 全部一致；`rrf_fuse`/`to_milvus_expr`/`analyze_query`/`SynonymDict::expand` 命名前后一致；`search_dense`/`search_bm25`/`insert_full`/`ensure_collection_text` 在 Task 5 定义、Task 6/8 使用）。

## 开放项（执行时需你确认，非计划缺陷）

1. **Milvus 2.5 全文检索的 analyzer/Function 具体字段名**随小版本可能微调（如 `analyzer_params.tokenizer` 的 jieba 配置键）。Task 5 给的是 2.5.x 通行写法；联调时以你的 Milvus 版本 REST 文档校正，报错即按返回信息微调 schema body。
2. **同义词/自定义分词词典内容**需领域人员扩充（当前为示例几行）；词典纳入版本管理，更新后需重建受影响 BM25 索引并记录词典版本（§9.0.3 末段）。
3. **密级模型**：本计划用简单字符串相等（access_level == 请求级 或 admin 放行）。真实分级（多级密级、包含关系）需按你的权限模型细化 `text_retrieve` 的校验逻辑。
4. **per_path_k / fused_k / RRF k=60** 为经验默认，应在 M4 评估集上调参。
```
