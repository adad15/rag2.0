# 评测 EvalCase 统一 Schema 重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `EvalCase` 统一为"证据组 + 生成 gold"单一内存模型，旧扁平 JSON 解析时规范化进该模型，`run_eval` 与 `run_generation_eval` 改读统一模型且**指标逐字不变**，最后删除扁平字段。

**Architecture:** 加性→迁移→删除三段式：先并存（新字段 + 旧扁平字段同时存在、parse 同时填充），用**纯函数 round-trip 恒等**（legacy→规范化→派生回 legacy 视图==原值）在单元层证明零变化，再让两个 evaluator 改读派生视图，最后删扁平字段。证据组/生成 gold 类型与派生助手都放进现有 `src/eval/dataset.{h,cpp}`（无需新增文件、无 vcxproj 改动）。

**Tech Stack:** C++20、MSBuild + vcpkg、nlohmann/json、spdlog、doctest。沿用 [[rag2-build-setup]]。

**Scope:** 见 spec [`2026-06-22-eval-schema-unification-design.md`](../specs/2026-06-22-eval-schema-unification-design.md)。只做"schema 统一 + 规范化解析 + 两 evaluator 改读统一模型、指标零变化"（决策 4）。**不**实现富检索指标（Group Recall/Complete/nDCG/Distractor）、**不**做数据集生成流水线、**不**改检索/生成逻辑。分支 V3.4。

**构建/测试（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
当前基线：180 单测全绿。实跑回归基线（Task 3-5 用，需 Milvus/PG/DeepSeek 在线）：`eval eval/dataset_seed.json 30 rule --gen` → 点查 hit@30 **5/5**、MRR **1**、coverage **22/22 14/14 5/9**、引用 **5/5**、数值(逐值) **18/18**。

---

## Task 1: 统一类型 + parse 规范化（保留扁平字段）

**Files:**
- Modify: `src/eval/dataset.h`, `src/eval/dataset.cpp`
- Test: `tests/test_dataset.cpp`

- [ ] **Step 1: 在 `src/eval/dataset.h` 加统一类型，并扩展 `EvalCase`（保留扁平字段）**

把整个 `EvalCase` 定义块（现 8-16 行）替换为：
```cpp
// 持久定位：标准号 + 方法号/条款号。chunk 重建后可据此回解析 chunk_id。
struct StableRef {
    std::string standard_no;   // 如 "JTG 3420-2020"（可空）
    std::string method_no;     // 如 "T0521-2005"（可空）
    std::string clause_no;     // 如 "5.3"（可空）
};

// 必要证据组：组内 chunk 等价（命中任一即覆盖），组间共同必要。
struct EvidenceGroup {
    std::string group_id;
    std::vector<std::string> chunk_ids;     // 首选；可空时由 stable_refs 解析
    std::vector<StableRef>  stable_refs;     // 持久锚点；引用评分也用它
};

enum class QueryType { ClauseMethodLocate, SingleFact, Procedure, Condition,
                       ParamFormulaTable, Compare, MultiEvidence, CrossClause, Unknown };
enum class Difficulty { Easy, Medium, Hard, Unknown };

// 生成答案轴 gold（M4.2）。
struct GenerationGold {
    std::vector<std::string> gold_values;   // 数值准确率（逐值）
    bool cite_required = false;             // 是否评引用（规范化时按是否点查置位）
    std::string reference_answer;           // 仅人工审核，不参与打分
};

// 评估样本（统一模型）。检索证据轴 = must_have_groups/acceptable/distractor；
// 生成答案轴 = generation。扁平字段为过渡期兼容，Task 5 删除。
struct EvalCase {
    std::string question;
    std::string note;

    // —— legacy 扁平字段（过渡保留；parse 仍填充，Task 5 删）——
    std::string gold_standard_no;
    std::string gold_clause_no;
    std::string gold_method_no;
    std::vector<std::string> gold_methods;
    std::vector<std::string> gold_values;

    // —— 统一模型（新）——
    std::string case_id;
    QueryType   query_type = QueryType::Unknown;
    Difficulty  difficulty = Difficulty::Unknown;
    bool        answerable = true;
    std::string language_variant;
    std::vector<std::string> source_standard_ids;
    std::vector<EvidenceGroup> must_have_groups;
    std::vector<std::string>   acceptable_chunks;
    std::vector<std::string>   distractor_chunks;
    GenerationGold generation;
    std::string generator_version, validation_status, expert_review;
};
```

- [ ] **Step 2: 写规范化与富格式解析的失败测试到 `tests/test_dataset.cpp`**

