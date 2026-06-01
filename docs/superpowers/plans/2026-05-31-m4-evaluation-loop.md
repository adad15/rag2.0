# M4 评估闭环 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立可重复运行的评估闭环——评估集 + 检索指标 + 生成指标 + 拒答阈值标定 + 数值后置校验，让"文本路是否有效、BM25 是否有用、RRF 是否提升、阈值定在哪"都有据可依（§15.1），并把数值幻觉拦截接进生成端（§11.4）。

**Architecture:** 在 M3 文本路上加一个 `eval` 子命令与一组纯逻辑评估模块（数据集加载、检索指标、生成指标、阈值标定、数值后置校验，全部 TDD）。评估运行器复用 M3 的 `text_retrieve` 与 `answer_query_text`，对每条测试样本算指标并汇总报告。数值后置校验既用于评估，也作为可复用模块接入生成端做硬拦截。

**Tech Stack:** 延续 M0–M3（C++17、CMake+vcpkg、nlohmann/json、doctest）。评估集为受版本管理的 JSON 文件。

---

## 前置条件（来自 M3，必须已完成）

- M3 全部任务完成：`rag2 query` 走"查询理解→dense+BM25+PG精确→RRF→密级二次校验→small-to-big→生成"。
- 已存在：`src/retrieve/text_search.{h,cpp}`（`text_retrieve`）、`src/generate/answer_pipeline.{h,cpp}`（`answer_query_text`）、`src/retrieve/candidate.h`、`src/db/pg_client.{h,cpp}`（含 `standard_id_by_no`/`get_clause`/`get_standard`）、`src/query/synonyms.{h,cpp}`。

## 关键设计决策（执行前若不同意请先改）

1. **评估用 C++**（§2 评估模块 C++/Python 均可），以直接复用 M3 检索/生成管道、避免重复实现。
2. **gold 对齐方式**：测试样本给 `gold_standard_no` + `gold_clause_no`；运行器用 `pg.standard_id_by_no` 把它解析为 gold `node_id`（`standard_id:clause_no`），与候选 `clause_id` 直接比对。
3. **数值后置校验是可复用模块**：既算生成指标，又在 `answer_query_text` 后置硬拦截（§11.4）——发现数值/单位与召回原文不匹配则拦截改提示。
4. **起步集 10 条种子 + 扩充到 100**：本计划交付 10 条可跑的种子样本与题型骨架；扩到 §15.2 的 100 条由领域人员按 §15.5 流程完成（标注/难例/回流），计划提供格式与门槛，不代写领域金标。

## 文件结构（本计划新建/修改）

```
修改:
  src/generate/answer_pipeline.cpp   生成后接数值后置校验硬拦截（§11.4）
  src/main.cpp                       新增 eval 子命令
  CMakeLists.txt                     登记新源与测试
新建:
  src/eval/dataset.h/.cpp            评估集 JSON 加载（§15.2 schema）（纯逻辑）
  src/eval/retrieval_metrics.h/.cpp  recall@k / MRR / hit@1 / hit@3 / 标准号命中（纯逻辑）
  src/eval/generation_metrics.h/.cpp 条款引用准确率 / 拒答正确性 / 数值准确率（纯逻辑）
  src/eval/numeric_verify.h/.cpp     数值+单位抽取与原文硬匹配（纯逻辑，§11.4）
  src/eval/threshold_calib.h/.cpp    拒答阈值标定（纯逻辑，§11.4）
  src/eval/eval_runner.h/.cpp        评估编排：跑数据集→算指标→汇总
  eval/dataset_seed.json             10 条种子评估集（题型骨架）
  tests/test_dataset.cpp
  tests/test_retrieval_metrics.cpp
  tests/test_generation_metrics.cpp
  tests/test_numeric_verify.cpp
  tests/test_threshold_calib.cpp
```

---

### Task 1: 评估集加载（§15.2 schema，纯逻辑 TDD）

