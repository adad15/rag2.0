# M4 检索评估核心 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立可重复运行的**检索评估核心**——评估集 JSON + 纯检索指标（点查 hit@k / MRR、列举查 coverage@method_no）+ `rag2 eval` 子命令，让"召回到底好不好、改了 query plan / 权重有没有变好"从肉眼变成有度量。

**Architecture:** 一组纯逻辑评估模块（`dataset` 解析、`retrieval_metrics` 指标，全部 doctest TDD）+ 一个 `eval_runner`（复用现有 `text_retrieve`，对每条样本回查 chunk 元数据算指标、汇总）+ `main.cpp` 的 `eval` 子命令。评估集是受版本管理的 JSON。本刀只做**检索侧**指标；生成侧指标 / 数值后置校验 / 拒答阈值标定（原 M4 §11.4）留作后续。

**Tech Stack:** C++20、MSBuild + vcpkg（**不是 CMake**）、nlohmann/json、spdlog、doctest。沿用 [[rag2-build-setup]] 的构建方式。

**Scope（M4 第一刀 = 检索评估核心）：** 交付 dataset/metrics/runner/CLI + 含"天平"覆盖用例的种子集。**不含**：生成侧指标、数值后置校验、拒答阈值标定（原 M4 后半段，另立计划）；也不在本刀改 query plan 的任何检索逻辑（只测量）。

**关联：** 原始 M4 计划 `docs/superpowers/plans/2026-05-31-m4-evaluation-loop.md`（本刀是其检索评估子集，并按现状把 CMake/C++17 更新为 MSBuild/C++20，把"天平/列举"用例纳入）。查询计划 spec `docs/superpowers/specs/2026-06-16-ragflow-lite-query-plan-design.md` §9.3。分支 V3.2。

---

## File Structure

| 文件 | 职责 |
|---|---|
| `src/eval/dataset.h` / `.cpp`（新增） | `EvalCase` 结构 + `parse_dataset(json)`（纯，nlohmann/json） |
| `src/eval/retrieval_metrics.h` / `.cpp`（新增） | 纯指标：`first_hit_rank` / `reciprocal_rank` / `hit_at_k` / `covered_count` |
| `src/eval/eval_runner.h` / `.cpp`（新增） | 编排：跑 `text_retrieve` → 回查 chunk → 算指标 → 汇总（集成，复用 mv/pg/embed/syn） |
| `src/main.cpp`（改） | 新增 `eval` 子命令 + usage |
| `eval/dataset_seed.json`（新增） | 种子评估集（天平覆盖用例 + 两条方法点查） |
| `tests/test_dataset.cpp`（新增） | `parse_dataset` 单测 |
| `tests/test_retrieval_metrics.cpp`（新增） | 指标纯函数单测 |
| `rag2.0/rag2.0.vcxproj`(+`.filters`)（改） | 登记 dataset/retrieval_metrics/eval_runner 源与头 |
| `rag2.0.tests/rag2.0.tests.vcxproj`(+`.filters`)（改） | 登记 dataset.cpp/retrieval_metrics.cpp + 两个测试（**不登记 eval_runner.cpp**，它依赖 Milvus/PG，不单测） |

**构建（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
**测试 exe：** `rag2.0.tests\x64\Debug\rag2.0.tests.exe`（doctest 过滤 `--test-case="名字"`）。**应用 exe：** `rag2.0\x64\Debug\rag2.0.exe`。

**登记要点（来自 [[rag2-build-setup]]）：** 新增 `.cpp/.h` 必须同时登记进**两个** `.vcxproj` 的 `<ClCompile>`/`<ClInclude>` 与对应 `.filters`，否则不编译。`.filters` 仅影响 VS 解决方案分组（不影响构建），新条目用顶层 `源文件`/`头文件`/`测试`/`被测源码` 过滤器即可，免去新建子文件夹的 GUID。

---

## Task 1: 评估集解析 dataset（纯函数，TDD）

**Files:**
- Create: `src/eval/dataset.h`, `src/eval/dataset.cpp`
- Test: `tests/test_dataset.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`(+`.filters`), `rag2.0.tests/rag2.0.tests.vcxproj`(+`.filters`)