文件顶部已 `#include "eval/dataset.h"`。追加：
```cpp
TEST_CASE("parse normalizes legacy point-method into one evidence group") {
    auto cs = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])");
    REQUIRE(cs.size() == 1);
    REQUIRE(cs[0].must_have_groups.size() == 1);
    REQUIRE(cs[0].must_have_groups[0].stable_refs.size() == 1);
    CHECK(cs[0].must_have_groups[0].stable_refs[0].method_no == "T0521-2005");
    CHECK(cs[0].generation.cite_required == true);   // 点查参与引用
}

TEST_CASE("parse normalizes legacy point-clause into one evidence group") {
    auto cs = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])");
    REQUIRE(cs[0].must_have_groups.size() == 1);
    auto& r = cs[0].must_have_groups[0].stable_refs[0];
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.clause_no == "7.3.1");
    CHECK(cs[0].generation.cite_required == true);
}

TEST_CASE("parse normalizes legacy coverage into N groups, no citation") {
    auto cs = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005"]}])");
    REQUIRE(cs[0].must_have_groups.size() == 2);
    CHECK(cs[0].must_have_groups[0].stable_refs[0].method_no == "T0316-2024");
    CHECK(cs[0].must_have_groups[1].stable_refs[0].method_no == "T0350-2005");
    CHECK(cs[0].generation.cite_required == false);  // 覆盖查不评引用
}

TEST_CASE("parse maps legacy gold_values into generation") {
    auto cs = parse_dataset(R"([{"question":"q","gold_values":["45","390"]}])");
    REQUIRE(cs[0].generation.gold_values.size() == 2);
    CHECK(cs[0].generation.gold_values[0] == "45");
    CHECK(cs[0].must_have_groups.empty());           // 纯数值题无证据组
}

TEST_CASE("parse reads rich evidence-group format directly") {
    auto cs = parse_dataset(R"([{
        "question":"q","query_type":"multi_evidence","difficulty":"hard","answerable":true,
        "must_have_groups":[
          {"group_id":"g1","chunk_ids":["c1","c2"],"stable_refs":[{"standard_no":"S","method_no":"T1"}]},
          {"group_id":"g2","chunk_ids":["c3"],"stable_refs":[{"standard_no":"S","clause_no":"5.3"}]}
        ],
        "acceptable_chunks":["a1"],"distractor_chunks":["d1"],
        "generation":{"gold_values":["25"],"cite_required":true}
    }])");
    REQUIRE(cs[0].must_have_groups.size() == 2);
    CHECK(cs[0].must_have_groups[0].chunk_ids.size() == 2);
    CHECK(cs[0].must_have_groups[1].stable_refs[0].clause_no == "5.3");
    CHECK(cs[0].acceptable_chunks[0] == "a1");
    CHECK(cs[0].distractor_chunks[0] == "d1");
    CHECK(cs[0].query_type == QueryType::MultiEvidence);
    CHECK(cs[0].difficulty == Difficulty::Hard);
    CHECK(cs[0].generation.cite_required == true);
    CHECK(cs[0].generation.gold_values[0] == "25");
}
```

- [ ] **Step 3: 运行新测试，确认失败**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*normalizes*","*rich evidence-group*","*gold_values into generation*"
```
Expected：FAIL（parse_dataset 还没填 `must_have_groups`/`generation`/`query_type` 等）。

- [ ] **Step 4: 在 `src/eval/dataset.cpp` 实现富格式解析 + 规范化**

整文件替换为：
```cpp
#include "eval/dataset.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