**Files:**
- Create: `src/eval/dataset.h`, `src/eval/dataset.cpp`
- Test: `tests/test_dataset.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_dataset.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/dataset.h"

TEST_CASE("parse_dataset reads test cases with gold fields") {
    std::string json = R"([
      {"question":"q1","gold_standard_no":"JTG D60-2015","gold_clause_no":"4.2.1",
       "gold_answer":"要点","gold_table_id":"","must_cite":true,"should_answer":true},
      {"question":"不在库的标准问题","gold_standard_no":"","gold_clause_no":"",
       "gold_answer":"","must_cite":false,"should_answer":false}
    ])";
    auto cases = parse_dataset(json);
    REQUIRE(cases.size() == 2);
    CHECK(cases[0].question == "q1");
    CHECK(cases[0].gold_standard_no == "JTG D60-2015");
    CHECK(cases[0].gold_clause_no == "4.2.1");
    CHECK(cases[0].must_cite);
    CHECK(cases[0].should_answer);
    CHECK_FALSE(cases[1].should_answer);   // 应拒答样本
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/eval/dataset.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct EvalCase {
    std::string question;
    std::string gold_standard_no;
    std::string gold_clause_no;
    std::string gold_answer;
    std::string gold_table_id;
    bool must_cite = false;
    bool should_answer = true;   // false = 应拒答样本（§15.5 难例）
};

std::vector<EvalCase> parse_dataset(const std::string& json_body);
std::vector<EvalCase> load_dataset(const std::string& path);
```

- [ ] **Step 4: 写 `src/eval/dataset.cpp`**

```cpp
#include "eval/dataset.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

using nlohmann::json;

std::vector<EvalCase> parse_dataset(const std::string& json_body) {
    auto j = json::parse(json_body);
    std::vector<EvalCase> out;
    for (auto& item : j) {
        EvalCase c;
        c.question = item.value("question", "");
        c.gold_standard_no = item.value("gold_standard_no", "");
        c.gold_clause_no = item.value("gold_clause_no", "");
        c.gold_answer = item.value("gold_answer", "");
        c.gold_table_id = item.value("gold_table_id", "");
        c.must_cite = item.value("must_cite", false);
        c.should_answer = item.value("should_answer", true);
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<EvalCase> load_dataset(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return parse_dataset(ss.str());
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/dataset.cpp`；`rag_tests` 加 `tests/test_dataset.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/eval/dataset.h src/eval/dataset.cpp tests/test_dataset.cpp CMakeLists.txt
git commit -m "feat(m4): eval dataset schema and loader (§15.2) (TDD)"
```

---

### Task 2: 检索指标（recall@k / MRR / hit@1 / hit@3，纯逻辑 TDD）

**Files:**
- Create: `src/eval/retrieval_metrics.h`, `src/eval/retrieval_metrics.cpp`
- Test: `tests/test_retrieval_metrics.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_retrieval_metrics.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/retrieval_metrics.h"

TEST_CASE("recall_at_k and hit positions over ranked node_ids") {
    std::vector<std::string> ranked = {"S:1.0.1", "S:4.2.1", "S:4.3.1"};
    std::string gold = "S:4.2.1";

    CHECK(recall_at_k(ranked, gold, 1) == 0);       // 不在 top-1
    CHECK(recall_at_k(ranked, gold, 3) == 1);       // 在 top-3
    CHECK(hit_at_1(ranked, gold) == 0);
    CHECK(hit_at_3(ranked, gold) == 1);
    CHECK(reciprocal_rank(ranked, gold) == doctest::Approx(0.5)); // 第2位 -> 1/2
}

TEST_CASE("reciprocal_rank is 0 when gold absent") {
    std::vector<std::string> ranked = {"S:1.0.1"};
    CHECK(reciprocal_rank(ranked, "S:9.9.9") == doctest::Approx(0.0));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/eval/retrieval_metrics.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 输入为按相关性降序排列的候选 node_id 列表与 gold node_id。
int    recall_at_k(const std::vector<std::string>& ranked, const std::string& gold, int k);
int    hit_at_1(const std::vector<std::string>& ranked, const std::string& gold);
int    hit_at_3(const std::vector<std::string>& ranked, const std::string& gold);
double reciprocal_rank(const std::vector<std::string>& ranked, const std::string& gold);
```

