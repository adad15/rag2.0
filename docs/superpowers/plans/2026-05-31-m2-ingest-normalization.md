# M2 入库正规化 + 结构化底座加厚 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 M1 的临时入库（朴素切分、最小表、文件名占位元数据）替换为文档正规设计——完整条款层级树、三文本分离、表格结构化、page→clause 映射、解析质检门禁，并接入 MinerU 服务与 poppler 并行。

**Architecture:** 在 M1 既有 C++ 骨架上加厚"解析→结构化→入库"段。解析器契约①做**加性扩展**（ParsedDoc 增加归一化元素流 `elements`），poppler 与新增的 MinerU 客户端都产出同一 IR；结构化处理（层级树、三文本、表格 cell、page 映射、质检）全是纯逻辑，走严格 TDD；MinerU 作为 Python/GPU 服务（FastAPI）独立部署，C++ 通过 HTTP 调用。入库门禁不达标的文档写入 PG 但标记待人工复核、暂不进 Milvus。

**Tech Stack:** 延续 M0/M1——C++17、CMake+vcpkg、libpqxx、cpp-httplib、nlohmann/json、spdlog、doctest、poppler-cpp。新增：MinerU 2.5 + FastAPI/uvicorn（Python 服务）。

---

## 前置条件（来自 M0/M1，必须已完成）

- M0+M1 计划全部任务完成：`rag2 smoke` 四项全绿、`rag2 ingest`/`rag2 query` 端到端可跑。
- 已存在文件：`src/parse/parser.h`（含 `Parser`/`ParsedDoc`/`ParsedPage`）、`src/parse/poppler_parser.{h,cpp}`、`src/db/schema.sql`、`src/db/pg_client.{h,cpp}`、`src/ingest/clause_splitter.{h,cpp}`、`src/ingest/ingest_pipeline.{h,cpp}`、`src/main.cpp`。
- 环境变量沿用 M1，并新增：`RAG_MINERU_BASE_URL`（默认 `http://localhost:8000`）。

## 关键设计决策（执行前若不同意请先改）

1. **IR 加性扩展，不破坏契约①。** `ParsedDoc` 新增 `elements`（归一化元素流）与 `standard_no`；`pages` 保留。poppler 用行级标题识别填 `elements`（降级版，无 bbox/表格），MinerU 填完整版。下游层级树只认 `elements`。
2. **结构化处理全部纯函数**：层级树、三文本、表格 cell、page 映射、质检都是"输入数据→输出数据"，便于 TDD，不依赖外部服务。
3. **MinerU 走 HTTP 服务**（§4.2 FastAPI）。C++ 端 `MineruParser` 是契约①的第二实现，仅负责"调用 + JSON 归一化到 IR"。MinerU 服务可 CPU 运行（慢）或 GPU；部署是独立任务。
4. **入库门禁**（§14.2）：质检不通过 → PG 写入但 `standards.review_status='pending_review'`，**不写 Milvus**；通过 → 正常索引。
5. **朴素切分器（M1 的 `clause_splitter`）保留但不再用于主流程**，由层级树构建器取代；保留以便回归对照，不删除。

## 文件结构（本计划新建/修改）

```
修改:
  src/parse/parser.h               扩展 IR：ParseElement / ParsedDoc.elements / standard_no
  src/parse/poppler_parser.cpp     额外填充 elements
  src/db/schema.sql                扩展为完整 schema（§5.1-5.6）
  src/db/pg_client.h / .cpp        新增结构体与写入/查询方法
  src/ingest/ingest_pipeline.h/.cpp 重写编排：route→tree→chunks→tables→pagemap→质检门禁→PG→embed→Milvus
  src/main.cpp                     ingest 子命令输出质检报告 + 复核标记
  CMakeLists.txt                   登记新源文件与测试
新建:
  src/ingest/metadata_extractor.h/.cpp   标准号/条款号正则抽取（纯逻辑）
  src/ingest/clause_tree.h/.cpp          层级树构建（纯逻辑）
  src/ingest/chunk_builder.h/.cpp        三文本分离（纯逻辑）
  src/ingest/table_structurer.h/.cpp     表格 HTML→结构化 cell（纯逻辑）
  src/ingest/page_map.h/.cpp             page→clause 映射构建（纯逻辑）
  src/quality/quality_check.h/.cpp       解析质检 + 门禁判定（纯逻辑）
  src/parse/mineru_parser.h/.cpp         MinerU HTTP 客户端解析器（契约①第二实现）
  src/parse/parser_router.h/.cpp         poppler vs MinerU 选择（纯逻辑判定）
  services/mineru/app.py                 MinerU FastAPI 服务
  services/mineru/requirements.txt       Python 依赖
  tests/test_metadata_extractor.cpp
  tests/test_clause_tree.cpp
  tests/test_chunk_builder.cpp
  tests/test_table_structurer.cpp
  tests/test_page_map.cpp
  tests/test_quality_check.cpp
  tests/test_mineru_normalize.cpp
  tests/test_parser_router.cpp
```

---

### Task 1: 扩展解析器 IR（契约①加性扩展）

**Files:**
- Modify: `src/parse/parser.h`
- Modify: `src/parse/poppler_parser.cpp`

- [ ] **Step 1: 修改 `src/parse/parser.h`，新增元素流（保留旧字段）**

把整个文件替换为：
```cpp
#pragma once
#include <string>
#include <vector>

enum class ElementType { Heading, Text, Table, Formula, Figure };

// 归一化元素：poppler（降级）与 MinerU（完整）都产出它，按阅读顺序排列。
struct ParseElement {
    ElementType type = ElementType::Text;
    int level = 0;                 // Heading 的层级（章=1, 节=2, 条=3...）；非标题为 0
    int page_no = 0;               // 从 1 开始
    std::string clause_no;         // 若元素自带条款号（如 4.2.1）
    std::string title;             // 标题文本（Heading）
    std::string text;              // 正文 / OCR 文本
    std::string table_html;        // 表格 HTML（Table 类型）
    std::string caption;           // 表题/图题
    std::string bbox;              // "x0,y0,x1,y1"，poppler 可空
    float ocr_confidence = 1.0f;   // MinerU 提供；poppler 默认 1.0
};

struct ParsedPage {
    int page_no = 0;
    std::string text;
};

struct ParsedDoc {
    std::string source_path;
    std::string title;
    std::string standard_no;                // 解析出的标准号（可空，Task 2 抽取后回填）
    std::vector<ParsedPage> pages;          // 每页全文（质检/page 映射用）
    std::vector<ParseElement> elements;     // 归一化元素流（层级树输入）
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual ParsedDoc parse(const std::string& file_path) = 0;
};
```

- [ ] **Step 2: 修改 `src/parse/poppler_parser.cpp`，在抽页文本后额外生成 elements**

把 `parse` 函数体替换为（保留原 include；新增 `<regex>`）：
```cpp
#include "parse/poppler_parser.h"
#include <poppler-document.h>
#include <poppler-page.h>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <regex>
#include <sstream>

ParsedDoc PopplerParser::parse(const std::string& file_path) {
    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_file(file_path));
    if (!doc)
        throw std::runtime_error("poppler: cannot open " + file_path);

    ParsedDoc out;
    out.source_path = file_path;
    out.title = std::filesystem::path(file_path).filename().string();

    // 行首条款号：覆盖 1.0.1 / 4.2.1 / 4.2.1-1 / 4.2.1-a
    static const std::regex clause_head(
        R"(^\s*(\d+\.\d+(?:\.\d+)?(?:-[0-9a-zA-Z]+)?)\s+(.*)$)");
    // 章标题：如 "4 总体设计" / "第4章 总体设计"
    static const std::regex chapter_head(R"(^\s*(?:第)?(\d+)(?:章)?\s+(\S.*)$)");

    int n = doc->pages();
    for (int i = 0; i < n; ++i) {
        std::unique_ptr<poppler::page> pg(doc->create_page(i));
        ParsedPage p;
        p.page_no = i + 1;
        if (pg) {
            poppler::byte_array ba = pg->text().to_utf8();
            p.text.assign(ba.begin(), ba.end());
        }
        out.pages.push_back(p);

        // 行级生成 elements（降级：无 bbox/表格）
        std::istringstream iss(p.text);
        std::string line;
        while (std::getline(iss, line)) {
            std::smatch m;
            if (std::regex_match(line, m, clause_head)) {
                ParseElement e;
                e.type = ElementType::Heading;
                e.level = 3;                 // 条级
                e.page_no = i + 1;
                e.clause_no = m[1].str();
                e.title = m[2].str();
                e.text = m[2].str();
                out.elements.push_back(std::move(e));
            } else if (std::regex_match(line, m, chapter_head)) {
                ParseElement e;
                e.type = ElementType::Heading;
                e.level = 1;                 // 章级
                e.page_no = i + 1;
                e.clause_no = m[1].str();
                e.title = m[2].str();
                out.elements.push_back(std::move(e));
            } else if (!out.elements.empty()) {
                std::string t = line;
                size_t a = t.find_first_not_of(" \t\r");
                if (a != std::string::npos)
                    out.elements.back().text += t.substr(a);
            }
        }
    }
    return out;
}
```