namespace {

QueryType parse_query_type(const std::string& s) {
    if (s == "clause_method_locate") return QueryType::ClauseMethodLocate;
    if (s == "single_fact")          return QueryType::SingleFact;
    if (s == "procedure")            return QueryType::Procedure;
    if (s == "condition")            return QueryType::Condition;
    if (s == "param_formula_table")  return QueryType::ParamFormulaTable;
    if (s == "compare")              return QueryType::Compare;
    if (s == "multi_evidence")       return QueryType::MultiEvidence;
    if (s == "cross_clause")         return QueryType::CrossClause;
    return QueryType::Unknown;
}
Difficulty parse_difficulty(const std::string& s) {
    if (s == "easy")   return Difficulty::Easy;
    if (s == "medium") return Difficulty::Medium;
    if (s == "hard")   return Difficulty::Hard;
    return Difficulty::Unknown;
}
std::vector<std::string> str_array(const json& item, const char* key) {
    std::vector<std::string> v;
    if (item.contains(key) && item[key].is_array())
        for (const auto& e : item[key]) if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}

void parse_rich(const json& item, EvalCase& c) {
    c.case_id          = item.value("case_id", "");
    c.query_type       = parse_query_type(item.value("query_type", ""));
    c.difficulty       = parse_difficulty(item.value("difficulty", ""));
    c.answerable       = item.value("answerable", true);
    c.language_variant = item.value("language_variant", "");
    c.source_standard_ids = str_array(item, "source_standard_ids");
    c.acceptable_chunks   = str_array(item, "acceptable_chunks");
    c.distractor_chunks   = str_array(item, "distractor_chunks");

    if (item.contains("must_have_groups") && item["must_have_groups"].is_array()) {
        for (const auto& g : item["must_have_groups"]) {
            if (!g.is_object()) continue;
            EvidenceGroup eg;
            eg.group_id  = g.value("group_id", "");
            eg.chunk_ids = str_array(g, "chunk_ids");
            if (g.contains("stable_refs") && g["stable_refs"].is_array())
                for (const auto& r : g["stable_refs"]) {
                    if (!r.is_object()) continue;
                    StableRef sr;
                    sr.standard_no = r.value("standard_no", "");
                    sr.method_no   = r.value("method_no", "");
                    sr.clause_no   = r.value("clause_no", "");
                    eg.stable_refs.push_back(std::move(sr));
                }
            c.must_have_groups.push_back(std::move(eg));
        }
    }
    if (item.contains("generation") && item["generation"].is_object()) {
        const auto& g = item["generation"];
        c.generation.gold_values      = str_array(g, "gold_values");
        c.generation.cite_required    = g.value("cite_required", false);
        c.generation.reference_answer = g.value("reference_answer", "");
    }
    if (item.contains("provenance") && item["provenance"].is_object()) {
        const auto& p = item["provenance"];
        c.generator_version = p.value("generator_version", "");
        c.validation_status = p.value("validation_status", "");
        c.expert_review     = p.value("expert_review", "");
    }
}

// 旧扁平字段 → 统一模型。富格式已给出 must_have_groups 时不覆盖。
void normalize_legacy(EvalCase& c) {
    if (c.must_have_groups.empty()) {
        if (!c.gold_methods.empty()) {                  // 覆盖查 → N 组，不评引用
            for (size_t i = 0; i < c.gold_methods.size(); ++i) {
                EvidenceGroup g; g.group_id = "m" + std::to_string(i);
                StableRef r; r.method_no = c.gold_methods[i];
                g.stable_refs.push_back(std::move(r));
                c.must_have_groups.push_back(std::move(g));
            }
        } else if (!c.gold_method_no.empty()) {         // 点查·方法 → 1 组 + 评引用
            EvidenceGroup g; g.group_id = "m0";
            StableRef r; r.method_no = c.gold_method_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        } else if (!c.gold_clause_no.empty() && !c.gold_standard_no.empty()) {  // 点查·条款
            EvidenceGroup g; g.group_id = "c0";
            StableRef r; r.standard_no = c.gold_standard_no; r.clause_no = c.gold_clause_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        }
        // gold_clause_no 缺 gold_standard_no：不构造组、不评引用（与旧行为一致，不计分）。
    }
    if (c.generation.gold_values.empty() && !c.gold_values.empty())
        c.generation.gold_values = c.gold_values;       // 兼容顶层旧写法
}

}  // namespace

std::vector<EvalCase> parse_dataset(const std::string& json_text) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        throw std::runtime_error("eval dataset must be a JSON array");

    std::vector<EvalCase> out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        EvalCase c;
        c.question         = item.value("question", "");
        c.note             = item.value("note", "");
        // 扁平字段（过渡保留）
        c.gold_standard_no = item.value("gold_standard_no", "");
        c.gold_clause_no   = item.value("gold_clause_no", "");
        c.gold_method_no   = item.value("gold_method_no", "");
        c.gold_methods     = str_array(item, "gold_methods");
        c.gold_values      = str_array(item, "gold_values");
        // 富格式 + 规范化
        parse_rich(item, c);
        normalize_legacy(c);
        out.push_back(std::move(c));
    }
    return out;
}
```

- [ ] **Step 5: 运行测试，确认通过 + 全量**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*normalizes*","*rich evidence-group*","*gold_values into generation*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：新用例全 PASS；全量绿（185 用例，0 failed——本任务 +5）。现有 `parse_dataset` 旧用例仍绿（扁平字段照旧填充）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/dataset.h src/eval/dataset.cpp tests/test_dataset.cpp
git commit -F - <<'EOF'
feat(eval): 统一 EvalCase 类型 + parse 富格式/规范化 (保留扁平字段)

加 StableRef/EvidenceGroup/QueryType/Difficulty/GenerationGold；parse 解析富格式并把
旧扁平 gold 规范化进 must_have_groups/generation；扁平字段过渡保留，evaluator 暂不变。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 2: 纯派生助手 + round-trip 恒等证明

**Files:**
- Modify: `src/eval/dataset.h`, `src/eval/dataset.cpp`
- Test: `tests/test_dataset.cpp`

- [ ] **Step 1: 在 `src/eval/dataset.h` 末尾（`parse_dataset` 声明后）加派生视图类型与函数声明**

```cpp
// ——— 从统一模型派生"现状 evaluator 视图"，用于零变化迁移 ———