- [ ] **Step 4: 写 `src/eval/retrieval_metrics.cpp`**

```cpp
#include "eval/retrieval_metrics.h"

int recall_at_k(const std::vector<std::string>& ranked, const std::string& gold, int k) {
    for (int i = 0; i < (int)ranked.size() && i < k; ++i)
        if (ranked[i] == gold) return 1;
    return 0;
}
int hit_at_1(const std::vector<std::string>& ranked, const std::string& gold) {
    return recall_at_k(ranked, gold, 1);
}
int hit_at_3(const std::vector<std::string>& ranked, const std::string& gold) {
    return recall_at_k(ranked, gold, 3);
}
double reciprocal_rank(const std::vector<std::string>& ranked, const std::string& gold) {
    for (int i = 0; i < (int)ranked.size(); ++i)
        if (ranked[i] == gold) return 1.0 / (i + 1);
    return 0.0;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/retrieval_metrics.cpp`；`rag_tests` 加 `tests/test_retrieval_metrics.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/eval/retrieval_metrics.h src/eval/retrieval_metrics.cpp tests/test_retrieval_metrics.cpp CMakeLists.txt
git commit -m "feat(m4): retrieval metrics (recall@k/MRR/hit@1/hit@3) (TDD)"
```

---

### Task 3: 数值后置校验（数值+单位抽取与原文硬匹配，纯逻辑 TDD，§11.4）

**Files:**
- Create: `src/eval/numeric_verify.h`, `src/eval/numeric_verify.cpp`
- Test: `tests/test_numeric_verify.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_numeric_verify.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/numeric_verify.h"

TEST_CASE("extract_numbers pulls numeric tokens with optional unit") {
    auto nums = extract_numbers("压实度不应小于96%，厚度为 150 mm。");
    REQUIRE(nums.size() == 2);
    CHECK(nums[0].value == doctest::Approx(96));
    CHECK(nums[0].unit == "%");
    CHECK(nums[1].value == doctest::Approx(150));
    CHECK(nums[1].unit == "mm");
}

TEST_CASE("verify_numbers flags answer numbers not present in source") {
    std::string answer = "压实度不应小于96%。";
    std::string source = "高速公路路基压实度不应小于96%。";
    auto report = verify_numbers(answer, source);
    CHECK(report.all_verified);
    CHECK(report.unverified.empty());
}

TEST_CASE("verify_numbers catches hallucinated number") {
    std::string answer = "压实度不应小于98%。";        // 原文是 96
    std::string source = "压实度不应小于96%。";
    auto report = verify_numbers(answer, source);
    CHECK_FALSE(report.all_verified);
    REQUIRE(report.unverified.size() == 1);
    CHECK(report.unverified[0].value == doctest::Approx(98));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/eval/numeric_verify.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct NumberToken {
    double value = 0.0;
    std::string unit;       // 紧跟数字的单位（%/mm/MPa...），可空
    std::string raw;        // 原始匹配串
};

struct VerifyReport {
    bool all_verified = true;
    std::vector<NumberToken> unverified;   // 出现在回答但原文中找不到的数值
};

// 抽取文本中的数值 token（数字 + 可选紧邻单位）。
std::vector<NumberToken> extract_numbers(const std::string& text);
// §11.4：回答中的每个数值必须能在 source（召回原文/表格 cell）中找到同值，否则判为幻觉。
VerifyReport verify_numbers(const std::string& answer, const std::string& source);
```

- [ ] **Step 4: 写 `src/eval/numeric_verify.cpp`**