- [ ] **Step 1: 写 `src/eval/dataset.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 评估样本（M4 检索评估核心）。两种 gold：
//  - 点查：gold_clause_no（配 gold_standard_no）或 gold_method_no 任一非空。
//  - 覆盖查：gold_methods 非空——衡量召回覆盖到这组 method_no 的多少。
struct EvalCase {
    std::string question;
    std::string note;                       // 备注/题型（可空）
    std::string gold_standard_no;           // 标准号文本，如 "JTG 3420"（可空）
    std::string gold_clause_no;             // 条款号，如 "5.1.2"（可空）
    std::string gold_method_no;             // 方法号，如 "T0521-2005"（可空）
    std::vector<std::string> gold_methods;  // 覆盖查 gold（空=非覆盖查）
};

// 解析评估集 JSON（对象数组）。缺字段取默认（空）。非数组/解析失败抛 std::runtime_error。纯函数。
std::vector<EvalCase> parse_dataset(const std::string& json_text);
```

- [ ] **Step 2: 写 `tests/test_dataset.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/dataset.h"

TEST_CASE("parse_dataset reads point-query and coverage cases") {
    std::string json = R"([
      {"question":"T0521 需要哪些仪具","gold_method_no":"T0521-2005","note":"method"},
      {"question":"哪些试验用到天平","gold_methods":["T0502-2005","T0590-2020"],"note":"coverage"},
      {"question":"JTG 3420 第5.1.2条","gold_standard_no":"JTG 3420","gold_clause_no":"5.1.2"}
    ])";
    auto cases = parse_dataset(json);
    REQUIRE(cases.size() == 3);

    CHECK(cases[0].question == "T0521 需要哪些仪具");
    CHECK(cases[0].gold_method_no == "T0521-2005");
    CHECK(cases[0].note == "method");
    CHECK(cases[0].gold_methods.empty());

    REQUIRE(cases[1].gold_methods.size() == 2);
    CHECK(cases[1].gold_methods[0] == "T0502-2005");
    CHECK(cases[1].gold_methods[1] == "T0590-2020");

    CHECK(cases[2].gold_standard_no == "JTG 3420");
    CHECK(cases[2].gold_clause_no == "5.1.2");
    CHECK(cases[2].gold_method_no.empty());
}

TEST_CASE("parse_dataset throws on a non-array top level") {
    CHECK_THROWS_AS(parse_dataset(R"({"question":"x"})"), std::runtime_error);
}

TEST_CASE("parse_dataset handles an empty array") {
    auto cases = parse_dataset("[]");
    CHECK(cases.empty());
}
```

- [ ] **Step 3: 写 `src/eval/dataset.cpp`**

```cpp
#include "eval/dataset.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

std::vector<EvalCase> parse_dataset(const std::string& json_text) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        throw std::runtime_error("eval dataset must be a JSON array");

    std::vector<EvalCase> out;
    for (const auto& item : j) {
        EvalCase c;
        c.question        = item.value("question", "");
        c.note            = item.value("note", "");
        c.gold_standard_no = item.value("gold_standard_no", "");
        c.gold_clause_no  = item.value("gold_clause_no", "");
        c.gold_method_no  = item.value("gold_method_no", "");
        if (item.contains("gold_methods") && item["gold_methods"].is_array())
            for (const auto& m : item["gold_methods"])
                c.gold_methods.push_back(m.get<std::string>());
        out.push_back(std::move(c));
    }
    return out;
}
```

- [ ] **Step 4: 登记文件**

`rag2.0/rag2.0.vcxproj`：在 `<ClCompile Include="..\src\query\query_terms.cpp" />` 之后加：
```xml
    <ClCompile Include="..\src\eval\dataset.cpp" />
```
在 `<ClInclude Include="..\src\query\query_terms.h" />` 之后加：
```xml
    <ClInclude Include="..\src\eval\dataset.h" />
```