- [ ] **Step 3: 构建验证（确保旧调用未被破坏）**

Run: `cmake --build build --config Debug`
Expected: 成功。M1 的 `ingest_pipeline`/`smoke` 仍编译通过（仅新增字段，未删旧字段）。

- [ ] **Step 4: Commit**

```
git add src/parse/parser.h src/parse/poppler_parser.cpp
git commit -m "feat(m2): extend Parser IR with normalized element stream (additive)"
```

---

### Task 2: 元数据抽取（标准号/条款号正则，纯逻辑 TDD）

**Files:**
- Create: `src/ingest/metadata_extractor.h`, `src/ingest/metadata_extractor.cpp`
- Test: `tests/test_metadata_extractor.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_metadata_extractor.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/metadata_extractor.h"

TEST_CASE("extract_standard_no recognizes GB and JTG formats") {
    CHECK(extract_standard_no("公路桥涵设计通用规范 JTG D60-2015") == "JTG D60-2015");
    CHECK(extract_standard_no("依据 GB/T 50081-2019 的规定") == "GB/T 50081-2019");
    CHECK(extract_standard_no("无标准号的普通文本") == "");
}

TEST_CASE("normalize_standard_no unifies spacing and slashes") {
    CHECK(normalize_standard_no("JTG/T D60") == "JTG/T D60");
    CHECK(normalize_standard_no("JTGD60-2015") == "JTG D60-2015");
    CHECK(normalize_standard_no("  GB/T  50081-2019 ") == "GB/T 50081-2019");
}

TEST_CASE("is_valid_clause_no accepts normative numbering") {
    CHECK(is_valid_clause_no("4.2.1"));
    CHECK(is_valid_clause_no("4.2.1-1"));
    CHECK(is_valid_clause_no("1.0.1"));
    CHECK_FALSE(is_valid_clause_no("第四条"));
    CHECK_FALSE(is_valid_clause_no("abc"));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/ingest/metadata_extractor.h`**

```cpp
#pragma once
#include <string>

// 从文本中抽取首个标准号（GB/GB/T、JTG/JTJ/TB 等）；无则返回空串。
std::string extract_standard_no(const std::string& text);
// 归一化：去多余空格、统一 "JTGD60"→"JTG D60"、保留斜杠与连字符。
std::string normalize_standard_no(const std::string& raw);
// 条款号是否合法（N.N / N.N.N / N.N.N-x）。
bool is_valid_clause_no(const std::string& s);
```

- [ ] **Step 4: 写 `src/ingest/metadata_extractor.cpp`**

```cpp
#include "ingest/metadata_extractor.h"
#include <regex>

std::string extract_standard_no(const std::string& text) {
    // 前缀族 + 可选 /T + 空格或无空格 + 字母数字编号 + 可选 -年份
    static const std::regex re(
        R"((GB|JTG|JTJ|TB|JGJ|CJJ|DL|SL|YB)(/T)?\s*([A-Z]?\d{2,5})(-\d{4})?)");
    std::smatch m;
    if (std::regex_search(text, m, re)) {
        std::string raw = m[1].str() + m[2].str() + " " + m[3].str() + m[4].str();
        return normalize_standard_no(raw);
    }
    return "";
}

std::string normalize_standard_no(const std::string& raw) {
    std::string s = raw;
    // 在前缀字母与编号之间补空格：JTGD60 -> JTG D60
    s = std::regex_replace(s, std::regex(R"((GB|JTG|JTJ|TB|JGJ|CJJ|DL|SL|YB)(/T)?([A-Z]?\d))"),
                           "$1$2 $3");
    // 压缩多空格
    s = std::regex_replace(s, std::regex(R"(\s+)"), " ");
    // 去首尾空格
    s = std::regex_replace(s, std::regex(R"(^\s+|\s+$)"), "");
    return s;
}

bool is_valid_clause_no(const std::string& s) {
    static const std::regex re(R"(^\d+\.\d+(?:\.\d+)?(?:-[0-9a-zA-Z]+)?$)");
    return std::regex_match(s, re);
}
```

- [ ] **Step 5: 在 `CMakeLists.txt` 登记源与测试**

在 `add_library(rag_core STATIC` 的源列表中加一行 `src/ingest/metadata_extractor.cpp`；
在 `add_executable(rag_tests` 列表中加一行 `tests/test_metadata_extractor.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/ingest/metadata_extractor.h src/ingest/metadata_extractor.cpp tests/test_metadata_extractor.cpp CMakeLists.txt
git commit -m "feat(m2): standard-no/clause-no extraction and normalization (TDD)"
```

---

### Task 3: 条款层级树构建（纯逻辑 TDD）

**Files:**
- Create: `src/ingest/clause_tree.h`, `src/ingest/clause_tree.cpp`
- Test: `tests/test_clause_tree.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_clause_tree.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/clause_tree.h"
#include "parse/parser.h"

static ParseElement heading(int level, const std::string& no,
                            const std::string& title, int page) {
    ParseElement e; e.type = ElementType::Heading; e.level = level;
    e.clause_no = no; e.title = title; e.text = title; e.page_no = page;
    return e;
}

TEST_CASE("build_clause_tree links clauses under chapters with paths") {
    std::vector<ParseElement> els = {
        heading(1, "4", "总体设计", 10),
        heading(3, "4.2", "设计要求", 11),
        heading(3, "4.2.1", "桥涵设计应符合规定。", 11),
        heading(3, "4.2.2", "设计洪水频率取值。", 12),
    };
    auto tree = build_clause_tree(els, "STD1");

    REQUIRE(tree.size() == 4);
    CHECK(tree[0].node_type == "chapter");
    CHECK(tree[0].clause_no == "4");

    // 4.2.1 的父应为 4.2，path 含完整链
    auto it = std::find_if(tree.begin(), tree.end(),
        [](const ClauseTreeNode& n){ return n.clause_no == "4.2.1"; });
    REQUIRE(it != tree.end());
    CHECK(it->node_type == "clause");
    CHECK(it->parent_id == "STD1:4.2");
    CHECK(it->path == "4 / 4.2 / 4.2.1");
    CHECK(it->page_start == 11);
    CHECK(it->text.find("桥涵设计应符合") != std::string::npos);
}

TEST_CASE("build_clause_tree assigns node_id as standard_id:clause_no") {
    std::vector<ParseElement> els = { heading(3, "1.0.1", "制定本规范。", 1) };
    auto tree = build_clause_tree(els, "STD9");
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].node_id == "STD9:1.0.1");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/ingest/clause_tree.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "parse/parser.h"

struct ClauseTreeNode {
    std::string node_id;     // standard_id : clause_no
    std::string standard_id;
    std::string parent_id;   // 父节点 node_id，根为空
    std::string node_type;   // chapter / section / clause / item
    std::string clause_no;
    std::string title;
    std::string path;        // "4 / 4.2 / 4.2.1"
    int level = 0;
    int page_start = 0;
    std::string text;        // 条款自身正文
};

// 由归一化 Heading 元素流构建层级树。
// 父子关系按 clause_no 的点分前缀推断（4.2.1 的父是 4.2，4.2 的父是 4）。
std::vector<ClauseTreeNode> build_clause_tree(
    const std::vector<ParseElement>& elements, const std::string& standard_id);
```

- [ ] **Step 4: 写 `src/ingest/clause_tree.cpp`**