enum class LegacyKind { None, Coverage, PointMethod, PointClause };

// 检索侧：把 must_have_groups 还原为 run_eval 需要的 gold 视图。
// 规则：>1 组 → Coverage；==1 组 → 看其首条 stable_ref 是 method 还是 clause。
struct LegacyRetrievalView {
    LegacyKind kind = LegacyKind::None;
    std::vector<std::string> gold_methods;   // Coverage
    std::string gold_method_no;              // PointMethod
    std::string gold_clause_no;              // PointClause
    std::string gold_standard_no;            // PointClause
};
LegacyRetrievalView derive_legacy_view(const EvalCase& c);

// 生成侧：引用目标（已对方法号取 stem 去年份）+ 数值 gold。
struct CiteTarget { std::string gold_standard_code; std::string gold_ref; };
struct GenerationView {
    std::vector<CiteTarget> cite_targets;    // 空=不评引用
    std::vector<std::string> gold_values;
};
GenerationView derive_generation_view(const EvalCase& c);
```

- [ ] **Step 2: 写 round-trip 恒等测试到 `tests/test_dataset.cpp`**

```cpp
TEST_CASE("derive_legacy_view round-trips legacy point-method") {
    auto c = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::PointMethod);
    CHECK(v.gold_method_no == "T0521-2005");
}

TEST_CASE("derive_legacy_view round-trips legacy point-clause") {
    auto c = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::PointClause);
    CHECK(v.gold_clause_no == "7.3.1");
    CHECK(v.gold_standard_no == "JTC 5210-2018");
}

TEST_CASE("derive_legacy_view round-trips legacy coverage in order") {
    auto c = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005","T0506-2005"]}])")[0];
    auto v = derive_legacy_view(c);
    CHECK(v.kind == LegacyKind::Coverage);
    REQUIRE(v.gold_methods.size() == 3);
    CHECK(v.gold_methods[0] == "T0316-2024");
    CHECK(v.gold_methods[2] == "T0506-2005");
}

TEST_CASE("derive_generation_view: point-method gives one cite target, stemmed, empty std") {
    auto c = parse_dataset(R"([{"question":"q","gold_method_no":"T0521-2005"}])")[0];
    auto v = derive_generation_view(c);
    REQUIRE(v.cite_targets.size() == 1);
    CHECK(v.cite_targets[0].gold_ref == "T0521");          // 去年份
    CHECK(v.cite_targets[0].gold_standard_code == "");      // 方法题无标准号
    CHECK(v.gold_values.empty());
}

TEST_CASE("derive_generation_view: point-clause gives std+clause cite target") {
    auto c = parse_dataset(R"([{"question":"q","gold_standard_no":"JTC 5210-2018","gold_clause_no":"7.3.1"}])")[0];
    auto v = derive_generation_view(c);
    REQUIRE(v.cite_targets.size() == 1);
    CHECK(v.cite_targets[0].gold_standard_code == "JTC 5210-2018");
    CHECK(v.cite_targets[0].gold_ref == "7.3.1");
}

TEST_CASE("derive_generation_view: coverage has no cite targets (not cite-scored)") {
    auto c = parse_dataset(R"([{"question":"q","gold_methods":["T0316-2024","T0350-2005"]}])")[0];
    auto v = derive_generation_view(c);
    CHECK(v.cite_targets.empty());
}