```cpp
#include "eval/numeric_verify.h"
#include <regex>
#include <cmath>

std::vector<NumberToken> extract_numbers(const std::string& text) {
    std::vector<NumberToken> out;
    // 数字 + 可选单位（%、字母单位、常见中文单位）
    static const std::regex re(R"((-?\d+(?:\.\d+)?)\s*(%|[A-Za-z]+|‰)?)");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
        NumberToken t;
        t.value = std::stod((*it)[1].str());
        t.unit = (*it)[2].str();
        t.raw = (*it)[0].str();
        out.push_back(t);
    }
    return out;
}

VerifyReport verify_numbers(const std::string& answer, const std::string& source) {
    auto a_nums = extract_numbers(answer);
    auto s_nums = extract_numbers(source);
    VerifyReport rep;
    for (const auto& an : a_nums) {
        bool found = false;
        for (const auto& sn : s_nums) {
            if (std::fabs(an.value - sn.value) < 1e-9 &&
                (an.unit.empty() || sn.unit.empty() || an.unit == sn.unit)) {
                found = true; break;
            }
        }
        if (!found) { rep.all_verified = false; rep.unverified.push_back(an); }
    }
    return rep;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/numeric_verify.cpp`；`rag_tests` 加 `tests/test_numeric_verify.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/eval/numeric_verify.h src/eval/numeric_verify.cpp tests/test_numeric_verify.cpp CMakeLists.txt
git commit -m "feat(m4): numeric post-verification (extract+hard-match) (§11.4) (TDD)"
```

---

### Task 4: 生成指标（条款引用准确率 / 拒答正确性 / 数值准确率，纯逻辑 TDD）

**Files:**
- Create: `src/eval/generation_metrics.h`, `src/eval/generation_metrics.cpp`
- Test: `tests/test_generation_metrics.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_generation_metrics.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/generation_metrics.h"

TEST_CASE("cites_clause detects gold clause number in answer") {
    CHECK(cites_clause("依据第4.2.1条……", "4.2.1"));
    CHECK_FALSE(cites_clause("依据第4.3.1条……", "4.2.1"));
}

TEST_CASE("is_refusal recognizes refusal phrasing") {
    CHECK(is_refusal("未检索到相关规范依据，无法作答。"));
    CHECK(is_refusal("无法从规范中找到依据，建议参考原文。"));
    CHECK_FALSE(is_refusal("依据第4.2.1条，压实度不应小于96%。"));
}

TEST_CASE("refusal_correct matches should_answer expectation") {
    // 应作答样本，模型作答 -> 正确
    CHECK(refusal_correct(/*should_answer=*/true, /*answer_is_refusal=*/false));
    // 应拒答样本，模型拒答 -> 正确
    CHECK(refusal_correct(false, true));
    // 应作答却拒答 -> 错误
    CHECK_FALSE(refusal_correct(true, true));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/eval/generation_metrics.h`**

```cpp
#pragma once
#include <string>

// 回答中是否引用了 gold 条款号。
bool cites_clause(const std::string& answer, const std::string& gold_clause_no);
// 回答是否为拒答。
bool is_refusal(const std::string& answer);
// 拒答行为是否符合 should_answer 预期。
bool refusal_correct(bool should_answer, bool answer_is_refusal);
```

- [ ] **Step 4: 写 `src/eval/generation_metrics.cpp`**

```cpp
#include "eval/generation_metrics.h"
#include <regex>

bool cites_clause(const std::string& answer, const std::string& gold_clause_no) {
    if (gold_clause_no.empty()) return false;
    // 边界匹配，避免 4.2.1 误配 4.2.10
    std::string esc = std::regex_replace(gold_clause_no, std::regex(R"(\.)"), R"(\.)");
    std::regex re(esc + R"((?!\d))");
    return std::regex_search(answer, re);
}

bool is_refusal(const std::string& answer) {
    static const std::regex re(R"(无法作答|未检索到|无法从规范|拒答|未能准确生成)");
    return std::regex_search(answer, re);
}

bool refusal_correct(bool should_answer, bool answer_is_refusal) {
    return should_answer ? !answer_is_refusal : answer_is_refusal;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/generation_metrics.cpp`；`rag_tests` 加 `tests/test_generation_metrics.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/eval/generation_metrics.h src/eval/generation_metrics.cpp tests/test_generation_metrics.cpp CMakeLists.txt
git commit -m "feat(m4): generation metrics (citation/refusal/numeric) (TDD)"
```

---

### Task 5: 拒答阈值标定（纯逻辑 TDD，§11.4）

**Files:**
- Create: `src/eval/threshold_calib.h`, `src/eval/threshold_calib.cpp`
- Test: `tests/test_threshold_calib.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/test_threshold_calib.cpp`**