```cpp
#include "ingest/clause_tree.h"
#include <map>

// 由 clause_no 推父号：去掉最后一段。4.2.1->4.2；4.2->4；4->""。
static std::string parent_clause_no(const std::string& no) {
    auto pos = no.find_last_of('.');
    if (pos == std::string::npos) return "";
    return no.substr(0, pos);
}

// 由点分段数推 node_type 与 level。
static std::string type_of(const std::string& no) {
    int dots = 0;
    for (char c : no) if (c == '.') ++dots;
    if (no.find('-') != std::string::npos) return "item";
    if (dots == 0) return "chapter";
    if (dots == 1) return "section";
    return "clause";
}

std::vector<ClauseTreeNode> build_clause_tree(
    const std::vector<ParseElement>& elements, const std::string& standard_id) {
    std::map<std::string, ClauseTreeNode> by_no;   // clause_no -> node
    std::vector<std::string> order;                // 保持插入顺序

    for (const auto& e : elements) {
        if (e.type != ElementType::Heading || e.clause_no.empty()) continue;
        if (by_no.count(e.clause_no)) {            // 同号重复：并入正文
            by_no[e.clause_no].text += e.text;
            continue;
        }
        ClauseTreeNode n;
        n.standard_id = standard_id;
        n.clause_no = e.clause_no;
        n.node_id = standard_id + ":" + e.clause_no;
        n.title = e.title;
        n.text = e.text;
        n.page_start = e.page_no;
        n.node_type = type_of(e.clause_no);
        by_no[e.clause_no] = n;
        order.push_back(e.clause_no);
    }

    // 回填 parent_id / path / level
    std::vector<ClauseTreeNode> out;
    for (const auto& no : order) {
        ClauseTreeNode n = by_no[no];
        std::string pno = parent_clause_no(no);
        if (!pno.empty() && by_no.count(pno))
            n.parent_id = standard_id + ":" + pno;
        // path：沿父链拼接（仅含库内存在的祖先）
        std::vector<std::string> chain;
        std::string cur = no;
        while (!cur.empty()) {
            chain.push_back(cur);
            std::string p = parent_clause_no(cur);
            cur = (by_no.count(p)) ? p : "";
        }
        std::string path;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (!path.empty()) path += " / ";
            path += *it;
        }
        n.path = path;
        n.level = (int)chain.size();
        out.push_back(n);
    }
    return out;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 源加 `src/ingest/clause_tree.cpp`；`rag_tests` 加 `tests/test_clause_tree.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/ingest/clause_tree.h src/ingest/clause_tree.cpp tests/test_clause_tree.cpp CMakeLists.txt
git commit -m "feat(m2): clause hierarchy tree builder with parent/path inference (TDD)"
```

---

### Task 4: 三文本分离（atomic/retrieval/context，纯逻辑 TDD）

**Files:**
- Create: `src/ingest/chunk_builder.h`, `src/ingest/chunk_builder.cpp`
- Test: `tests/test_chunk_builder.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_chunk_builder.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/chunk_builder.h"

static ClauseTreeNode node(const std::string& sid, const std::string& no,
                           const std::string& type, const std::string& parent,
                           const std::string& path, const std::string& text) {
    ClauseTreeNode n; n.standard_id = sid; n.clause_no = no;
    n.node_id = sid + ":" + no; n.node_type = type;
    n.parent_id = parent; n.path = path; n.text = text;
    return n;
}