TEST_CASE("derive_generation_view: numeric values pass through") {
    auto c = parse_dataset(R"([{"question":"q","gold_values":["45","390"]}])")[0];
    auto v = derive_generation_view(c);
    CHECK(v.cite_targets.empty());
    REQUIRE(v.gold_values.size() == 2);
    CHECK(v.gold_values[0] == "45");
}
```

- [ ] **Step 3: 运行测试，确认失败（链接错误：函数未定义）**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected：链接失败（`derive_legacy_view`/`derive_generation_view` 未定义）。

- [ ] **Step 4: 在 `src/eval/dataset.cpp` 实现派生助手**

把匿名命名空间里（`}  // namespace` 之前）加 `method_stem` 助手：
```cpp
std::string method_stem(const std::string& m) { return m.substr(0, m.find('-')); }
```
在文件末尾（`parse_dataset` 定义之后）追加：
```cpp
LegacyRetrievalView derive_legacy_view(const EvalCase& c) {
    LegacyRetrievalView v;
    if (c.must_have_groups.size() > 1) {
        v.kind = LegacyKind::Coverage;
        for (const auto& g : c.must_have_groups)
            if (!g.stable_refs.empty() && !g.stable_refs[0].method_no.empty())
                v.gold_methods.push_back(g.stable_refs[0].method_no);
    } else if (c.must_have_groups.size() == 1 && !c.must_have_groups[0].stable_refs.empty()) {
        const StableRef& r = c.must_have_groups[0].stable_refs[0];
        if (!r.method_no.empty()) {
            v.kind = LegacyKind::PointMethod;
            v.gold_method_no = r.method_no;
        } else if (!r.clause_no.empty()) {
            v.kind = LegacyKind::PointClause;
            v.gold_clause_no = r.clause_no;
            v.gold_standard_no = r.standard_no;
        }
    }
    return v;
}

GenerationView derive_generation_view(const EvalCase& c) {
    GenerationView v;
    v.gold_values = c.generation.gold_values;
    if (c.generation.cite_required) {
        for (const auto& g : c.must_have_groups) {
            if (g.stable_refs.empty()) continue;
            const StableRef& r = g.stable_refs[0];
            CiteTarget t;
            if (!r.method_no.empty()) { t.gold_ref = method_stem(r.method_no); t.gold_standard_code = r.standard_no; }
            else if (!r.clause_no.empty()) { t.gold_ref = r.clause_no; t.gold_standard_code = r.standard_no; }
            if (!t.gold_ref.empty()) v.cite_targets.push_back(std::move(t));
        }
    }
    return v;
}
```

- [ ] **Step 5: 运行测试，确认通过 + 全量**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe --test-case="*derive_legacy_view*","*derive_generation_view*"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：派生用例全 PASS；全量绿（192 用例，0 failed——本任务 +7）。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/dataset.h src/eval/dataset.cpp tests/test_dataset.cpp
git commit -F - <<'EOF'
feat(eval): derive_legacy_view / derive_generation_view 纯派生助手

从统一模型派生现状 evaluator 视图(检索 gold 种类 + 生成引用/数值)，round-trip 恒等单测，
为零变化迁移做证明。方法号 stem 去年份逻辑下沉到此。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 3: 迁移 `run_eval` 读统一模型（实跑回归）

**Files:**
- Modify: `src/eval/eval_runner.cpp`

- [ ] **Step 1: 用 `derive_legacy_view` 替换 `run_eval` 里的扁平字段读取**

`src/eval/eval_runner.cpp` 把这段（覆盖/点查 gold 判定，现约 40-71 行）：
```cpp
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
                if (c.gold_standard_no.empty()) {
                    spdlog::warn("样本不计分（gold_clause_no 非空但 gold_standard_no 为空）: {}",
                                 c.question);
                } else {
                    std::string sid = pg.find_standard_by_code(strip_spaces(c.gold_standard_no));
                    if (sid.empty())
                        spdlog::warn("样本不计分（标准 {} 未找到）: {}",
                                     c.gold_standard_no, c.question);
                    else { gold_key = sid + "|" + c.gold_clause_no; keys = &cand_clause_keys; }
                }
            }
            if (keys) {
                cr.scored = true;
                cr.rank = first_hit_rank(*keys, gold_key);
                rep.point_cases++;
                if (hit_at_k(cr.rank, k)) rep.point_hits++;
                rep.mrr_sum += reciprocal_rank(cr.rank);
            }
            // keys==nullptr：无有效 gold → 不计分（cr.scored 保持 false）
        }
        rep.results.push_back(cr);
```
替换为（语义逐字等价，仅 gold 来源换成派生视图）：
```cpp
        CaseResult cr;
        cr.question = c.question;

        LegacyRetrievalView gv = derive_legacy_view(c);
        if (gv.kind == LegacyKind::Coverage) {
            cr.is_coverage = true;
            cr.covered = covered_count(cand_methods, gv.gold_methods);
            cr.gold_total = static_cast<int>(gv.gold_methods.size());
            rep.coverage_cases++;
        } else {
            std::string gold_key;
            const std::vector<std::string>* keys = nullptr;
            if (gv.kind == LegacyKind::PointMethod) {
                gold_key = gv.gold_method_no;
                keys = &cand_methods;
            } else if (gv.kind == LegacyKind::PointClause) {
                std::string sid = pg.find_standard_by_code(strip_spaces(gv.gold_standard_no));
                if (sid.empty())
                    spdlog::warn("样本不计分（标准 {} 未找到）: {}", gv.gold_standard_no, c.question);
                else { gold_key = sid + "|" + gv.gold_clause_no; keys = &cand_clause_keys; }
            }
            if (keys) {
                cr.scored = true;
                cr.rank = first_hit_rank(*keys, gold_key);
                rep.point_cases++;
                if (hit_at_k(cr.rank, k)) rep.point_hits++;
                rep.mrr_sum += reciprocal_rank(cr.rank);
            }
        }
        rep.results.push_back(cr);
```
（`derive_legacy_view` 来自 `eval/dataset.h`，`eval_runner.cpp` 已 `#include "eval/dataset.h"`。`strip_spaces` 匿名助手保留。clause 缺 standard 的样本现由 `derive_legacy_view` 返回 `kind=None` → 不计分，与原"缺 standard 不计分"等价；原那条 warn 改由数据缺陷在生成期暴露，此处省略不影响指标。）