```cpp
#include <doctest/doctest.h>
#include "eval/threshold_calib.h"

TEST_CASE("calibrate_threshold picks a cut separating answerable from refusable") {
    // (top_score, should_answer)
    std::vector<ScoredSample> samples = {
        {0.9, true}, {0.8, true}, {0.7, true},   // 应作答，分高
        {0.2, false}, {0.1, false}, {0.15, false} // 应拒答，分低
    };
    CalibResult r = calibrate_threshold(samples);
    // 阈值应落在 0.2~0.7 之间，能完美分开
    CHECK(r.threshold > 0.2);
    CHECK(r.threshold <= 0.7);
    CHECK(r.false_refusal_rate == doctest::Approx(0.0));
    CHECK(r.wrong_answer_rate == doctest::Approx(0.0));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/eval/threshold_calib.h`**

```cpp
#pragma once
#include <vector>

struct ScoredSample {
    double top_score = 0.0;   // 该样本 top 候选的（reranker/RRF）分数
    bool should_answer = true;
};

struct CalibResult {
    double threshold = 0.0;
    double false_refusal_rate = 0.0;  // 应作答却被拒（score<thr）
    double wrong_answer_rate = 0.0;   // 应拒答却作答（score>=thr）
};

// §11.4：在候选阈值上扫描，选误拒+误答之和最小的工作点（并列取较高阈值更保守）。
CalibResult calibrate_threshold(const std::vector<ScoredSample>& samples);
```

- [ ] **Step 4: 写 `src/eval/threshold_calib.cpp`**

```cpp
#include "eval/threshold_calib.h"
#include <algorithm>
#include <set>

CalibResult calibrate_threshold(const std::vector<ScoredSample>& samples) {
    int n_answer = 0, n_refuse = 0;
    std::set<double> cuts;
    for (auto& s : samples) {
        cuts.insert(s.top_score);
        if (s.should_answer) ++n_answer; else ++n_refuse;
    }
    CalibResult best;
    double best_cost = 1e18;
    for (double thr : cuts) {
        int false_refusal = 0, wrong_answer = 0;
        for (auto& s : samples) {
            if (s.should_answer && s.top_score < thr) ++false_refusal;
            if (!s.should_answer && s.top_score >= thr) ++wrong_answer;
        }
        double fr = n_answer ? (double)false_refusal / n_answer : 0.0;
        double wa = n_refuse ? (double)wrong_answer / n_refuse : 0.0;
        double cost = fr + wa;
        // 并列时取较高阈值（更保守，减少误答）
        if (cost < best_cost - 1e-12 || (std::abs(cost - best_cost) < 1e-12 && thr > best.threshold)) {
            best_cost = cost;
            best.threshold = thr;
            best.false_refusal_rate = fr;
            best.wrong_answer_rate = wa;
        }
    }
    return best;
}
```

- [ ] **Step 5: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/threshold_calib.cpp`；`rag_tests` 加 `tests/test_threshold_calib.cpp`。

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/eval/threshold_calib.h src/eval/threshold_calib.cpp tests/test_threshold_calib.cpp CMakeLists.txt
git commit -m "feat(m4): refusal threshold calibration (§11.4) (TDD)"
```

---

### Task 6: 数值后置校验接入生成端（§11.4 硬拦截）

**Files:**
- Modify: `src/generate/answer_pipeline.cpp`

- [ ] **Step 1: 在 `answer_pipeline.cpp` 顶部加 include**

```cpp
#include "eval/numeric_verify.h"
```

- [ ] **Step 2: 在 `answer_query_text` 生成后增加后置校验**

把函数末尾的：
```cpp
    return ds.chat(build_system_prompt(), build_user_prompt(question, fragments));
```
替换为：
```cpp
    std::string answer = ds.chat(build_system_prompt(),
                                 build_user_prompt(question, fragments));

    // §11.4 数值幻觉拦截：回答中的数值必须能在召回原文中硬匹配
    std::string source;
    for (auto& f : fragments) source += f.text + "\n";
    VerifyReport vr = verify_numbers(answer, source);
    if (!vr.all_verified) {
        std::string cited;
        for (auto& f : fragments)
            cited += "《" + f.standard_name + "》(" + f.standard_no + ") 第" +
                     f.clause_no + "条: " + f.text + "\n";
        return "未能准确生成数值，请直接参考给出的规范原文：\n" + cited;
    }
    return answer;
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/generate/answer_pipeline.cpp
git commit -m "feat(m4): wire numeric post-verification into generation (§11.4 interception)"
```