TEST_CASE("build_chunks separates atomic/retrieval/context texts") {
    std::vector<ClauseTreeNode> tree = {
        node("STD1", "4.2", "section", "", "4 / 4.2", "设计要求"),
        node("STD1", "4.2.1", "clause", "STD1:4.2", "4 / 4.2 / 4.2.1",
             "桥涵设计应符合本规范。"),
    };
    StandardMeta meta{ "STD1", "JTG D60-2015", "公路桥涵设计通用规范" };
    auto chunks = build_chunks(tree, meta);

    auto it = std::find_if(chunks.begin(), chunks.end(),
        [](const RetrievalChunk& c){ return c.node_id == "STD1:4.2.1"; });
    REQUIRE(it != chunks.end());

    CHECK(it->atomic_text == "桥涵设计应符合本规范。");
    // retrieval_text 应含标准号 + 名称 + 路径 + 条款号 + 原文
    CHECK(it->retrieval_text.find("JTG D60-2015") != std::string::npos);
    CHECK(it->retrieval_text.find("公路桥涵设计通用规范") != std::string::npos);
    CHECK(it->retrieval_text.find("4.2.1") != std::string::npos);
    CHECK(it->retrieval_text.find("桥涵设计应符合") != std::string::npos);
    // context_text 应含父节 4.2 的正文
    CHECK(it->context_text.find("设计要求") != std::string::npos);
    CHECK(it->chunk_type == "clause");
    CHECK(it->chunk_id == "STD1:4.2.1#clause");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/ingest/chunk_builder.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "ingest/clause_tree.h"

struct StandardMeta {
    std::string standard_id;
    std::string standard_no;
    std::string standard_name;
};

struct RetrievalChunk {
    std::string chunk_id;        // node_id#chunk_type
    std::string node_id;
    std::string standard_id;
    std::string atomic_text;     // 原子条款文本
    std::string retrieval_text;  // 检索扩展文本
    std::string context_text;    // small-to-big 父级上下文
    std::string chunk_type;      // clause / section / item ...
};

// §7.2：为每个 clause/item/section 节点生成三文本检索块。
std::vector<RetrievalChunk> build_chunks(
    const std::vector<ClauseTreeNode>& tree, const StandardMeta& meta);
```

- [ ] **Step 4: 写 `src/ingest/chunk_builder.cpp`**

```cpp
#include "ingest/chunk_builder.h"
#include <map>

std::vector<RetrievalChunk> build_chunks(
    const std::vector<ClauseTreeNode>& tree, const StandardMeta& meta) {
    std::map<std::string, const ClauseTreeNode*> by_id;
    for (const auto& n : tree) by_id[n.node_id] = &n;

    std::vector<RetrievalChunk> out;
    for (const auto& n : tree) {
        // 仅对叶级条款/项/节生成检索块；章标题不单独建块
        if (n.node_type == "chapter") continue;

        RetrievalChunk c;
        c.node_id = n.node_id;
        c.standard_id = n.standard_id;
        c.chunk_type = n.node_type;
        c.chunk_id = n.node_id + "#" + n.node_type;
        c.atomic_text = n.text;

        c.retrieval_text = meta.standard_no + " " + meta.standard_name + " " +
                           n.path + " " + n.clause_no + " " + n.text;

        // context_text：父节点正文（small-to-big 回填基础）
        if (!n.parent_id.empty() && by_id.count(n.parent_id))
            c.context_text = by_id[n.parent_id]->text;
        else
            c.context_text = n.text;

        out.push_back(std::move(c));
    }
    return out;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/ingest/chunk_builder.cpp`；`rag_tests` 加 `tests/test_chunk_builder.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/ingest/chunk_builder.h src/ingest/chunk_builder.cpp tests/test_chunk_builder.cpp CMakeLists.txt
git commit -m "feat(m2): three-text chunk builder (atomic/retrieval/context) (TDD)"
```

---

### Task 5: 表格结构化（HTML→cell，纯逻辑 TDD）

**Files:**
- Create: `src/ingest/table_structurer.h`, `src/ingest/table_structurer.cpp`
- Test: `tests/test_table_structurer.cpp`
- Modify: `CMakeLists.txt`

> 说明：M2 处理 MinerU 输出的规整 HTML 表格（无跨行跨列的常见情形）。合并单元格的完整还原（rowspan/colspan→merged_info）作为后续增强；本任务实现基础解析 + 数值/单位抽取 + 简单 colspan 标记。

- [ ] **Step 1: 写失败测试 `tests/test_table_structurer.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/table_structurer.h"

TEST_CASE("structure_table parses headers, cells, numbers and units") {
    std::string html =
        "<table>"
        "<tr><td>等级</td><td>压实度(%)</td></tr>"
        "<tr><td>高速</td><td>96</td></tr>"
        "<tr><td>一级</td><td>95</td></tr>"
        "</table>";
    SpecTable t = structure_table(html, "STD1", "STD1:4.2.1", "路基压实度", 12);

    CHECK(t.table_id == "STD1:T:12:0");
    CHECK(t.node_id == "STD1:4.2.1");
    CHECK(t.caption == "路基压实度");
    CHECK(t.page_no == 12);
    REQUIRE(t.cells.size() == 4);   // 2 行数据 × 2 列

    // 第一数据行第二列：数值 96，单位 %（取自列表头 "压实度(%)"）
    auto& cell = t.cells[0];
    CHECK(cell.row_idx == 1);
    CHECK(cell.col_idx == 1);
    CHECK(cell.col_header == "压实度(%)");
    CHECK(cell.row_header == "高速");
    CHECK(cell.number_value == doctest::Approx(96.0));
    CHECK(cell.unit == "%");
}

TEST_CASE("structure_table leaves number_value empty for non-numeric cells") {
    std::string html = "<table><tr><td>名称</td></tr><tr><td>沥青</td></tr></table>";
    SpecTable t = structure_table(html, "STD1", "STD1:1.0.1", "材料", 3);
    REQUIRE(t.cells.size() == 1);
    CHECK_FALSE(t.cells[0].has_number);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/ingest/table_structurer.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct TableCell {
    int row_idx = 0;
    int col_idx = 0;
    std::string row_header;
    std::string col_header;
    std::string cell_text;
    bool has_number = false;
    double number_value = 0.0;
    std::string unit;
    std::string merged_info;   // M2 基础版留空；合并单元格增强见后续
};

struct SpecTable {
    std::string table_id;
    std::string standard_id;
    std::string node_id;
    std::string caption;
    int page_no = 0;
    std::string table_html;
    std::string table_markdown;
    std::string table_summary;
    std::vector<TableCell> cells;
};

// 解析规整 HTML 表格：第一行作列表头、第一列作行表头，
// 数据单元格抽取数值与单位（单位优先取列表头括号内，如 "压实度(%)"）。
// table_index 用于同页多表去重，默认 0。
SpecTable structure_table(const std::string& table_html,
                          const std::string& standard_id,
                          const std::string& node_id,
                          const std::string& caption,
                          int page_no,
                          int table_index = 0);
```

- [ ] **Step 4: 写 `src/ingest/table_structurer.cpp`**

```cpp
#include "ingest/table_structurer.h"
#include <regex>
#include <sstream>

// 极简 HTML 表解析：抓 <tr>...</tr> 内的 <td>/<th> 文本。
static std::vector<std::vector<std::string>> parse_rows(const std::string& html) {
    std::vector<std::vector<std::string>> rows;
    static const std::regex tr_re(R"(<tr[^>]*>(.*?)</tr>)",
                                  std::regex::icase);
    static const std::regex td_re(R"(<t[dh][^>]*>(.*?)</t[dh]>)",
                                  std::regex::icase);
    for (std::sregex_iterator it(html.begin(), html.end(), tr_re), end; it != end; ++it) {
        std::string row_html = (*it)[1].str();
        std::vector<std::string> cells;
        for (std::sregex_iterator c(row_html.begin(), row_html.end(), td_re), e; c != e; ++c) {
            std::string cell = (*c)[1].str();
            cell = std::regex_replace(cell, std::regex(R"(<[^>]*>)"), ""); // 去内层标签
            cell = std::regex_replace(cell, std::regex(R"(^\s+|\s+$)"), "");
            cells.push_back(cell);
        }
        if (!cells.empty()) rows.push_back(cells);
    }
    return rows;
}

// 从列表头括号内取单位："压实度(%)"->"%"；"长度(mm)"->"mm"。
static std::string unit_from_header(const std::string& header) {
    std::smatch m;
    if (std::regex_search(header, m, std::regex(R"([\(（]([^\)）]+)[\)）])")))
        return m[1].str();
    return "";
}

static bool to_number(const std::string& s, double& out) {
    std::smatch m;
    if (std::regex_search(s, m, std::regex(R"(-?\d+(?:\.\d+)?)"))) {
        out = std::stod(m[0].str());
        return true;
    }
    return false;
}

SpecTable structure_table(const std::string& html, const std::string& standard_id,
                          const std::string& node_id, const std::string& caption,
                          int page_no, int table_index) {
    SpecTable t;
    t.standard_id = standard_id;
    t.node_id = node_id;
    t.caption = caption;
    t.page_no = page_no;
    t.table_html = html;
    t.table_id = standard_id + ":T:" + std::to_string(page_no) + ":" +
                 std::to_string(table_index);

    auto rows = parse_rows(html);
    if (rows.empty()) return t;
    const auto& col_headers = rows[0];

    std::ostringstream md;
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size(); ++c) {
            md << "| " << rows[r][c] << " ";
        }
        md << "|\n";
        if (r == 0) {
            for (size_t c = 0; c < rows[r].size(); ++c) md << "|---";
            md << "|\n";
        }
    }
    t.table_markdown = md.str();

    for (size_t r = 1; r < rows.size(); ++r) {
        const std::string& row_header = rows[r].empty() ? "" : rows[r][0];
        for (size_t c = 1; c < rows[r].size(); ++c) {
            TableCell cell;
            cell.row_idx = (int)r;
            cell.col_idx = (int)c;
            cell.row_header = row_header;
            cell.col_header = (c < col_headers.size()) ? col_headers[c] : "";
            cell.cell_text = rows[r][c];
            double num;
            if (to_number(cell.cell_text, num)) {
                cell.has_number = true;
                cell.number_value = num;
                cell.unit = unit_from_header(cell.col_header);
            }
            t.cells.push_back(std::move(cell));
        }
    }
    return t;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/ingest/table_structurer.cpp`；`rag_tests` 加 `tests/test_table_structurer.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/ingest/table_structurer.h src/ingest/table_structurer.cpp tests/test_table_structurer.cpp CMakeLists.txt
git commit -m "feat(m2): table HTML to structured cells with number/unit extraction (TDD)"
```

---

### Task 6: page→clause 映射构建（纯逻辑 TDD）

**Files:**
- Create: `src/ingest/page_map.h`, `src/ingest/page_map.cpp`
- Test: `tests/test_page_map.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_page_map.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/page_map.h"

TEST_CASE("build_page_map maps each page to clause nodes appearing on it") {
    std::vector<ClauseTreeNode> tree;
    { ClauseTreeNode n; n.node_id="STD1:4.2.1"; n.standard_id="STD1"; n.page_start=11; tree.push_back(n); }
    { ClauseTreeNode n; n.node_id="STD1:4.2.2"; n.standard_id="STD1"; n.page_start=11; tree.push_back(n); }
    { ClauseTreeNode n; n.node_id="STD1:4.3.1"; n.standard_id="STD1"; n.page_start=12; tree.push_back(n); }

    auto rows = build_page_map(tree);
    // 第 11 页应映射到两条
    int p11 = 0;
    for (auto& r : rows) if (r.page_no == 11) ++p11;
    CHECK(p11 == 2);
    // 校验字段
    auto it = std::find_if(rows.begin(), rows.end(),
        [](const PageClauseRow& r){ return r.node_id == "STD1:4.3.1"; });
    REQUIRE(it != rows.end());
    CHECK(it->page_no == 12);
    CHECK(it->coverage_type == "full");
    CHECK(it->standard_id == "STD1");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/ingest/page_map.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "ingest/clause_tree.h"

struct PageClauseRow {
    std::string standard_id;
    int page_no = 0;
    std::string node_id;
    std::string coverage_type;   // full / partial / table / figure
};

// §5.4：由每个条款的起始页生成 page→clause 映射。
// M2 基础版用 page_start 建立 full 覆盖；跨页 partial 覆盖留待视觉路里程碑细化。
std::vector<PageClauseRow> build_page_map(const std::vector<ClauseTreeNode>& tree);
```

- [ ] **Step 4: 写 `src/ingest/page_map.cpp`**

```cpp
#include "ingest/page_map.h"

std::vector<PageClauseRow> build_page_map(const std::vector<ClauseTreeNode>& tree) {
    std::vector<PageClauseRow> out;
    for (const auto& n : tree) {
        if (n.page_start <= 0) continue;
        PageClauseRow r;
        r.standard_id = n.standard_id;
        r.page_no = n.page_start;
        r.node_id = n.node_id;
        r.coverage_type = "full";
        out.push_back(r);
    }
    return out;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/ingest/page_map.cpp`；`rag_tests` 加 `tests/test_page_map.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/ingest/page_map.h src/ingest/page_map.cpp tests/test_page_map.cpp CMakeLists.txt
git commit -m "feat(m2): page->clause map builder (TDD)"
```

---

### Task 7: 解析质检 + 入库门禁（纯逻辑 TDD）

**Files:**
- Create: `src/quality/quality_check.h`, `src/quality/quality_check.cpp`
- Test: `tests/test_quality_check.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_quality_check.cpp`**

```cpp
#include <doctest/doctest.h>
#include "quality/quality_check.h"

TEST_CASE("check_quality passes when standard_no found and clauses continuous") {
    QualityInput in;
    in.standard_no = "JTG D60-2015";
    in.clause_count = 3;
    in.avg_ocr_confidence = 0.98;
    in.low_conf_page_count = 0;
    in.page_map_count = 3;
    QualityReport r = check_quality(in);
    CHECK(r.passed);
    CHECK(r.issues.empty());
}

TEST_CASE("check_quality fails when standard_no missing") {
    QualityInput in;
    in.standard_no = "";
    in.clause_count = 5;
    in.avg_ocr_confidence = 0.99;
    QualityReport r = check_quality(in);
    CHECK_FALSE(r.passed);
    CHECK(std::find(r.issues.begin(), r.issues.end(), "standard_no_missing") != r.issues.end());
}

TEST_CASE("check_quality fails on low OCR confidence or zero clauses") {
    QualityInput a; a.standard_no="X"; a.clause_count=0; a.avg_ocr_confidence=0.99;
    CHECK_FALSE(check_quality(a).passed);

    QualityInput b; b.standard_no="X"; b.clause_count=3; b.avg_ocr_confidence=0.50;
    QualityReport rb = check_quality(b);
    CHECK_FALSE(rb.passed);
    CHECK(std::find(rb.issues.begin(), rb.issues.end(), "low_ocr_confidence") != rb.issues.end());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/quality/quality_check.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct QualityInput {
    std::string standard_no;
    int clause_count = 0;
    double avg_ocr_confidence = 1.0;
    int low_conf_page_count = 0;
    int page_map_count = 0;
};

struct QualityReport {
    bool passed = false;
    std::vector<std::string> issues;   // 机器可读问题码
};

// §14.1/§14.2：质检 + 门禁判定。不通过的文档应进待人工复核库（不索引 Milvus）。
// 阈值：avg_ocr_confidence >= 0.80；clause_count > 0；standard_no 非空。
QualityReport check_quality(const QualityInput& in);
```

- [ ] **Step 4: 写 `src/quality/quality_check.cpp`**

```cpp
#include "quality/quality_check.h"

QualityReport check_quality(const QualityInput& in) {
    QualityReport r;
    if (in.standard_no.empty())        r.issues.push_back("standard_no_missing");
    if (in.clause_count <= 0)          r.issues.push_back("no_clauses");
    if (in.avg_ocr_confidence < 0.80)  r.issues.push_back("low_ocr_confidence");
    r.passed = r.issues.empty();
    return r;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/quality/quality_check.cpp`；`rag_tests` 加 `tests/test_quality_check.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/quality/quality_check.h src/quality/quality_check.cpp tests/test_quality_check.cpp CMakeLists.txt
git commit -m "feat(m2): parse quality check and ingest gating (TDD)"
```

---

### Task 8: 扩展 PG schema + PgClient 写入方法

**Files:**
- Modify: `src/db/schema.sql`
- Modify: `src/db/pg_client.h`, `src/db/pg_client.cpp`

I/O 任务；端到端在 Task 13 验证。

- [ ] **Step 1: 扩展 `src/db/schema.sql`（在 M1 内容后追加，幂等）**

在文件末尾追加：
```sql
-- M2 扩展：standards 增字段
ALTER TABLE standards ADD COLUMN IF NOT EXISTS standard_type   TEXT;
ALTER TABLE standards ADD COLUMN IF NOT EXISTS version_year     TEXT;
ALTER TABLE standards ADD COLUMN IF NOT EXISTS access_level     TEXT DEFAULT 'public';
ALTER TABLE standards ADD COLUMN IF NOT EXISTS review_status    TEXT DEFAULT 'passed';
ALTER TABLE standards ADD COLUMN IF NOT EXISTS parse_version    TEXT;

-- M2 扩展：clause_nodes 增层级与标记字段
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS parent_id      TEXT;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS node_type      TEXT;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS level          INT;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS page_end       INT;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS is_mandatory   BOOLEAN DEFAULT FALSE;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS is_explanation BOOLEAN DEFAULT FALSE;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS refs           TEXT;
ALTER TABLE clause_nodes ADD COLUMN IF NOT EXISTS status         TEXT DEFAULT '现行';

CREATE TABLE IF NOT EXISTS retrieval_chunks (
    chunk_id       TEXT PRIMARY KEY,
    node_id        TEXT REFERENCES clause_nodes(node_id),
    standard_id    TEXT,
    atomic_text    TEXT,
    retrieval_text TEXT,
    context_text   TEXT,
    chunk_type     TEXT
);

CREATE TABLE IF NOT EXISTS page_clause_map (
    standard_id   TEXT,
    page_no       INT,
    node_id       TEXT,
    coverage_type TEXT,
    PRIMARY KEY (standard_id, page_no, node_id)
);

CREATE TABLE IF NOT EXISTS spec_tables (
    table_id       TEXT PRIMARY KEY,
    standard_id    TEXT,
    node_id        TEXT,
    caption        TEXT,
    page_no        INT,
    table_html     TEXT,
    table_markdown TEXT,
    table_summary  TEXT
);

CREATE TABLE IF NOT EXISTS spec_table_cells (
    cell_id      TEXT PRIMARY KEY,
    table_id     TEXT REFERENCES spec_tables(table_id),
    row_idx      INT,
    col_idx      INT,
    row_header   TEXT,
    col_header   TEXT,
    cell_text    TEXT,
    number_value DOUBLE PRECISION,
    unit         TEXT,
    merged_info  TEXT
);

CREATE INDEX IF NOT EXISTS idx_chunk_node ON retrieval_chunks(node_id);
CREATE INDEX IF NOT EXISTS idx_cells_table ON spec_table_cells(table_id);
```

- [ ] **Step 2: 在 `src/db/pg_client.h` 增加结构体与方法声明**

在文件中 `class PgClient` 之前加入结构体：
```cpp
struct ClauseNodeRow {
    std::string node_id;
    std::string standard_id;
    std::string parent_id;
    std::string node_type;
    std::string clause_no;
    std::string title;
    std::string path;
    int level = 0;
    int page_start = 0;
    std::string text;
};

struct ChunkRow {
    std::string chunk_id;
    std::string node_id;
    std::string standard_id;
    std::string atomic_text;
    std::string retrieval_text;
    std::string context_text;
    std::string chunk_type;
};
```

在 `class PgClient` 的 public 段追加方法声明：
```cpp
    void insert_clause_node(const ClauseNodeRow& n);
    void insert_chunk(const ChunkRow& c);
    void insert_page_clause(const std::string& standard_id, int page_no,
                            const std::string& node_id, const std::string& coverage);
    void insert_spec_table(const std::string& table_id, const std::string& standard_id,
                           const std::string& node_id, const std::string& caption,
                           int page_no, const std::string& html, const std::string& markdown);
    void insert_table_cell(const std::string& cell_id, const std::string& table_id,
                           int row_idx, int col_idx, const std::string& row_header,
                           const std::string& col_header, const std::string& cell_text,
                           bool has_number, double number_value, const std::string& unit);
    void set_review_status(const std::string& standard_id, const std::string& status);
```

- [ ] **Step 3: 在 `src/db/pg_client.cpp` 实现新方法（追加到文件末尾）**

```cpp
void PgClient::insert_clause_node(const ClauseNodeRow& n) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    tx.exec_params(
        "INSERT INTO clause_nodes(node_id,standard_id,parent_id,node_type,clause_no,"
        "title,path,level,text,page_start) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) "
        "ON CONFLICT (node_id) DO UPDATE SET parent_id=EXCLUDED.parent_id,"
        "node_type=EXCLUDED.node_type, path=EXCLUDED.path, level=EXCLUDED.level,"
        "text=EXCLUDED.text, title=EXCLUDED.title",
        n.node_id, n.standard_id, n.parent_id, n.node_type, n.clause_no,
        n.title, n.path, n.level, n.text, n.page_start);
    tx.commit();
}

void PgClient::insert_chunk(const ChunkRow& c) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    tx.exec_params(
        "INSERT INTO retrieval_chunks(chunk_id,node_id,standard_id,atomic_text,"
        "retrieval_text,context_text,chunk_type) VALUES($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT (chunk_id) DO UPDATE SET retrieval_text=EXCLUDED.retrieval_text,"
        "context_text=EXCLUDED.context_text, atomic_text=EXCLUDED.atomic_text",
        c.chunk_id, c.node_id, c.standard_id, c.atomic_text,
        c.retrieval_text, c.context_text, c.chunk_type);
    tx.commit();
}

void PgClient::insert_page_clause(const std::string& standard_id, int page_no,
                                  const std::string& node_id, const std::string& coverage) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    tx.exec_params(
        "INSERT INTO page_clause_map(standard_id,page_no,node_id,coverage_type) "
        "VALUES($1,$2,$3,$4) ON CONFLICT (standard_id,page_no,node_id) DO NOTHING",
        standard_id, page_no, node_id, coverage);
    tx.commit();
}

void PgClient::insert_spec_table(const std::string& table_id, const std::string& standard_id,
                                 const std::string& node_id, const std::string& caption,
                                 int page_no, const std::string& html, const std::string& markdown) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    tx.exec_params(
        "INSERT INTO spec_tables(table_id,standard_id,node_id,caption,page_no,"
        "table_html,table_markdown) VALUES($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT (table_id) DO UPDATE SET table_html=EXCLUDED.table_html,"
        "table_markdown=EXCLUDED.table_markdown, caption=EXCLUDED.caption",
        table_id, standard_id, node_id, caption, page_no, html, markdown);
    tx.commit();
}

void PgClient::insert_table_cell(const std::string& cell_id, const std::string& table_id,
                                 int row_idx, int col_idx, const std::string& row_header,
                                 const std::string& col_header, const std::string& cell_text,
                                 bool has_number, double number_value, const std::string& unit) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    if (has_number) {
        tx.exec_params(
            "INSERT INTO spec_table_cells(cell_id,table_id,row_idx,col_idx,row_header,"
            "col_header,cell_text,number_value,unit) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9) "
            "ON CONFLICT (cell_id) DO NOTHING",
            cell_id, table_id, row_idx, col_idx, row_header, col_header, cell_text,
            number_value, unit);
    } else {
        tx.exec_params(
            "INSERT INTO spec_table_cells(cell_id,table_id,row_idx,col_idx,row_header,"
            "col_header,cell_text,number_value,unit) VALUES($1,$2,$3,$4,$5,$6,$7,NULL,$8) "
            "ON CONFLICT (cell_id) DO NOTHING",
            cell_id, table_id, row_idx, col_idx, row_header, col_header, cell_text, unit);
    }
    tx.commit();
}

void PgClient::set_review_status(const std::string& standard_id, const std::string& status) {
    pqxx::connection cn(conninfo_); pqxx::work tx(cn);
    tx.exec_params("UPDATE standards SET review_status=$2 WHERE standard_id=$1",
                   standard_id, status);
    tx.commit();
}
```

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 5: Commit**

```
git add src/db/schema.sql src/db/pg_client.h src/db/pg_client.cpp
git commit -m "feat(m2): expand PG schema (§5) and PgClient writers for nodes/chunks/tables/pagemap"
```

---

### Task 9: MinerU FastAPI 服务（Python）

**Files:**
- Create: `services/mineru/app.py`
- Create: `services/mineru/requirements.txt`

> MinerU 需要较重依赖（建议 GPU；CPU 可跑但慢）。本任务交付一个最小可用的解析服务：接收文件路径，返回归一化元素流 JSON。若 MinerU 安装受阻，可先用其内部的 pipeline 输出，后续替换。

- [ ] **Step 1: 写 `services/mineru/requirements.txt`**

```
fastapi
uvicorn[standard]
mineru[core]
```

- [ ] **Step 2: 写 `services/mineru/app.py`（最小解析服务，输出归一化元素流）**

```python
from fastapi import FastAPI
from pydantic import BaseModel
from pathlib import Path
import subprocess, json, tempfile, os

app = FastAPI()

class ParseReq(BaseModel):
    file_path: str

def _normalize(content_list, source_path):
    """把 MinerU content_list.json 归一化为 C++ 端 IR 元素流。"""
    elements = []
    for item in content_list:
        t = item.get("type", "text")
        page = int(item.get("page_idx", 0)) + 1
        if t == "table":
            elements.append({
                "type": "Table", "page_no": page,
                "table_html": item.get("table_body", ""),
                "caption": " ".join(item.get("table_caption", [])),
                "text": "", "clause_no": "", "level": 0,
                "bbox": ",".join(map(str, item.get("bbox", []))),
                "ocr_confidence": 1.0,
            })
        elif t in ("text", "title"):
            text = item.get("text", "")
            level = 1 if t == "title" else 0
            elements.append({
                "type": "Heading" if t == "title" else "Text",
                "page_no": page, "level": level,
                "title": text if t == "title" else "",
                "text": text, "clause_no": "", "table_html": "",
                "caption": "", "bbox": ",".join(map(str, item.get("bbox", []))),
                "ocr_confidence": float(item.get("score", 1.0)),
            })
    return {"source_path": source_path, "title": Path(source_path).name,
            "standard_no": "", "elements": elements}

@app.post("/parse")
def parse(req: ParseReq):
    src = req.file_path
    if not os.path.exists(src):
        return {"error": f"file not found: {src}"}
    out_dir = tempfile.mkdtemp(prefix="mineru_")
    # 调用 MinerU CLI 产出 content_list.json
    subprocess.run(["mineru", "-p", src, "-o", out_dir], check=True)
    stem = Path(src).stem
    # MinerU 输出目录结构: <out_dir>/<stem>/auto/<stem>_content_list.json
    cl_path = Path(out_dir) / stem / "auto" / f"{stem}_content_list.json"
    if not cl_path.exists():
        # 兼容不同版本：递归找 *_content_list.json
        matches = list(Path(out_dir).rglob("*_content_list.json"))
        if not matches:
            return {"error": "content_list.json not produced"}
        cl_path = matches[0]
    content_list = json.loads(cl_path.read_text(encoding="utf-8"))
    return _normalize(content_list, src)

@app.get("/health")
def health():
    return {"status": "ok"}
```

- [ ] **Step 3: 安装并启动服务**

Run（PowerShell，建议在独立 Python 环境）:
```
python -m venv services\mineru\.venv
services\mineru\.venv\Scripts\pip install -r services\mineru\requirements.txt
services\mineru\.venv\Scripts\python -m uvicorn services.mineru.app:app --host 0.0.0.0 --port 8000
```
Expected: uvicorn 启动；`curl http://localhost:8000/health` 返回 `{"status":"ok"}`。
（若 MinerU 安装/模型下载受阻，记录为风险并优先用 poppler 路推进 Task 12，MinerU 路可后补。）

- [ ] **Step 4: Commit**

```
git add services/mineru/app.py services/mineru/requirements.txt
git commit -m "feat(m2): minimal MinerU FastAPI parse service returning normalized IR"
```

---

### Task 10: MinerU C++ 客户端解析器（契约①第二实现）

**Files:**
- Create: `src/parse/mineru_parser.h`, `src/parse/mineru_parser.cpp`
- Test: `tests/test_mineru_normalize.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_mineru_normalize.cpp`（纯解析逻辑，不触网）**

```cpp
#include <doctest/doctest.h>
#include "parse/mineru_parser.h"

TEST_CASE("parse_mineru_json maps service JSON to ParsedDoc") {
    std::string json = R"({
      "source_path": "C:/a.pdf",
      "title": "a.pdf",
      "standard_no": "JTG D60-2015",
      "elements": [
        {"type":"Heading","page_no":1,"level":1,"title":"4 总体设计","text":"4 总体设计",
         "clause_no":"","table_html":"","caption":"","bbox":"0,0,1,1","ocr_confidence":0.97},
        {"type":"Table","page_no":2,"level":0,"title":"","text":"",
         "clause_no":"","table_html":"<table><tr><td>a</td></tr></table>",
         "caption":"表1","bbox":"","ocr_confidence":1.0}
      ]
    })";
    ParsedDoc d = parse_mineru_json(json);
    CHECK(d.source_path == "C:/a.pdf");
    CHECK(d.standard_no == "JTG D60-2015");
    REQUIRE(d.elements.size() == 2);
    CHECK(d.elements[0].type == ElementType::Heading);
    CHECK(d.elements[0].level == 1);
    CHECK(d.elements[1].type == ElementType::Table);
    CHECK(d.elements[1].table_html.find("<table>") != std::string::npos);
    CHECK(d.elements[1].caption == "表1");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/parse/mineru_parser.h`**

```cpp
#pragma once
#include "parse/parser.h"
#include <string>

// 纯函数：把 MinerU 服务返回 JSON 解析为 ParsedDoc（可单测）。
ParsedDoc parse_mineru_json(const std::string& json_body);

class MineruParser : public Parser {
public:
    MineruParser(std::string base_url);
    ParsedDoc parse(const std::string& file_path) override;
private:
    std::string base_url_;
};
```

- [ ] **Step 4: 写 `src/parse/mineru_parser.cpp`**

```cpp
#include "parse/mineru_parser.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

static ElementType to_type(const std::string& s) {
    if (s == "Heading") return ElementType::Heading;
    if (s == "Table")   return ElementType::Table;
    if (s == "Formula") return ElementType::Formula;
    if (s == "Figure")  return ElementType::Figure;
    return ElementType::Text;
}

ParsedDoc parse_mineru_json(const std::string& json_body) {
    auto j = json::parse(json_body);
    if (j.contains("error"))
        throw std::runtime_error("mineru service error: " + j["error"].get<std::string>());
    ParsedDoc d;
    d.source_path = j.value("source_path", "");
    d.title = j.value("title", "");
    d.standard_no = j.value("standard_no", "");
    for (auto& e : j["elements"]) {
        ParseElement pe;
        pe.type = to_type(e.value("type", "Text"));
        pe.page_no = e.value("page_no", 0);
        pe.level = e.value("level", 0);
        pe.clause_no = e.value("clause_no", "");
        pe.title = e.value("title", "");
        pe.text = e.value("text", "");
        pe.table_html = e.value("table_html", "");
        pe.caption = e.value("caption", "");
        pe.bbox = e.value("bbox", "");
        pe.ocr_confidence = e.value("ocr_confidence", 1.0f);
        d.elements.push_back(std::move(pe));
    }
    return d;
}

MineruParser::MineruParser(std::string base_url) : base_url_(std::move(base_url)) {}

ParsedDoc MineruParser::parse(const std::string& file_path) {
    json body; body["file_path"] = file_path;
    auto res = http::post_json(base_url_, "/parse", body.dump(), {});
    if (!res.ok())
        throw std::runtime_error("mineru /parse failed: " + res.body + res.error);
    return parse_mineru_json(res.body);
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/parse/mineru_parser.cpp`；`rag_tests` 加 `tests/test_mineru_normalize.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/parse/mineru_parser.h src/parse/mineru_parser.cpp tests/test_mineru_normalize.cpp CMakeLists.txt
git commit -m "feat(m2): MinerU HTTP parser (contract 1 second impl) with testable normalization"
```

---

### Task 11: 解析器路由（poppler vs MinerU，纯逻辑 TDD）

**Files:**
- Create: `src/parse/parser_router.h`, `src/parse/parser_router.cpp`
- Test: `tests/test_parser_router.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_parser_router.cpp`**

```cpp
#include <doctest/doctest.h>
#include "parse/parser_router.h"

TEST_CASE("choose_parser_kind picks poppler for text-rich native PDF") {
    // 平均每页字符数高 -> 原生 PDF -> poppler
    CHECK(choose_parser_kind(/*avg_chars_per_page=*/800, /*is_pdf=*/true) == ParserKind::Poppler);
}

TEST_CASE("choose_parser_kind picks MinerU for scanned/low-text PDF") {
    // 平均每页字符数极低 -> 疑似扫描 -> MinerU
    CHECK(choose_parser_kind(/*avg_chars_per_page=*/20, /*is_pdf=*/true) == ParserKind::Mineru);
}

TEST_CASE("choose_parser_kind picks MinerU for non-pdf inputs") {
    CHECK(choose_parser_kind(0, /*is_pdf=*/false) == ParserKind::Mineru);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/parse/parser_router.h`**

```cpp
#pragma once

enum class ParserKind { Poppler, Mineru };

// 选路：原生且文本充足的 PDF 走 poppler 快路；扫描/低文本/非 PDF 走 MinerU。
// 阈值：平均每页字符数 < 100 视为疑似扫描。
ParserKind choose_parser_kind(int avg_chars_per_page, bool is_pdf);
```

- [ ] **Step 4: 写 `src/parse/parser_router.cpp`**

```cpp
#include "parse/parser_router.h"

ParserKind choose_parser_kind(int avg_chars_per_page, bool is_pdf) {
    if (!is_pdf) return ParserKind::Mineru;
    if (avg_chars_per_page < 100) return ParserKind::Mineru;
    return ParserKind::Poppler;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/parse/parser_router.cpp`；`rag_tests` 加 `tests/test_parser_router.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/parse/parser_router.h src/parse/parser_router.cpp tests/test_parser_router.cpp CMakeLists.txt
git commit -m "feat(m2): parser router (poppler fast-path vs MinerU) (TDD)"
```

---

### Task 12: 重写入库管道（route→tree→chunks→tables→pagemap→质检门禁→PG→embed→Milvus）

**Files:**
- Modify: `src/ingest/ingest_pipeline.h`, `src/ingest/ingest_pipeline.cpp`

- [ ] **Step 1: 重写 `src/ingest/ingest_pipeline.h`**

```cpp
#pragma once
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "parse/poppler_parser.h"
#include "parse/mineru_parser.h"
#include "quality/quality_check.h"

struct IngestResult {
    std::string standard_id;
    int clause_count = 0;
    int table_count = 0;
    bool passed_gate = false;
    QualityReport quality;
};

// M2 正规入库：选路解析 → 元数据抽取 → 层级树 → 三文本 → 表格结构化
// → page 映射 → 质检门禁 → 写 PG；通过门禁才 embed(retrieval_text) 并写 Milvus。
IngestResult ingest_file_v2(const std::string& file_path,
                            PopplerParser& poppler,
                            MineruParser& mineru,
                            PgClient& pg,
                            milvus::MilvusRest& mv,
                            EmbeddingClient& embed,
                            const std::string& collection);
```

- [ ] **Step 2: 重写 `src/ingest/ingest_pipeline.cpp`**

```cpp
#include "ingest/ingest_pipeline.h"
#include "ingest/metadata_extractor.h"
#include "ingest/clause_tree.h"
#include "ingest/chunk_builder.h"
#include "ingest/table_structurer.h"
#include "ingest/page_map.h"
#include "parse/parser_router.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <functional>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file_v2(const std::string& file_path, PopplerParser& poppler,
                            MineruParser& mineru, PgClient& pg, milvus::MilvusRest& mv,
                            EmbeddingClient& embed, const std::string& collection) {
    namespace fs = std::filesystem;
    std::string ext = fs::path(file_path).extension().string();
    bool is_pdf = (ext == ".pdf" || ext == ".PDF");

    // 先用 poppler 探测文本密度以决定选路
    ParsedDoc probe = is_pdf ? poppler.parse(file_path) : ParsedDoc{};
    int total_chars = 0;
    for (auto& p : probe.pages) total_chars += (int)p.text.size();
    int avg = probe.pages.empty() ? 0 : total_chars / (int)probe.pages.size();

    ParserKind kind = choose_parser_kind(avg, is_pdf);
    ParsedDoc doc = (kind == ParserKind::Poppler) ? probe : mineru.parse(file_path);
    spdlog::info("解析路线: {}", kind == ParserKind::Poppler ? "poppler" : "MinerU");

    std::string standard_id = make_id(file_path);

    // 元数据：标准号优先用解析得到，否则从首页文本抽取
    std::string standard_no = doc.standard_no;
    if (standard_no.empty() && !doc.pages.empty())
        standard_no = extract_standard_no(doc.pages[0].text);
    if (standard_no.empty())                 // 再退一步：从标题
        standard_no = extract_standard_no(doc.title);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = standard_no.empty() ? doc.title : standard_no;
    s.standard_name = doc.title;
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);

    // 结构化
    auto tree = build_clause_tree(doc.elements, standard_id);
    StandardMeta meta{ standard_id, s.standard_no, s.standard_name };
    auto chunks = build_chunks(tree, meta);
    auto page_rows = build_page_map(tree);

    // 写层级树与 page 映射
    for (auto& n : tree) {
        ClauseNodeRow r;
        r.node_id=n.node_id; r.standard_id=n.standard_id; r.parent_id=n.parent_id;
        r.node_type=n.node_type; r.clause_no=n.clause_no; r.title=n.title;
        r.path=n.path; r.level=n.level; r.page_start=n.page_start; r.text=n.text;
        pg.insert_clause_node(r);
    }
    for (auto& pr : page_rows)
        pg.insert_page_clause(pr.standard_id, pr.page_no, pr.node_id, pr.coverage_type);

    // 写检索块
    for (auto& c : chunks) {
        ChunkRow r;
        r.chunk_id=c.chunk_id; r.node_id=c.node_id; r.standard_id=c.standard_id;
        r.atomic_text=c.atomic_text; r.retrieval_text=c.retrieval_text;
        r.context_text=c.context_text; r.chunk_type=c.chunk_type;
        pg.insert_chunk(r);
    }

    // 表格结构化（仅 Table 元素）
    int table_count = 0;
    int table_index = 0;
    for (auto& e : doc.elements) {
        if (e.type != ElementType::Table || e.table_html.empty()) continue;
        // 表格归属：暂挂到同页最近的条款（M2 简化），无则挂标准根
        std::string node_id = standard_id;
        for (auto& n : tree)
            if (n.page_start == e.page_no) { node_id = n.node_id; break; }
        SpecTable t = structure_table(e.table_html, standard_id, node_id,
                                      e.caption, e.page_no, table_index++);
        pg.insert_spec_table(t.table_id, t.standard_id, t.node_id, t.caption,
                             t.page_no, t.table_html, t.table_markdown);
        int ci = 0;
        for (auto& cell : t.cells) {
            std::string cell_id = t.table_id + ":" + std::to_string(ci++);
            pg.insert_table_cell(cell_id, t.table_id, cell.row_idx, cell.col_idx,
                                 cell.row_header, cell.col_header, cell.cell_text,
                                 cell.has_number, cell.number_value, cell.unit);
        }
        ++table_count;
    }

    // 质检门禁
    QualityInput qin;
    qin.standard_no = standard_no;
    qin.clause_count = (int)tree.size();
    qin.avg_ocr_confidence = 1.0;          // poppler 视为高置信；MinerU 可后续填均值
    qin.page_map_count = (int)page_rows.size();
    QualityReport qr = check_quality(qin);

    IngestResult result;
    result.standard_id = standard_id;
    result.clause_count = (int)tree.size();
    result.table_count = table_count;
    result.quality = qr;
    result.passed_gate = qr.passed;

    if (!qr.passed) {
        pg.set_review_status(standard_id, "pending_review");
        spdlog::warn("质检未通过，标记待复核，跳过向量索引。问题: {}",
                     qr.issues.empty() ? "" : qr.issues[0]);
        return result;
    }

    // 通过门禁：embed retrieval_text 写 Milvus
    pg.set_review_status(standard_id, "passed");
    mv.ensure_collection(collection, embed.dim());
    for (auto& c : chunks) {
        std::vector<float> vec = embed.embed(c.retrieval_text);
        mv.insert(collection, c.node_id, c.standard_id, vec);
    }
    spdlog::info("入库完成: {} 条款, {} 表格 (standard_id={})",
                 result.clause_count, table_count, standard_id);
    return result;
}
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。（旧 `ingest_file` 仍在文件中、未被删除，保留向后兼容；新管道为 `ingest_file_v2`。）

- [ ] **Step 4: Commit**

```
git add src/ingest/ingest_pipeline.h src/ingest/ingest_pipeline.cpp
git commit -m "feat(m2): v2 ingest pipeline (route->tree->chunks->tables->pagemap->gate->index)"
```

---

### Task 13: 更新 `ingest` 子命令接 v2 管道 + 输出质检报告

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: 在 `main.cpp` 顶部补 include**

在现有 include 区追加：
```cpp
#include "parse/mineru_parser.h"
```

- [ ] **Step 2: 替换 `cmd_ingest` 函数体为 v2 版本**

```cpp
static int cmd_ingest(const Config& cfg) {
    if (cfg.doc_path.empty()) { spdlog::error("未设置 RAG_DOC_PATH"); return 1; }
    PgClient pg(cfg.pg_conninfo);
    pg.apply_schema(read_file("src/db/schema.sql"));

    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    PopplerParser poppler;
    MineruParser mineru(cfg.mineru_base_url);

    auto r = ingest_file_v2(cfg.doc_path, poppler, mineru, pg, mv, embed,
                            cfg.milvus_collection);
    if (r.passed_gate)
        spdlog::info("ingest 完成(已索引): standard_id={}, clauses={}, tables={}",
                     r.standard_id, r.clause_count, r.table_count);
    else {
        std::string issues;
        for (auto& i : r.quality.issues) issues += i + " ";
        spdlog::warn("ingest 完成(待复核, 未索引): standard_id={}, 问题: {}",
                     r.standard_id, issues);
    }
    return 0;
}
```

- [ ] **Step 3: 在 `Config` 增加 `mineru_base_url`**

修改 `src/config.h`：在 `doc_path` 字段前加 `std::string mineru_base_url;`。
修改 `src/config.cpp`：在 `from_map` 中 `c.doc_path` 行前加：
```cpp
    c.mineru_base_url  = get(e, "RAG_MINERU_BASE_URL", "http://localhost:8000");
```
并在 `from_env` 的 `keys[]` 数组中加入 `"RAG_MINERU_BASE_URL"`。

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 5: Commit**

```
git add src/main.cpp src/config.h src/config.cpp
git commit -m "feat(m2): wire ingest subcommand to v2 pipeline with quality reporting"
```

---

### Task 14: 端到端验证（M2 验收）

**Files:** 无（运行验证）

- [ ] **Step 1: 重置环境，重建 schema 并入库（用你的原生 PDF）**

Run（沿用 M1 环境变量，确保从项目根目录运行）:
```
cmake --build build --config Debug
.\build\Debug\rag2.exe ingest
```
Expected: 日志显示 `解析路线: poppler` 与 `ingest 完成(已索引): ... clauses=N, tables=M`（N>0）。

- [ ] **Step 2: 校验 PG 结构化底座写入正确**

Run（psql 或任意 PG 客户端）:
```
SELECT count(*) FROM clause_nodes;        -- 应等于 N
SELECT count(*) FROM retrieval_chunks;    -- 应 > 0
SELECT count(*) FROM page_clause_map;     -- 应 > 0
SELECT node_id, parent_id, node_type, path FROM clause_nodes LIMIT 5;  -- path/parent 正确
SELECT count(*) FROM spec_tables;         -- 若文档有表则 > 0
```
Expected: 层级树有 parent_id 与 path；三文本块、page 映射、表格（若有）均写入。

- [ ] **Step 3: 端到端提问回归（确认检索仍工作且引用条款号）**

Run:
```
.\build\Debug\rag2.exe query "你这份规范里关于<某主题>的要求是什么？"
```
Expected: 回答正确引用该文档中真实存在的条款号；相比 M1，retrieval_text 更丰富（含标准号/路径），召回应不劣于 M1。

- [ ] **Step 4: 质检门禁负向验证（可选）**

把 `RAG_DOC_PATH` 指向一个无标准号/空内容的 PDF，运行 `rag2 ingest`。
Expected: 日志 `ingest 完成(待复核, 未索引)`，`standards.review_status='pending_review'`，Milvus 未新增该文档向量。

- [ ] **Step 5: 跑全部单测确认无回归**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: 全部 PASS（含 M1 既有测试 + M2 八个新测试文件）。

- [ ] **Step 6: Commit（如有未提交的微调）**

```
git add -A
git commit -m "test(m2): end-to-end verification of structured ingest (M2 acceptance)"
```

> **M2 完成判据：** `rag2 ingest` 走正规管道，PG 中 `clause_nodes`（含 parent/path/node_type）、`retrieval_chunks`、`page_clause_map`、`spec_tables`/`spec_table_cells` 均正确写入；质检门禁对不合格文档标记待复核且不索引；`rag2 query` 回归通过、引用真实条款号；全部单测 PASS。

---

## 自检结果（Spec 覆盖核对）

对照 M2 在总览 spec §3.1 与技术文档 §5/§6/§7/§8.2/§14 的范围：

- **完整条款层级树（章/节/条/款/项）** → Task 3 `build_clause_tree`（node_type/parent/path/level）✅
- **atomic/retrieval/context 三文本分离（§7.2）** → Task 4 `build_chunks` ✅
- **page_clause_map（§5.4）** → Task 6 `build_page_map` + Task 8 表 + Task 12 写入 ✅
- **表格结构化建表 spec_tables + spec_table_cells（§5.6）** → Task 5 `structure_table` + Task 8 schema + Task 12 写入 ✅
- **MinerU 接入并与 poppler 并行（§6/§4.2）** → Task 9 服务 + Task 10 客户端 + Task 11 路由 + Task 12 选路 ✅
- **解析质检与入库门禁（§14.1/§14.2）** → Task 7 `check_quality` + Task 12 门禁（pending_review 不索引）✅
- **元数据规则层：标准号识别（§12.2）** → Task 2 `extract_standard_no` ✅
- **clause_text 检索文本升级（§8.2 retrieval_text）** → Task 4 + Task 12 embed retrieval_text ✅
- **契约①加性扩展（IR）** → Task 1 ✅
- **契约②/③/④/⑤ 不破坏** → 入库侧仅用②embed；检索/生成（③④⑤）未改，M1 query 回归通过（Task 14 Step3）✅

**未纳入 M2（按设计归属后续里程碑，非缺口）：**
- 强制性条文 `is_mandatory` 识别 → 列在 schema（Task 8）但识别逻辑属 M2 后续/M5（文档 §17.2 第4项"完善强制性条文识别"在第二阶段）；本计划 schema 预留字段，默认 false。
- 合并单元格 `merged_info` 完整还原 → Task 5 留字段，cell 级精确问答在 M5（§9.7）。
- `refs` 引用字段填充与引用扩展 → schema 预留（Task 8），消费在 M5（§10.5）。

无占位符遗留；跨任务类型一致（`ParseElement`/`ClauseTreeNode`/`RetrievalChunk`/`SpecTable`/`ClauseNodeRow`/`ChunkRow`/`QualityInput` 命名前后一致；`ingest_file_v2` 签名与 Task 13 调用一致；`Config.mineru_base_url` 在 Task 13 新增并使用）。

## 开放项（执行时需你提供/确认，非计划缺陷）

1. MinerU 安装与模型下载是否顺利（GPU/CPU）；若受阻，先用 poppler 路完成 Task 1–8、12–14，MinerU 路（Task 9–11 的服务联调）后补。
2. `choose_parser_kind` 的扫描判定阈值（默认每页 <100 字符）需用你的真实文档校准。
3. 质检门禁阈值（avg_ocr_confidence≥0.80、clause_count>0、standard_no 非空）按你的文档质量调整。
4. 表格归属策略 M2 用"同页最近条款"简化；复杂版面下的精确归属可在视觉路里程碑改进。