`rag2.0/rag2.0.vcxproj.filters`：加（顶层过滤器，免新建子文件夹）：
```xml
    <ClCompile Include="..\src\eval\dataset.cpp"><Filter>源文件</Filter></ClCompile>
    <ClInclude Include="..\src\eval\dataset.h"><Filter>头文件</Filter></ClInclude>
```

`rag2.0.tests/rag2.0.tests.vcxproj`：在 `<ClCompile Include="..\src\query\query_terms.cpp" />` 之后加被测源码与测试：
```xml
    <ClCompile Include="..\src\eval\dataset.cpp" />
    <ClCompile Include="..\tests\test_dataset.cpp" />
```

`rag2.0.tests/rag2.0.tests.vcxproj.filters`：加：
```xml
    <ClCompile Include="..\src\eval\dataset.cpp"><Filter>被测源码</Filter></ClCompile>
    <ClCompile Include="..\tests\test_dataset.cpp"><Filter>测试</Filter></ClCompile>
```

- [ ] **Step 5: 构建并运行 dataset 测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*parse_dataset*"
```
Expected：构建通过；3 个 parse_dataset 用例 PASS。再跑全量确认无回归：
```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：全绿（应为 151 用例，0 failed）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/dataset.h src/eval/dataset.cpp tests/test_dataset.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(eval): evaluation dataset parser

EvalCase（点查 gold_clause/method + 覆盖查 gold_methods）+ parse_dataset(json)，纯函数。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: 检索指标 retrieval_metrics（纯函数，TDD）

**Files:**
- Create: `src/eval/retrieval_metrics.h`, `src/eval/retrieval_metrics.cpp`
- Test: `tests/test_retrieval_metrics.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`(+`.filters`), `rag2.0.tests/rag2.0.tests.vcxproj`(+`.filters`)

- [ ] **Step 1: 写 `src/eval/retrieval_metrics.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 点查：candidate_keys 里第一个等于 gold_key 的 1-based 排名；无命中返回 0。纯函数。
int first_hit_rank(const std::vector<std::string>& candidate_keys,
                   const std::string& gold_key);

// reciprocal rank：rank>0 → 1.0/rank；否则 0.0。
double reciprocal_rank(int rank);

// hit@k：rank 落在 [1,k] 返回 true（rank==0 即未命中，恒 false）。
bool hit_at_k(int rank, int k);

// 覆盖查：candidate_methods 里出现的、属于 gold_methods 的**去重**个数。纯函数。
int covered_count(const std::vector<std::string>& candidate_methods,
                  const std::vector<std::string>& gold_methods);
```

- [ ] **Step 2: 写 `tests/test_retrieval_metrics.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/retrieval_metrics.h"

TEST_CASE("first_hit_rank returns 1-based rank of first match") {
    std::vector<std::string> keys = {"a", "b", "gold", "gold"};
    CHECK(first_hit_rank(keys, "gold") == 3);
    CHECK(first_hit_rank(keys, "a") == 1);
}

TEST_CASE("first_hit_rank returns 0 when absent") {
    std::vector<std::string> keys = {"a", "b"};
    CHECK(first_hit_rank(keys, "z") == 0);
    CHECK(first_hit_rank({}, "z") == 0);
}

TEST_CASE("reciprocal_rank and hit_at_k") {
    CHECK(reciprocal_rank(1) == doctest::Approx(1.0));
    CHECK(reciprocal_rank(4) == doctest::Approx(0.25));
    CHECK(reciprocal_rank(0) == doctest::Approx(0.0));
    CHECK(hit_at_k(3, 5));
    CHECK_FALSE(hit_at_k(6, 5));
    CHECK_FALSE(hit_at_k(0, 5));
}

TEST_CASE("covered_count counts distinct gold methods present in candidates") {
    std::vector<std::string> cand = {"T1", "T2", "T2", "Tx", "T3"};
    std::vector<std::string> gold = {"T1", "T2", "T3", "T9"};
    CHECK(covered_count(cand, gold) == 3);   // T1,T2,T3 命中；T2 重复只算一次；T9 未召回
}

TEST_CASE("covered_count is 0 for empty inputs") {
    CHECK(covered_count({}, {"T1"}) == 0);
    CHECK(covered_count({"T1"}, {}) == 0);
}
```