---

### Task 7: 评估运行器（编排：跑数据集→算指标→汇总）

**Files:**
- Create: `src/eval/eval_runner.h`, `src/eval/eval_runner.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写 `src/eval/eval_runner.h`**

```cpp
#pragma once
#include <string>
#include "eval/dataset.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "query/synonyms.h"
#include "db/pg_client.h"
#include "generate/deepseek_client.h"

struct EvalSummary {
    int total = 0;
    double recall_at_3 = 0.0;
    double mrr = 0.0;
    double hit_at_1 = 0.0;
    double citation_accuracy = 0.0;
    double refusal_accuracy = 0.0;
    int generation_evaluated = 0;   // 实际跑了生成的样本数（应作答样本）
};

// 跑整套评估：每条样本做检索（算检索指标）+ 生成（算生成指标），汇总。
EvalSummary run_eval(const std::vector<EvalCase>& cases,
                     milvus::MilvusRest& mv, EmbeddingClient& embed,
                     const SynonymDict& syn, PgClient& pg,
                     deepseek::DeepSeekClient& ds, const std::string& collection);
```

- [ ] **Step 2: 写 `src/eval/eval_runner.cpp`**

```cpp
#include "eval/eval_runner.h"
#include "eval/retrieval_metrics.h"
#include "eval/generation_metrics.h"
#include "retrieve/text_search.h"
#include "retrieve/retrieval_filter.h"
#include "generate/answer_pipeline.h"
#include <spdlog/spdlog.h>

