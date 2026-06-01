# M1 范围修订 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在已完成的 M1 之上做 4 项最小修订——嵌入只用条款正文、落定 Qwen3-Embedding-8B/4096/SiliconFlow、切分器全角归一化+支持到四级、首页正则轻量抽真实标准号——让端到端能稳定点火、溯源标准号尽量变真，且不动任何契约/Schema。

**Architecture:** 改动集中在 ingest 路径的两个纯函数（条款切分、标准号抽取，均可 TDD）+ ingest 编排的两行接线 + 一份 env 样例。五个接口契约、PG/Milvus schema、子命令接口全部不变。

**Tech Stack:** C++17/20、VS2022 / MSBuild、doctest、std::regex、std::filesystem。嵌入经 SiliconFlow（OpenAI 兼容）。

> **构建/测试命令约定（贯穿全计划）：**
> - 构建：`& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`（PowerShell；勿用 git-bash，`/p:` 会被 MSYS 路径转义）。
> - 跑测试：运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest，全绿即通过）。
> - 跑应用：运行 `rag2.0\x64\Debug\rag2.0.exe`。
> - **本仓库自 commit 993c445 起用显式源文件清单（非通配符）：新增 `.cpp`/`.h` 必须同时登记到两个 `.vcxproj` 的 `<ClCompile>`/`<ClInclude>` 与对应 `.vcxproj.filters`；测试工程需把被测 src `.cpp` 也加进去。** 仅修改已有文件的任务无需改工程文件。

来源 spec：[2026-06-01-m1-scope-revision-design.md](../specs/2026-06-01-m1-scope-revision-design.md)

---

## 文件结构（本计划创建/修改的文件及职责）

```
rag2.0/
  src/ingest/clause_splitter.cpp        修改：加 normalize_fullwidth 纯函数 + 正则放宽到 2~4 级（修订③）
  src/ingest/standard_meta.h            新建：extract_standard_no 声明（修订④）
  src/ingest/standard_meta.cpp          新建：首页正则抽标准号、回退文件名（修订④）
  src/ingest/ingest_pipeline.cpp        修改：嵌入只用 c.text（修订①）+ 用 extract_standard_no 接线（修订④）
  .env.example                          修改：SiliconFlow / Qwen3-8B / 4096（修订②）
  tests/test_clause_splitter.cpp        修改：补全角空格/四级/单级守护用例（修订③）
  tests/test_standard_meta.cpp          新建：标准号抽取用例（修订④）
  rag2.0/rag2.0.vcxproj(.filters)       登记 standard_meta.cpp/.h
  rag2.0.tests/rag2.0.tests.vcxproj(.filters)  登记 standard_meta.cpp/.h + test_standard_meta.cpp
```

---

### Task 1: 切分器加固（全角归一化 + 支持到四级，纯函数 TDD）（修订③）

**Files:**
- Modify: `src/ingest/clause_splitter.cpp`
- Test: `tests/test_clause_splitter.cpp`

（`clause_splitter.h` 不变，`SplitClause` 结构不动；本任务不改工程文件。）

- [ ] **Step 1: 在 `tests/test_clause_splitter.cpp` 末尾追加失败测试**

在文件末尾追加以下三个用例（保留原有用例不动）：