- [ ] **Step 2: 构建 + 全量单测**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误；全量绿（192，0 failed）。

- [ ] **Step 3: 实跑检索回归（需 Milvus/PG 在线）**

先确认服务：`docker ps --filter "name=milvus-standalone" --format "{{.Status}}"` 应 healthy（否则 `docker start milvus-etcd milvus-minio; docker start milvus-standalone` 等就绪）。
```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule 2>&1 | Select-String -Pattern "point|MRR|coverage@" | Out-String
```
Expected（与基线**逐字一致**）：点查 hit@30 **5/5**、MRR **1**、coverage **22/22**、**14/14**、**5/9**。不一致则按 systematic-debugging 查 `derive_legacy_view`（很可能是 coverage/point 判定或顺序）。

- [ ] **Step 4: Commit**

```powershell
git add src/eval/eval_runner.cpp
git commit -F - <<'EOF'
refactor(eval): run_eval 改读统一模型 (derive_legacy_view, 指标零变化)

覆盖/点查 gold 来源从扁平字段换成 derive_legacy_view；实测 hit@k/MRR/coverage 与基线逐字一致。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 4: 迁移 `run_generation_eval` 读统一模型（实跑回归）

**Files:**
- Modify: `src/eval/generation_eval.cpp`

- [ ] **Step 1: 用 `derive_generation_view` 替换引用/数值 gold 读取**

`src/eval/generation_eval.cpp`：删除匿名命名空间里的 `method_stem`（已下沉到 dataset.cpp）。确认顶部 `#include "eval/dataset.h"`（经 `generation_eval.h` 间接已含；若直接用更稳可加）。

把循环体里这段（现约 46-59 行）：
```cpp
        // 选引用 gold：方法号优先（取 stem）；否则条款号+标准号。
        std::string gold_ref, gold_std;
        if (!c.gold_method_no.empty()) {
            gold_ref = method_stem(c.gold_method_no);     // gold_std 留空：方法题无 gold_standard_no
        } else if (!c.gold_clause_no.empty() && !c.gold_standard_no.empty()) {
            gold_ref = c.gold_clause_no;
            gold_std = c.gold_standard_no;
        } else if (!c.gold_clause_no.empty()) {
            // spec §7：有条款号但缺 gold_standard_no → 引用不计分并告警。
            spdlog::warn("[gen-eval] 条款题缺 gold_standard_no，引用不计分: {}", c.question);
        }
        const bool want_cite = !gold_ref.empty();
        const bool want_num  = !c.gold_values.empty();
        if (!want_cite && !want_num) continue;            // 无生成 gold（如纯覆盖题）→ 跳过，不调 LLM
```
替换为：
```cpp
        GenerationView gv = derive_generation_view(c);
        const bool want_cite = !gv.cite_targets.empty();
        const bool want_num  = !gv.gold_values.empty();
        if (!want_cite && !want_num) continue;            // 无生成 gold（如纯覆盖题）→ 跳过，不调 LLM
```

再把评分段（现约 76-91 行）里的引用/数值评分改为用 `gv`。把：
```cpp
        GenCaseResult cr;
        cr.question = c.question;
        if (want_cite) {
            cr.cite_scored = true;
            cr.cite_hit = citation_hit(answer, gold_std, gold_ref);
            rep.cite_scored++;
            if (cr.cite_hit) rep.cite_hits++;
        }
        if (want_num) {
            cr.value_gold = static_cast<int>(c.gold_values.size());
            cr.value_hits = count_value_hits(answer, c.gold_values);
            rep.value_gold_total += cr.value_gold;
            rep.value_hit_total  += cr.value_hits;
        }
```
替换为（多目标=全部命中才算 cite_hit，对齐 spec 决策 2；单目标退化为现状）：
```cpp
        GenCaseResult cr;
        cr.question = c.question;
        if (want_cite) {
            cr.cite_scored = true;
            bool all = true;
            for (const auto& t : gv.cite_targets)
                if (!citation_hit(answer, t.gold_standard_code, t.gold_ref)) { all = false; break; }
            cr.cite_hit = all;
            rep.cite_scored++;
            if (cr.cite_hit) rep.cite_hits++;
        }
        if (want_num) {
            cr.value_gold = static_cast<int>(gv.gold_values.size());
            cr.value_hits = count_value_hits(answer, gv.gold_values);
            rep.value_gold_total += cr.value_gold;
            rep.value_hit_total  += cr.value_hits;
        }
```