- [ ] **Step 3: 写 `src/eval/retrieval_metrics.cpp`**

```cpp
#include "eval/retrieval_metrics.h"
#include <algorithm>
#include <unordered_set>

int first_hit_rank(const std::vector<std::string>& candidate_keys,
                   const std::string& gold_key) {
    for (size_t i = 0; i < candidate_keys.size(); ++i)
        if (candidate_keys[i] == gold_key) return static_cast<int>(i) + 1;
    return 0;
}

double reciprocal_rank(int rank) {
    return rank > 0 ? 1.0 / rank : 0.0;
}

bool hit_at_k(int rank, int k) {
    return rank >= 1 && rank <= k;
}

int covered_count(const std::vector<std::string>& candidate_methods,
                  const std::vector<std::string>& gold_methods) {
    std::unordered_set<std::string> gold(gold_methods.begin(), gold_methods.end());
    std::unordered_set<std::string> seen;
    for (const auto& m : candidate_methods)
        if (gold.count(m)) seen.insert(m);
    return static_cast<int>(seen.size());
}
```

- [ ] **Step 4: 登记文件**

`rag2.0/rag2.0.vcxproj`：在 `<ClCompile Include="..\src\eval\dataset.cpp" />` 之后加：
```xml
    <ClCompile Include="..\src\eval\retrieval_metrics.cpp" />
```
在 `<ClInclude Include="..\src\eval\dataset.h" />` 之后加：
```xml
    <ClInclude Include="..\src\eval\retrieval_metrics.h" />
```

`rag2.0/rag2.0.vcxproj.filters`：加：
```xml
    <ClCompile Include="..\src\eval\retrieval_metrics.cpp"><Filter>源文件</Filter></ClCompile>
    <ClInclude Include="..\src\eval\retrieval_metrics.h"><Filter>头文件</Filter></ClInclude>
```

`rag2.0.tests/rag2.0.tests.vcxproj`：在 `<ClCompile Include="..\src\eval\dataset.cpp" />` 之后加：
```xml
    <ClCompile Include="..\src\eval\retrieval_metrics.cpp" />
    <ClCompile Include="..\tests\test_retrieval_metrics.cpp" />
```

`rag2.0.tests/rag2.0.tests.vcxproj.filters`：加：
```xml
    <ClCompile Include="..\src\eval\retrieval_metrics.cpp"><Filter>被测源码</Filter></ClCompile>
    <ClCompile Include="..\tests\test_retrieval_metrics.cpp"><Filter>测试</Filter></ClCompile>
```

- [ ] **Step 5: 构建并运行指标测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*first_hit_rank*","*reciprocal_rank*","*covered_count*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：指标用例全 PASS；全量绿（应为 156 用例，0 failed）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/retrieval_metrics.h src/eval/retrieval_metrics.cpp tests/test_retrieval_metrics.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters rag2.0.tests/rag2.0.tests.vcxproj rag2.0.tests/rag2.0.tests.vcxproj.filters
git commit -m @'
feat(eval): pure retrieval metrics

first_hit_rank / reciprocal_rank / hit_at_k（点查）+ covered_count（覆盖查），纯函数。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: eval_runner + `rag2 eval` 子命令 + 种子集 + 实测

**Files:**
- Create: `src/eval/eval_runner.h`, `src/eval/eval_runner.cpp`
- Create: `eval/dataset_seed.json`
- Modify: `src/main.cpp`
- Modify: `rag2.0/rag2.0.vcxproj`(+`.filters`)（**只加到 app 工程，不加到 tests 工程** —— 依赖 Milvus/PG，不单测）