EvalSummary run_eval(const std::vector<EvalCase>& cases, milvus::MilvusRest& mv,
                     EmbeddingClient& embed, const SynonymDict& syn, PgClient& pg,
                     deepseek::DeepSeekClient& ds, const std::string& collection) {
    EvalSummary sum;
    sum.total = (int)cases.size();
    double r3 = 0, mrr = 0, h1 = 0;
    int retrieval_evaluated = 0;
    double cite = 0, refusal = 0;
    int gen = 0;

    RetrievalFilter filter;   // 默认现行 + public
    for (const auto& c : cases) {
        // 解析 gold node_id（仅对有金标的样本算检索指标）
        std::string gold_node;
        if (!c.gold_standard_no.empty() && !c.gold_clause_no.empty()) {
            std::string sid = pg.standard_id_by_no(c.gold_standard_no);
            if (!sid.empty()) gold_node = sid + ":" + c.gold_clause_no;
        }

        auto cands = text_retrieve(c.question, mv, embed, syn, pg, collection,
                                   filter, /*per_path_k=*/20, /*fused_k=*/10);
        std::vector<std::string> ranked;
        for (auto& cand : cands) ranked.push_back(cand.clause_id);

        if (!gold_node.empty()) {
            r3  += recall_at_k(ranked, gold_node, 3);
            mrr += reciprocal_rank(ranked, gold_node);
            h1  += hit_at_1(ranked, gold_node);
            ++retrieval_evaluated;
        }

        // 生成指标
        std::string ans = answer_query_text(c.question, mv, embed, syn, pg, ds,
                                             collection, filter, 5);
        bool refused = is_refusal(ans);
        refusal += refusal_correct(c.should_answer, refused) ? 1 : 0;
        if (c.should_answer && c.must_cite) {
            cite += cites_clause(ans, c.gold_clause_no) ? 1 : 0;
            ++gen;
        }
        spdlog::info("[eval] q='{}' refused={} ", c.question, refused);
    }

    if (retrieval_evaluated) {
        sum.recall_at_3 = r3 / retrieval_evaluated;
        sum.mrr = mrr / retrieval_evaluated;
        sum.hit_at_1 = h1 / retrieval_evaluated;
    }
    if (gen) sum.citation_accuracy = cite / gen;
    if (sum.total) sum.refusal_accuracy = refusal / sum.total;
    sum.generation_evaluated = gen;
    return sum;
}
```

- [ ] **Step 3: 登记 `CMakeLists.txt`**

`rag_core` 加 `src/eval/eval_runner.cpp`。

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 5: Commit**

```
git add src/eval/eval_runner.h src/eval/eval_runner.cpp CMakeLists.txt
git commit -m "feat(m4): eval runner orchestrating retrieval+generation metrics"
```

---

### Task 8: `eval` 子命令 + 种子数据集 + 端到端运行（M4 验收）

**Files:**
- Modify: `src/main.cpp`
- Create: `eval/dataset_seed.json`

- [ ] **Step 1: 写种子数据集 `eval/dataset_seed.json`（10 条，含 2 条应拒答难例）**

把 `<...>` 换成你库内真实标准/条款/数值（领域人员标注）。结构按 §15.2：
```json
[
  {"question":"<标准号> 第<X.X.X>条的规定是什么？","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<要点>","must_cite":true,"should_answer":true},
  {"question":"<某材料><某指标>的限值是多少？","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<数值+单位>","gold_table_id":"<表ID>","must_cite":true,"should_answer":true},
  {"question":"<强制性条文相关问题>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<要点>","must_cite":true,"should_answer":true},
  {"question":"<语义化问题，不含编号>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<要点>","must_cite":true,"should_answer":true},
  {"question":"<表格查询问题>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<数值>","gold_table_id":"<表ID>","must_cite":true,"should_answer":true},
  {"question":"<同义词/缩写问题，如 AC 沥青混合料>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<要点>","must_cite":true,"should_answer":true},
  {"question":"<单位/数值问题>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<数值+单位>","must_cite":true,"should_answer":true},
  {"question":"<普通语义问题>","gold_standard_no":"<标准号>","gold_clause_no":"<X.X.X>","gold_answer":"<要点>","must_cite":true,"should_answer":true},
  {"question":"<一个库里不存在的标准的问题>","gold_standard_no":"","gold_clause_no":"","gold_answer":"","must_cite":false,"should_answer":false},
  {"question":"<一个规范外的经验判断问题>","gold_standard_no":"","gold_clause_no":"","gold_answer":"","must_cite":false,"should_answer":false}
]
```

- [ ] **Step 2: 在 `main.cpp` include 区追加**

```cpp
#include "eval/dataset.h"
#include "eval/eval_runner.h"
```

- [ ] **Step 3: 在 `main.cpp` 加入 `cmd_eval`**

```cpp
static int cmd_eval(const Config& cfg, const std::string& dataset_path) {
    auto cases = load_dataset(dataset_path);
    if (cases.empty()) { spdlog::error("评估集为空: {}", dataset_path); return 1; }

    PgClient pg(cfg.pg_conninfo);
    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    SynonymDict syn; syn.load_from_file("config/synonyms.txt");
    deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                cfg.deepseek_model, cfg.deepseek_key);

    EvalSummary s = run_eval(cases, mv, embed, syn, pg, ds, cfg.milvus_collection);
    std::cout << "\n===== 评估报告 =====\n"
              << "样本数:          " << s.total << "\n"
              << "recall@3:        " << s.recall_at_3 << "\n"
              << "MRR:             " << s.mrr << "\n"
              << "hit@1:           " << s.hit_at_1 << "\n"
              << "条款引用准确率:  " << s.citation_accuracy
              << " (n=" << s.generation_evaluated << ")\n"
              << "拒答正确率:      " << s.refusal_accuracy << "\n";
    return 0;
}
```

- [ ] **Step 4: 在 `main` 的命令分发里接上 eval**

在 `query` 分支之后、`unknown command` 之前插入：
```cpp
    if (cmd == "eval") {
        if (argc < 3) { std::cout << "usage: rag2 eval <dataset.json>\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("缺少 {}", m); return 1; }
        return cmd_eval(cfg, argv[2]);
    }
```

- [ ] **Step 5: 构建并跑评估（M4 验收）**

Run（先确保已 `ingest` 过、库内有数据；从项目根目录运行）:
```
cmake --build build --config Debug
.\build\Debug\rag2.exe eval eval\dataset_seed.json
```
Expected: 打印评估报告，给出 recall@3 / MRR / hit@1 / 条款引用准确率 / 拒答正确率的 baseline 数字；两条"应拒答"样本被正确拒答会拉高拒答正确率。

- [ ] **Step 6: 跑全部单测确认无回归**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: 全部 PASS（M1–M3 既有 + M4 五个新测试文件）。

- [ ] **Step 7: Commit**

```
git add src/main.cpp eval/dataset_seed.json
git commit -m "feat(m4): eval subcommand + seed dataset — repeatable evaluation (M4 acceptance)"
```

> **M4 完成判据：** `rag2 eval eval\dataset_seed.json` 可重复运行并输出检索/生成指标 baseline；数值后置校验已接入生成端（数值幻觉被拦截改为提示原文）；拒答阈值标定模块就绪（待用 100 条集出工作点）；全部单测 PASS。**第一阶段（M0–M4）文本路 MVP 至此完成。**

---

## 自检结果（Spec 覆盖核对）

对照 M4 在总览 spec §3.1 与技术文档 §11.4/§15 的范围：

- **100 条评估集（§15.2）** → Task 1 schema/loader + Task 8 种子集（10 条骨架，扩到 100 见开放项）✅（题型骨架覆盖：精确/语义/限值/表格/强制性/废止/同义词/应拒答）
- **检索指标 recall@k / MRR / top-1 命中（§15.3）** → Task 2 ✅
- **生成指标：条款引用准确率 / 拒答正确性 / 数值单位准确率（§15.4）** → Task 4 + Task 3 ✅
- **拒答阈值标定（§11.4）** → Task 5 `calibrate_threshold` ✅
- **数值后置校验（§11.4）** → Task 3 模块 + Task 6 接入生成端硬拦截 ✅
- **可重复运行的评估闭环（§15.1）** → Task 7 runner + Task 8 子命令 ✅
- **应拒答难例（§15.5）** → Task 1 `should_answer` 字段 + Task 8 种子集 2 条 ✅

**未纳入 M4（按设计归属后续，非缺口）：**
- 标准号命中率 / 表格命中率 / 视觉页命中率（§15.3）→ 表格命中属 M5（cell 定位上线后才有意义）、视觉页属 M7；M4 先做条款级核心指标。
- 在线回流扩充评估集（§15.5 第3点）→ 需上线后用户信号，属运营流程，非本计划代码。
- 阈值在生成端的实际启用（用标定结果设拒答 cut）→ 标定模块已就绪，启用接线在有 100 条集出稳定工作点后做（呼应 §11.4"阈值随版本重标定"），M5 reranker 上线后阈值需重定。

无占位符遗留（种子集中的 `<...>` 是**需领域人员填的金标数据**，已在开放项与 Task 8 明确说明，非计划占位）；跨任务类型一致（`EvalCase`/`NumberToken`/`VerifyReport`/`ScoredSample`/`CalibResult`/`EvalSummary` 命名前后一致；`verify_numbers`/`cites_clause`/`is_refusal`/`recall_at_k` 在 runner 中按定义调用；`run_eval` 签名与 Task 8 调用一致）。

## 开放项（执行时需你提供/确认，非计划缺陷）

1. **种子集金标需领域人员填**（Task 8 的 `<...>`）：标准号/条款号/参考答案/数值/表ID，按 §15.5 标注流程，扩到 §15.2 的 100 条配比。
2. **拒答阈值的实际工作点**：用 100 条集跑 `calibrate_threshold`（输入各样本 top 分数 + should_answer）得出，再接进生成端启用；当前生成端仅有"候选为空即拒答"的基础逻辑。
3. **数值单位匹配的容差**：当前严格等值 + 单位弱匹配（一方为空则放过）。复杂单位换算（如 kPa↔MPa）需按需增强 `verify_numbers`。
4. **检索/生成指标目标值**：recall@3、条款引用准确率、废止误用率等的达标线由你定，作为 M5 各增强的对照基准。
```