- [ ] **Step 2: 构建 + 全量单测**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误（`method_stem` 已删、无未用告警）；全量绿（192，0 failed）。

- [ ] **Step 3: 实跑生成回归（需 Milvus/PG/DeepSeek 在线）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule --gen 2>&1 | Select-String -Pattern "引用准确率|数值准确率" | Out-String
```
Expected（与基线**逐字一致**，答案缓存命中、不花新 token）：引用准确率 **5/5**、数值准确率(逐值) **18/18**。

- [ ] **Step 4: Commit**

```powershell
git add src/eval/generation_eval.cpp
git commit -F - <<'EOF'
refactor(eval): run_generation_eval 改读统一模型 (derive_generation_view)

引用/数值 gold 来源换成 derive_generation_view；多必要组=全部引到才算命中(单组退化为现状)；
method_stem 下沉到 dataset。实测引用 5/5、数值 18/18 与基线一致。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Task 5: 删除扁平字段（最终收口 + 全回归）

**Files:**
- Modify: `src/eval/dataset.h`, `src/eval/dataset.cpp`

- [ ] **Step 1: 从 `EvalCase` 删除扁平字段**

`src/eval/dataset.h` 删除 `EvalCase` 里这 5 行：
```cpp
    std::string gold_standard_no;
    std::string gold_clause_no;
    std::string gold_method_no;
    std::vector<std::string> gold_methods;
    std::vector<std::string> gold_values;
```
（连同上方 `// —— legacy 扁平字段…` 注释一并删。）

- [ ] **Step 2: `parse_dataset` 改为把旧 JSON 键读进局部变量、只规范化**

`src/eval/dataset.cpp` 的 `parse_dataset` 与 `normalize_legacy` 调整：让 `normalize_legacy` 接收从 JSON 直接读到的旧键值，而不再依赖结构体扁平字段。

把 `normalize_legacy(EvalCase& c)` 签名改为：
```cpp
void normalize_legacy(EvalCase& c,
                      const std::string& gold_method_no,
                      const std::string& gold_clause_no,
                      const std::string& gold_standard_no,
                      const std::vector<std::string>& gold_methods,
                      const std::vector<std::string>& gold_values) {
    if (c.must_have_groups.empty()) {
        if (!gold_methods.empty()) {
            for (size_t i = 0; i < gold_methods.size(); ++i) {
                EvidenceGroup g; g.group_id = "m" + std::to_string(i);
                StableRef r; r.method_no = gold_methods[i];
                g.stable_refs.push_back(std::move(r));
                c.must_have_groups.push_back(std::move(g));
            }
        } else if (!gold_method_no.empty()) {
            EvidenceGroup g; g.group_id = "m0";
            StableRef r; r.method_no = gold_method_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        } else if (!gold_clause_no.empty() && !gold_standard_no.empty()) {
            EvidenceGroup g; g.group_id = "c0";
            StableRef r; r.standard_no = gold_standard_no; r.clause_no = gold_clause_no;
            g.stable_refs.push_back(std::move(r));
            c.must_have_groups.push_back(std::move(g));
            c.generation.cite_required = true;
        }
    }
    if (c.generation.gold_values.empty() && !gold_values.empty())
        c.generation.gold_values = gold_values;
}
```
`parse_dataset` 循环体改为（删去对 `c.gold_*` 的赋值，改用局部变量）：
```cpp
        EvalCase c;
        c.question = item.value("question", "");
        c.note     = item.value("note", "");
        std::string gold_standard_no = item.value("gold_standard_no", "");
        std::string gold_clause_no   = item.value("gold_clause_no", "");
        std::string gold_method_no   = item.value("gold_method_no", "");
        std::vector<std::string> gold_methods = str_array(item, "gold_methods");
        std::vector<std::string> gold_values  = str_array(item, "gold_values");
        parse_rich(item, c);
        normalize_legacy(c, gold_method_no, gold_clause_no, gold_standard_no, gold_methods, gold_values);
        out.push_back(std::move(c));
```

- [ ] **Step 3: 构建 + 全量单测**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected：0 编译错误（确认 `eval_runner.cpp`/`generation_eval.cpp` 已无任何 `c.gold_*` 引用——若报错说明 Task 3/4 漏迁移，回去补）；全量绿（192，0 failed）。

- [ ] **Step 4: 静态确认无残留扁平字段引用**