- [ ] **Step 1: 写 `src/eval/eval_runner.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "eval/dataset.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "db/pg_client.h"
#include "query/synonyms.h"

struct CaseResult {
    std::string question;
    bool is_coverage = false;
    int rank = 0;          // 点查：首命中 1-based 排名（0=miss）
    int covered = 0;       // 覆盖查：覆盖到的 gold method 数
    int gold_total = 0;    // 覆盖查：gold method 总数
};

struct EvalReport {
    int point_cases = 0;       // 点查样本数
    int point_hits = 0;        // hit@k 命中数
    double mrr_sum = 0.0;      // 点查 reciprocal rank 之和（MRR = mrr_sum/point_cases）
    int coverage_cases = 0;    // 覆盖查样本数
    std::vector<CaseResult> results;
};

// 对每条样本跑 text_retrieve（per_path_k=k*4, top_k=k），回查 chunk 元数据算指标。
// 复用现有检索管道，不改检索逻辑。需 Milvus/PG/embedding 在线。
EvalReport run_eval(const std::vector<EvalCase>& cases,
                    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                    const SynonymDict& syn, const std::string& collection, int k);
```

- [ ] **Step 2: 写 `src/eval/eval_runner.cpp`**

```cpp
#include "eval/eval_runner.h"
#include "eval/retrieval_metrics.h"
#include "retrieve/text_search.h"
#include <optional>

namespace {
// 去空格（gold_standard_no 可能写成 "JTG 3420"，find_standard_by_code 要裸代号）。
std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s) if (c != ' ' && c != '\t') out += c;
    return out;
}
}  // namespace

EvalReport run_eval(const std::vector<EvalCase>& cases,
                    milvus::MilvusRest& mv, EmbeddingClient& embed, PgClient& pg,
                    const SynonymDict& syn, const std::string& collection, int k) {
    EvalReport rep;
    for (const auto& c : cases) {
        auto cands = text_retrieve(c.question, mv, embed, pg, syn, collection,
                                   /*per_path_k=*/k * 4, /*top_k=*/k);

        // 回查每个候选的 method_no 与 (standard_id|clause_no)
        std::vector<std::string> cand_methods;
        std::vector<std::string> cand_clause_keys;
        cand_methods.reserve(cands.size());
        cand_clause_keys.reserve(cands.size());
        for (const auto& cand : cands) {
            auto row = pg.get_chunk(cand.chunk_id);
            cand_methods.push_back(row ? row->method_no : "");
            cand_clause_keys.push_back(
                row ? (row->standard_id + "|" + row->clause_no) : "");
        }

        CaseResult cr;
        cr.question = c.question;

        if (!c.gold_methods.empty()) {
            cr.is_coverage = true;
            cr.covered = covered_count(cand_methods, c.gold_methods);
            cr.gold_total = static_cast<int>(c.gold_methods.size());
            rep.coverage_cases++;
        } else {
            std::string gold_key;
            const std::vector<std::string>* keys = nullptr;
            if (!c.gold_method_no.empty()) {
                gold_key = c.gold_method_no;
                keys = &cand_methods;
            } else if (!c.gold_clause_no.empty()) {
                std::string sid = pg.find_standard_by_code(strip_spaces(c.gold_standard_no));
                gold_key = sid + "|" + c.gold_clause_no;
                keys = &cand_clause_keys;
            }
            if (keys) {
                cr.rank = first_hit_rank(*keys, gold_key);
                rep.point_cases++;
                if (hit_at_k(cr.rank, k)) rep.point_hits++;
                rep.mrr_sum += reciprocal_rank(cr.rank);
            }
            // 无任何 gold 的样本：不计分（仅 question 入 results 供观察）
        }
        rep.results.push_back(cr);
    }
    return rep;
}
```

- [ ] **Step 3: 写种子集 `eval/dataset_seed.json`**

> gold_methods 为"仪具/材料 章节含天平"的真实方法集（由 `data/chunk_cache/5797633264338373449.json` 导出，共 23 条）。点查两条用真实方法号。后续按原 M4 §15.5 由领域人员扩到 ~100 条。