```cpp
TEST_CASE("split_clauses recognizes clause headers separated by a full-width space") {
    // 国标常用全角空格(U+3000)分隔条款号与正文
    std::string page =
        "4.2.1　桥涵设计应符合本规范的规定。\n"
        "4.2.2　设计洪水频率应按表4.2.2取值。\n";
    auto clauses = split_clauses(page, 7);
    REQUIRE(clauses.size() == 2);
    CHECK(clauses[0].clause_no == "4.2.1");
    CHECK(clauses[0].text.find("桥涵设计应符合") != std::string::npos);
    CHECK(clauses[1].clause_no == "4.2.2");
}

TEST_CASE("split_clauses recognizes four-level clause numbers") {
    std::string page = "4.2.1.1 具体要求如下。\n4.2.1.2 另一要求。\n";
    auto clauses = split_clauses(page, 9);
    REQUIRE(clauses.size() == 2);
    CHECK(clauses[0].clause_no == "4.2.1.1");
    CHECK(clauses[1].clause_no == "4.2.1.2");
}

TEST_CASE("split_clauses normalizes full-width digits in the clause number") {
    // 全角数字条款号 ４.２.１
    std::string page = "４.２.１ 全角编号条款。\n";
    auto clauses = split_clauses(page, 3);
    REQUIRE(clauses.size() == 1);
    CHECK(clauses[0].clause_no == "4.2.1");
}

TEST_CASE("split_clauses does NOT treat single-level numeric headings as clauses") {
    // M1 明确跳过单级章标题，避免表格/列表误判
    std::string page = "4 桥涵设计\n2 100 200\n3 个螺栓\n";
    auto clauses = split_clauses(page, 1);
    CHECK(clauses.empty());
}
```

- [ ] **Step 2: 构建并跑测试，确认新用例失败**

Run: 先 MSBuild 构建，再运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`
Expected: 新增的全角空格、四级、全角数字三个用例 FAIL（当前正则 `\s` 不认全角空格、最多三级、不归一化全角数字）；单级守护用例 PASS。

- [ ] **Step 3: 重写 `src/ingest/clause_splitter.cpp`**

整文件替换为：

```cpp
#include "ingest/clause_splitter.h"
#include <regex>
#include <sstream>