```powershell
Select-String -Path src\*.cpp,src\**\*.cpp,src\**\*.h -Pattern "\.gold_method_no|\.gold_clause_no|\.gold_standard_no|\.gold_methods|\.gold_values" |
  Where-Object { $_.Path -notlike "*dataset.cpp" }
```
Expected：无输出（除 dataset.cpp 内部局部变量外，全代码不再引用扁平字段成员）。

- [ ] **Step 5: 全链路实跑回归（需 Milvus/PG/DeepSeek 在线）**

```powershell
$OutputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8
.\rag2.0\x64\Debug\rag2.0.exe eval eval/dataset_seed.json 30 rule --gen 2>&1 | Select-String -Pattern "point|MRR|coverage@|引用准确率|数值准确率" | Out-String
```
Expected（与基线**逐字一致**）：点查 5/5、MRR 1、coverage 22/22 14/14 5/9、引用 5/5、数值 18/18。

- [ ] **Step 6: Commit**

```powershell
git add src/eval/dataset.h src/eval/dataset.cpp
git commit -F - <<'EOF'
refactor(eval): 删除 EvalCase 扁平 gold 字段，单一统一模型收口

evaluator 已全部改读统一模型；parse 把旧 JSON 键读进局部变量后只规范化、不再存扁平字段。
全链路 hit@k/MRR/coverage/引用/数值 与基线逐字一致。

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

## Self-Review

**1. Spec 覆盖（逐节对照 `2026-06-22-eval-schema-unification-design.md`）：**
- §3 决策 1（去扁平、统一模型、旧 JSON 兼容）→ Task 1 解析兼容 + Task 5 删字段。决策 2（多组=全引到）→ Task 4 Step 1 的 `all` 循环。决策 3（gold_values 进 generation，兼容顶层）→ Task 1 `normalize_legacy` + Task 5。决策 4（重构、零变化、富指标后续）→ 整个 plan 的实跑回归 + Scope。
- §4 JSON schema（富 + 旧扁平）→ Task 1 `parse_rich` + `normalize_legacy`。
- §5 C++ 模型 → Task 1 类型定义。
- §6 规范化映射（method/clause/coverage/values）→ Task 1 `normalize_legacy` + Task 2 round-trip 测试逐条覆盖。
- §7 消费规则（run_eval 派生、generation 派生、参与条件）→ Task 2 助手 + Task 3/4 迁移。
- §8 稳健性：富格式空组 needs_review（解析时空 stable_refs 的组仍会进 must_have_groups，但 `derive_*` 对空 stable_refs 跳过=不计分，等价不计分）；顶层 vs generation.gold_values 取后者 → `normalize_legacy` 的 `if (c.generation.gold_values.empty())` 守卫。**注**：§8 的"显式 warn/needs_review 标记"本刀未落地为状态字段（YAGNI：当前无富格式数据），仅保证"非法/缺失 → 不计分"的行为正确；状态标记留给数据集生成刀。
- §9 测试策略 → Task 1/2 纯单测 + Task 3/4/5 实跑回归。§10 验收 → 各 Task Step 的 Expected。

**2. Placeholder 扫描：** 无 TBD；类型、`normalize_legacy`、`derive_legacy_view`/`derive_generation_view`、两处 evaluator 替换、删字段均给完整代码与确切命令。

**3. 类型一致性：** `StableRef{standard_no,method_no,clause_no}`、`EvidenceGroup{group_id,chunk_ids,stable_refs}`、`GenerationGold{gold_values,cite_required,reference_answer}`、`LegacyKind`、`LegacyRetrievalView{kind,gold_methods,gold_method_no,gold_clause_no,gold_standard_no}`、`CiteTarget{gold_standard_code,gold_ref}`、`GenerationView{cite_targets,gold_values}` 在 Task 1/2 定义并被 Task 3/4 一致引用；`derive_legacy_view`/`derive_generation_view`/`method_stem` 签名前后一致；复用既有 `covered_count`/`first_hit_rank`/`hit_at_k`/`reciprocal_rank`/`citation_hit`/`count_value_hits`/`find_standard_by_code`/`strip_spaces`。

**4. 已知限制（实现者须知）：**
- `derive_legacy_view` 用"组数 >1 ⇒ Coverage、==1 ⇒ 点查"区分。单方法的覆盖查会被当作点查——当前数据集无此情形，实跑回归保证真实数据零变化；富检索指标刀落地后此区分被 Group Recall 取代。
- clause 缺 standard 的样本：现规范化为"无证据组、不计分"，原 `run_eval`/`gen-eval` 的 warn 取消（无此数据；行为=不计分，与原一致）。
- §8 的 `needs_review`/`stale_gold` 状态字段未实现（YAGNI，留数据集生成刀）。