```json
[
  {
    "question": "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平",
    "note": "coverage: 仪具与材料含天平的试验",
    "gold_methods": [
      "T0502-2005","T0503-2005","T0504-2005","T0505-2020","T0506-2005",
      "T0510-2005","T0511-2005","T0512-2005","T0514-2020","T0515-2020",
      "T0516-2020","T0517-2020","T0521-2005","T0525-2020","T0526-2005",
      "T0537-2020","T0563-2005","T0583-2020","T0586-2020","T0589-2020",
      "T0590-2020","T0591-2020","T0596-2020"
    ]
  },
  {
    "question": "T0521 水泥混凝土拌合物拌和与现场取样需要哪些仪具",
    "note": "method point query",
    "gold_method_no": "T0521-2005"
  },
  {
    "question": "T0590 砂浆稠度试验需要哪些仪具与材料",
    "note": "method point query",
    "gold_method_no": "T0590-2020"
  }
]
```

- [ ] **Step 4: 在 `src/main.cpp` 加 `cmd_eval` 与派发**

先看文件顶部 include 区与 `cmd_retrievecheck` 的写法（构造 PgClient/MilvusRest/CloudEmbedding/SynonymDict 的方式照抄）。在 include 区加：
```cpp
#include "eval/dataset.h"
#include "eval/eval_runner.h"
#include <fstream>
#include <sstream>
```
（若 `<fstream>`/`<sstream>` 已包含则不重复。）

在 `cmd_retrievecheck` 函数之后，加：
```cpp
// 评估：跑评估集，打印每条命中/覆盖 + 汇总。复用 text_retrieve，不调 LLM。
static int cmd_eval(const Config& cfg, const std::string& dataset_path, int k) {
    try {
        std::ifstream f(dataset_path, std::ios::binary);
        if (!f) { spdlog::error("打不开评估集: {}", dataset_path); return 1; }
        std::stringstream ss; ss << f.rdbuf();
        auto cases = parse_dataset(ss.str());

        PgClient pg(cfg.pg_conninfo);
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                             cfg.embed_key, cfg.embed_dim);
        SynonymDict syn;
        syn.load_from_file("config/synonyms.txt");

        EvalReport rep = run_eval(cases, mv, embed, pg, syn, cfg.milvus_collection, k);

        std::cout << "\neval \"" << dataset_path << "\" | " << cases.size()
                  << " 条 (k=" << k << ")\n\n";
        for (const auto& r : rep.results) {
            if (r.is_coverage) {
                std::cout << "[coverage] " << r.covered << "/" << r.gold_total
                          << "  " << r.question << "\n";
            } else {
                std::cout << "[point   ] " << (r.rank > 0 ? "hit@" + std::to_string(r.rank)
                                                          : std::string("MISS"))
                          << "  " << r.question << "\n";
            }
        }
        std::cout << "\n--- 汇总 ---\n";
        if (rep.point_cases > 0) {
            std::cout << "点查 hit@" << k << ": " << rep.point_hits << "/" << rep.point_cases
                      << "   MRR: " << (rep.mrr_sum / rep.point_cases) << "\n";
        }
        for (const auto& r : rep.results)
            if (r.is_coverage && r.gold_total > 0)
                std::cout << "覆盖 coverage@" << k << ": "
                          << (100.0 * r.covered / r.gold_total) << "%  ("
                          << r.covered << "/" << r.gold_total << ")\n";
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] eval: {}", e.what());
        return 1;
    }
}
```

在 `main` 的 usage 行追加 `eval`，并在 `retrievecheck` 派发块之后加：
```cpp
    if (cmd == "eval") {
        if (argc < 3) { std::cout << "usage: rag2 eval <dataset.json> [k]\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
        int k = (argc >= 4) ? std::max(1, std::atoi(argv[3])) : 20;
        return cmd_eval(cfg, argv[2], k);
    }
```
（usage 字符串把 `retrievecheck` 改成 `retrievecheck|eval`。）

- [ ] **Step 5: 登记 eval_runner（仅 app 工程）**

`rag2.0/rag2.0.vcxproj`：在 `<ClCompile Include="..\src\eval\retrieval_metrics.cpp" />` 之后加：
```xml
    <ClCompile Include="..\src\eval\eval_runner.cpp" />
```
在 `<ClInclude Include="..\src\eval\retrieval_metrics.h" />` 之后加：
```xml
    <ClInclude Include="..\src\eval\eval_runner.h" />
```