// 把行内全角字符归一到半角，仅处理切分所需的三类：
//   全角空格 U+3000(E3 80 80) -> ' '
//   全角数字 U+FF10..FF19(EF BC 90..99) -> '0'..'9'
//   全角句点 U+FF0E(EF BC 8E) -> '.'
// 其余字节原样保留。归一化同时作用于条款号与正文（半角化对检索/展示更友好）。
static std::string normalize_fullwidth(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char b0 = static_cast<unsigned char>(in[i]);
        if (b0 >= 0xE0 && i + 2 < in.size()) {
            unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(in[i + 2]);
            if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) { out += ' '; i += 3; continue; }
            if (b0 == 0xEF && b1 == 0xBC) {
                if (b2 >= 0x90 && b2 <= 0x99) { out += static_cast<char>('0' + (b2 - 0x90)); i += 3; continue; }
                if (b2 == 0x8E) { out += '.'; i += 3; continue; }
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no) {
    // 行首条款号：2~4 级（N.N / N.N.N / N.N.N.N），保留 -x 后缀（如 4.2.1-1 / 4.2.1-a）。
    // 单级编号（"4 桥涵设计"）不识别——M1 明确跳过，避免表格/列表误判。
    // M1 临时实现，M2 由层级树正规化替换。
    static const std::regex head(R"(^\s*(\d+(?:\.\d+){1,3}(?:-[0-9a-zA-Z]+)?)\s+(.*)$)");

    std::vector<SplitClause> out;
    std::istringstream iss(page_text);
    std::string raw;
    while (std::getline(iss, raw)) {
        std::string line = normalize_fullwidth(raw);
        std::smatch m;
        if (std::regex_match(line, m, head)) {
            SplitClause c;
            c.clause_no = m[1].str();
            c.text = m[2].str();
            c.page_start = page_no;
            out.push_back(std::move(c));
        } else if (!out.empty()) {
            // 续行：去掉首尾空白后并入当前条款
            size_t a = line.find_first_not_of(" \t\r");
            if (a != std::string::npos) {
                out.back().text += line.substr(a);
            }
        }
    }
    return out;
}
```

- [ ] **Step 4: 构建并跑测试，确认全绿**

Run: MSBuild 构建后运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`
Expected: 全部 PASS（含原有 3 个 + 新增 4 个切分用例）。

- [ ] **Step 5: Commit**

```
git add src/ingest/clause_splitter.cpp tests/test_clause_splitter.cpp
git commit -m "fix: harden clause splitter (full-width normalize + up to 4 levels, skip single-level) (M1 revision 3)"
```

---

### Task 2: 标准号轻量抽取（新建纯函数 TDD）（修订④）

**Files:**
- Create: `src/ingest/standard_meta.h`
- Create: `src/ingest/standard_meta.cpp`
- Test: `tests/test_standard_meta.cpp`
- Modify（登记）: `rag2.0/rag2.0.vcxproj`、`rag2.0/rag2.0.vcxproj.filters`、`rag2.0.tests/rag2.0.tests.vcxproj`、`rag2.0.tests/rag2.0.tests.vcxproj.filters`

- [ ] **Step 1: 写失败测试 `tests/test_standard_meta.cpp`**

新建文件：

```cpp
#include <doctest/doctest.h>
#include "ingest/standard_meta.h"

TEST_CASE("extract_standard_no pulls a JTG number from cover text") {
    std::string page =
        "中华人民共和国行业标准\n"
        "公路桥涵设计通用规范\n"
        "JTG D60-2015\n"
        "2015-09-01 发布\n";
    CHECK(extract_standard_no(page, "fallback") == "JTG D60-2015");
}

TEST_CASE("extract_standard_no handles GB and GB/T forms") {
    CHECK(extract_standard_no("GB 50010-2010 混凝土结构设计规范", "fb") == "GB 50010-2010");
    CHECK(extract_standard_no("GB/T 50081-2019 普通混凝土力学性能试验方法", "fb")
          == "GB/T 50081-2019");
}

TEST_CASE("extract_standard_no falls back when no number is present") {
    CHECK(extract_standard_no("前言 本规范由交通运输部提出。", "公路桥涵设计通用规范")
          == "公路桥涵设计通用规范");
}
```

- [ ] **Step 2: 登记测试文件并构建，确认失败**

在 `rag2.0.tests/rag2.0.tests.vcxproj` 的测试 `<ClCompile>` 组里（紧跟 `test_prompt_builder.cpp` 之后）追加：
```xml
    <ClCompile Include="..\tests\test_standard_meta.cpp" />
```
在 `rag2.0.tests/rag2.0.tests.vcxproj.filters` 的 `测试` 过滤组里追加：
```xml
    <ClCompile Include="..\tests\test_standard_meta.cpp"><Filter>测试</Filter></ClCompile>
```

Run: MSBuild 构建。
Expected: 编译 FAIL（`ingest/standard_meta.h` 不存在）。

- [ ] **Step 3: 写 `src/ingest/standard_meta.h`**

```cpp
#pragma once
#include <string>

// 从首页文本里正则抽中文国标/行标标准号（JTG D60-2015 / GB 50010-2010 /
// GB/T 50081-2019 / JGJ 3-2010 等）。抽不到则返回 fallback（通常文件名去扩展名）。
// 纯函数，可单测。M1 临时实现，M2 由正规元数据抽取替换。
std::string extract_standard_no(const std::string& page_text, const std::string& fallback);
```

- [ ] **Step 4: 写 `src/ingest/standard_meta.cpp`**

```cpp
#include "ingest/standard_meta.h"
#include <regex>

std::string extract_standard_no(const std::string& page_text, const std::string& fallback) {
    // 前缀大写字母(>=2) + 可选 /字母 + 空白 + 可选字母段 + 数字(可带小数) + - + 四位年份(19xx/20xx)
    // 覆盖 JTG D60-2015 / GB 50010-2010 / GB/T 50081-2019 / JGJ 3-2010 / TB 10002-2017。
    static const std::regex pat(
        R"([A-Z]{2,}(?:/[A-Z]+)?\s*[A-Z]{0,2}\d+(?:\.\d+)?-(?:19|20)\d{2})");
    std::smatch m;
    if (std::regex_search(page_text, m, pat)) return m[0].str();
    return fallback;
}
```

- [ ] **Step 5: 登记实现文件到两个工程**

在 `rag2.0/rag2.0.vcxproj` 的 `<ClCompile>` 组里（`ingest_pipeline.cpp` 之后）追加：
```xml
    <ClCompile Include="..\src\ingest\standard_meta.cpp" />
```
在同文件 `<ClInclude>` 组里（`ingest_pipeline.h` 之后）追加：
```xml
    <ClInclude Include="..\src\ingest\standard_meta.h" />
```
在 `rag2.0/rag2.0.vcxproj.filters` 的源/头过滤组里分别追加：
```xml
    <ClCompile Include="..\src\ingest\standard_meta.cpp"><Filter>源文件\ingest</Filter></ClCompile>
```
```xml
    <ClInclude Include="..\src\ingest\standard_meta.h"><Filter>头文件\ingest</Filter></ClInclude>
```
在 `rag2.0.tests/rag2.0.tests.vcxproj` 的被测 `<ClCompile>` 组里（`ingest_pipeline.cpp` 之后）追加：
```xml
    <ClCompile Include="..\src\ingest\standard_meta.cpp" />
```
在同文件 `<ClInclude>` 组里追加：
```xml
    <ClInclude Include="..\src\ingest\standard_meta.h" />
```
在 `rag2.0.tests/rag2.0.tests.vcxproj.filters` 的 `被测源码` 与 `头文件` 过滤组里分别追加：
```xml
    <ClCompile Include="..\src\ingest\standard_meta.cpp"><Filter>被测源码</Filter></ClCompile>
```
```xml
    <ClInclude Include="..\src\ingest\standard_meta.h"><Filter>头文件</Filter></ClInclude>
```

- [ ] **Step 6: 构建并跑测试，确认全绿**

Run: MSBuild 构建后运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`
Expected: 全部 PASS（含新增 standard_meta 3 个用例）。

- [ ] **Step 7: Commit**

```
git add src/ingest/standard_meta.h src/ingest/standard_meta.cpp tests/test_standard_meta.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m "feat: lightweight standard_no extraction from cover text (M1 revision 4)"
```

---

### Task 3: ingest 编排接线（嵌入只用正文 + 接入标准号抽取）（修订①④）

**Files:**
- Modify: `src/ingest/ingest_pipeline.cpp`

编排改动，无独立单测；由构建 + Task 5 现场 `ingest` 验证。

- [ ] **Step 1: 在 `src/ingest/ingest_pipeline.cpp` 顶部补 include**

把：
```cpp
#include "ingest/ingest_pipeline.h"
#include "ingest/clause_splitter.h"
```
改为：
```cpp
#include "ingest/ingest_pipeline.h"
#include "ingest/clause_splitter.h"
#include "ingest/standard_meta.h"
```

- [ ] **Step 2: 改 standards 记录的标准号/名称来源**

把现有这段（约 16-25 行）：
```cpp
    std::string fname = std::filesystem::path(file_path).filename().string();
    std::string standard_id = make_id(file_path);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = fname;          // M1 占位
    s.standard_name = doc.title;    // M1 占位
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);
```
替换为：
```cpp
    std::string stem = std::filesystem::path(file_path).stem().string();
    std::string standard_id = make_id(file_path);
    std::string page1 = doc.pages.empty() ? std::string() : doc.pages[0].text;

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = extract_standard_no(page1, stem);  // 修订④：首页抽真号，回退文件名(去扩展名)
    s.standard_name = stem;                            // M1 仍用文件名(去扩展名)，真名留给 M2
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);
```

- [ ] **Step 3: 改嵌入文本为只用条款正文**

把：
```cpp
            // 检索文本：M1 简单拼标准号 + 条款号 + 正文（retrieval_text 雏形）
            std::string retrieval_text = s.standard_no + " " + c.clause_no + " " + c.text;
            std::vector<float> vec = embed.embed(retrieval_text);