`rag2.0/rag2.0.vcxproj.filters`：加：
```xml
    <ClCompile Include="..\src\eval\eval_runner.cpp"><Filter>源文件</Filter></ClCompile>
    <ClInclude Include="..\src\eval\eval_runner.h"><Filter>头文件</Filter></ClInclude>
```
（**不要**加进 tests 工程——它依赖 Milvus/PG，不单测。）

- [ ] **Step 6: 构建 + 全量测试**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误；全量测试绿（仍 156，eval_runner 不带新单测）。

- [ ] **Step 7: 确认 Milvus 在跑**

```powershell
docker ps --filter "name=milvus-standalone" --format "{{.Names}} {{.Status}}"
```
Expected：`milvus-standalone Up ... (healthy)`。若未起，按 [[local-services]] 启动（先 `docker start milvus-etcd milvus-minio` 再 `docker start milvus-standalone`），等 healthy。

- [ ] **Step 8: 实测评估集**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 20 2>&1 | Out-String
```
Expected（按当前 Phase 1 检索水平）：
- 打印 3 行 per-case：天平那条 `[coverage] N/23`，两条方法点查 `[point] hit@…`；
- 汇总：`点查 hit@20: 2/2`（方法号精确路应能命中）、`MRR: …`；`覆盖 coverage@20: …% (N/23)`。
- 记录 coverage 的实际数字——这就是 Phase 1 在"天平"列举题上的召回 baseline（预计 ~50%，因为 top-20 里混入了压力机/万能机等无天平的 仪具 chunk）。

> 这条 baseline 正是后续要不要做查询计划 Phase 2 §6.3（method_no 去重 + 覆盖）的依据。

- [ ] **Step 9: Commit**

```powershell
git add src/eval/eval_runner.h src/eval/eval_runner.cpp eval/dataset_seed.json src/main.cpp rag2.0/rag2.0.vcxproj rag2.0/rag2.0.vcxproj.filters
git commit -m @'
feat(eval): eval_runner + `rag2 eval` command + seed dataset

run_eval 复用 text_retrieve，对每条样本回查 chunk 元数据算 hit@k/MRR/coverage；
新增 eval 子命令；种子集含天平覆盖用例（23 真实方法 gold）+ 两条方法点查。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Self-Review

**1. Spec/范围覆盖：**
- 评估集加载（原 M4 Task 1 / §15.2 schema 子集）→ Task 1。
- 检索指标 recall/hit@k/MRR（原 M4 §15.1）+ coverage@method_no（查询计划 spec §6.3/§9.3 新增）→ Task 2。
- 评估运行器复用 text_retrieve（原 M4 架构）→ Task 3。
- `eval` 子命令 → Task 3 Step 4。
- 种子集（原 M4 "10 条种子" 的精简版，3 条真实可跑；扩充留领域）→ Task 3 Step 3。
- **显式不做**：生成侧指标、数值后置校验、拒答阈值标定（原 M4 §11.4 / Task 3-5）——见 Scope，另立计划。

**2. Placeholder 扫描：** 无 TBD/TODO；每个代码步给完整代码与确切命令、预期输出；gold_methods 用真实导出值（非占位）。

**3. 类型一致性：** `EvalCase{question,note,gold_standard_no,gold_clause_no,gold_method_no,gold_methods}`、`parse_dataset`、`first_hit_rank/reciprocal_rank/hit_at_k/covered_count`、`CaseResult/EvalReport/run_eval` 在 Task 1/2/3 与测试、main.cpp 调用处一致。`run_eval` 复用既有 `text_retrieve(question,mv,embed,pg,syn,collection,per_path_k,top_k)`、`pg.get_chunk→RetrievalChunkRow{method_no,clause_no,standard_id}`、`pg.find_standard_by_code` —— 均与现有代码签名一致。

**4. 已知限制（转后续）：** 生成侧评估与数值幻觉拦截未做；种子集仅 3 条（机制验证够，统计意义需扩充）；coverage gold 取"仪具/材料含天平"口径，其它列举题需各自标 gold。