```
替换为：
```cpp
            // 修订①：M1 只嵌条款正文（去掉占位文件名/条款号噪声）；M2 有真元数据后再升级为正规 retrieval_text
            std::vector<float> vec = embed.embed(c.text);
```

- [ ] **Step 4: 构建验证**

Run: MSBuild 构建。
Expected: 成功（应用与测试工程均编译链接通过）。

- [ ] **Step 5: Commit**

```
git add src/ingest/ingest_pipeline.cpp
git commit -m "feat: ingest embeds clause body only + uses extracted standard_no (M1 revisions 1+4)"
```

---

### Task 4: `.env.example` 切到 SiliconFlow / Qwen3-8B / 4096（修订②）

**Files:**
- Modify: `.env.example`

- [ ] **Step 1: 改 `.env.example` 的 embedding 四行**

把：
```
RAG_EMBED_BASE_URL=https://dashscope.aliyuncs.com
RAG_EMBED_PATH=/compatible-mode/v1/embeddings
RAG_EMBED_MODEL=text-embedding-v3
RAG_EMBED_DIM=1024
```
替换为：
```
RAG_EMBED_BASE_URL=https://api.siliconflow.cn
RAG_EMBED_PATH=/v1/embeddings
RAG_EMBED_MODEL=Qwen/Qwen3-Embedding-8B
RAG_EMBED_DIM=4096
```

（`config.cpp` 默认值按 spec 保持不变，由 env 覆盖。`RAG_EMBED_KEY` 留空不动。）

- [ ] **Step 2: Commit**

```
git add .env.example
git commit -m "chore: default embedding env to SiliconFlow Qwen3-Embedding-8B (4096-dim) (M1 revision 2)"
```

---

### Task 5: 现场端到端验收（需用户环境，非纯代码任务）

**前置（用户执行）：** 启动 Milvus(Docker) + PostgreSQL；设好 env（`RAG_EMBED_KEY`=SiliconFlow key、`RAG_DEEPSEEK_KEY`、`RAG_DOC_PATH`=一份**排版规整的原生 PDF** 绝对路径，以及 Task 4 的四个 embedding env）。
**⚠️ 若此前已用 1024 维建过 `clause_text` 集合：先删除该集合再入库**，否则 4096 维插入会维度不匹配报错。

- [ ] **Step 1: 入库**

Run（项目根目录）: `rag2.0\x64\Debug\rag2.0.exe ingest`
Expected: 日志 `ingest 完成: standard_id=..., clauses=N`，**N>0** 且与文档条款规模大致相称（不再因全角空格出现 `clauses=0`）。PG `clause_nodes` 有 N 行、Milvus `clause_text` entity 数 = N。

- [ ] **Step 2: 提问（M1 验收闸）**

Run: `rag2.0\x64\Debug\rag2.0.exe query "<该 PDF 中某条款相关问题>"`
Expected: 返回回答，且**引用到该文档真实存在的条款号**（与 PG `clause_no` 对得上）；若该 PDF 首页含标准号，回答里的**标准号为真**。

> **M1（修订后）完成判据：** `ingest` 写入 N>0 条；`query` 回答正确引用文档中真实存在的条款号。

---

## 自检结果（Spec 覆盖核对）

对照 spec §1 四项修订：

- **修订① 嵌入只用条款正文** → Task 3 Step 3 ✅
- **修订② Qwen3-8B/4096/SiliconFlow** → Task 4 ✅（代码不动，纯 env）
- **修订③ 切分器全角归一化 + 支持到四级 + 跳过单级** → Task 1（normalize_fullwidth + 正则 `{1,3}` + 单级守护测试）✅
- **修订④ 首页正则抽标准号、回退文件名** → Task 2（extract_standard_no）+ Task 3 Step 2 接线 ✅
- **spec §3 验收（N>0、引用真实条款号）** → Task 5 ✅
- **spec §4 测试策略（切分/抽取走 TDD，①②手动验收）** → Task 1/2 单测、Task 5 手动 ✅

类型/签名一致性：`split_clauses`/`SplitClause` 不变；新增 `extract_standard_no(const std::string&, const std::string&)` 在 Task 2 定义、Task 3 调用，签名一致。无占位符遗留。

## 开放项（执行时由用户提供）

1. SiliconFlow `RAG_EMBED_KEY`、`RAG_DEEPSEEK_KEY` 真实密钥。
2. `RAG_DOC_PATH`：一份排版规整的原生 PDF。
3. 若已存在 1024 维 `clause_text` 集合需先删（维度变更）。
