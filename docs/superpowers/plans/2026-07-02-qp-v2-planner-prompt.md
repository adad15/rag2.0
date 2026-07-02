# qp-v2 LLM 查询规划 prompt 升级 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `kQueryPlannerSystemPrompt` 从 qp-v1 升到 qp-v2——所有题型产出 key_terms/sparse_text/dense_text 三件套 + 靶向 few-shot + 安全阀,版本号 bump 使旧 plan 缓存失效;四模式 rich eval 验收定去留。

**Architecture:** 纯 prompt 变更:只改 `src/query/query_planner.cpp` 的两个常量(prompt 字符串 + `kQueryPlannerPromptVersion`)。下游 `parse_llm_plan`/`text_retrieve` 的空值回退逻辑已存在,零改动。验收靠 llm planner 四模式 rich eval 对照 2026-07-01 qp-v1 baseline。

**Tech Stack:** C++20、MSBuild + vcpkg、doctest。沿用 [[rag2-build-setup]]、[[local-services]]。

**Spec:** [`docs/superpowers/specs/2026-07-02-qp-v2-planner-prompt-design.md`](../specs/2026-07-02-qp-v2-planner-prompt-design.md)

**构建/测试（PowerShell，项目根目录）：**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

**关键事实（源码勘察）：**
- 现 prompt/版本在 `src/query/query_planner.cpp:11-28`（`kQueryPlannerPromptVersion="qp-v1"`、`kQueryPlannerSystemPrompt` 含 3 个 few-shot：天平/马歇尔/路基沉降）。两者都在 `query_planner.h:30-31` 有 `extern` 声明——测试可直接断言。
- plan 缓存键含版本号（`query_planner.cpp:87` `normalize_question(q) + "\x1f" + prompt_version`）→ bump 版本即全量 miss，qp-v1 缓存文件保留在 `data/query_plan_cache/`（回滚零成本）。
- `QueryPlanner::plan`（:110-147）对**任意** intent 都会把 parsed 的 key_terms/sparse/dense 映射进 `QueryAnalysis`——GeneralFact 带非空三件套**今天就能流通**，无需改代码。
- `text_search.cpp:34-35`：dense/sparse 空则回退 `clean_text`。`list_recall` 判据含 `intent==ListByCondition`（:48-49），GeneralFact 带 key_terms 不会误入列举链路。
- 编号题在 `plan():114` 短路走规则，不受 prompt 影响。
- 测试惯例见 `tests/test_query_planner.cpp`：`make_test_planner(fn, mode)` 注入假 `llm_call` + 临时缓存目录。
- **程序只读 `config.json`、不读环境变量**——eval 切 `RAG_RERANK_MODE` 必须改文件。
- eval 需要：PG+Milvus 在线（`docker start milvus-etcd milvus-minio milvus-standalone`）、DeepSeek 余额（qp-v2 首轮 ~100 次 plan 调用，之后缓存命中）、SiliconFlow 余额（model 轮 ~100 次 rerank，query 变了旧缓存 miss；hybrid 复用 model 新缓存）。

**qp-v1 llm baseline（2026-07-01 实测，验收对照表）：**

| mode | GR@20 | Complete@20 | nDCG@1 | nDCG@20 | Red@10 | Red@20 | DH@20 | DbG |
|---|---|---|---|---|---|---|---|---|
| off | 0.9967 | 0.99 | 0.865 | 0.7939 | 0.2506 | 0.2557 | 0.894 | 0.0588 |
| light | 1.0 | 1.0 | 0.875 | 0.7894 | 0.2884 | 0.2716 | 0.882 | 0.0588 |
| model | 0.9967 | 0.99 | 0.890 | 0.8065 | 0.2569 | 0.2865 | 0.929 | 0.0824 |
| hybrid | 0.9967 | 0.99 | 0.890 | 0.8065 | 0.2572 | 0.2878 | 0.929 | 0.0824 |

**提交纪律：** 每次 pathspec 提交（`git add <files>` + `git commit -m "…" -- <files>`），绝不 `git add -A`/裸 commit。工作区有勿动的未提交文件。

---

## Task 1: qp-v2 prompt + 版本号 + doctest

**Files:**
- Modify: `src/query/query_planner.cpp:11-28`
- Test: `tests/test_query_planner.cpp`

- [ ] **Step 1: 写失败测试（`tests/test_query_planner.cpp` 末尾追加）**

```cpp
TEST_CASE("qp-v2: prompt version bumped and prompt content updated") {
    CHECK(std::string(kQueryPlannerPromptVersion) == "qp-v2");
    std::string p(kQueryPlannerSystemPrompt);
    // 新 few-shot 锚点：流程题 / 对比题 / 仪器题
    CHECK(p.find("环球法") != std::string::npos);
    CHECK(p.find("微型狄法尔") != std::string::npos);
    CHECK(p.find("粗集料筛分") != std::string::npos);
    // 安全阀规则句
    CHECK(p.find("回退用原句") != std::string::npos);
    // 旧行为教材已删：全空示例、同构 List 示例、"GeneralFact 留空"规则
    CHECK(p.find("路基沉降") == std::string::npos);
    CHECK(p.find("哪些试验用到马歇尔") == std::string::npos);
    CHECK(p.find("GeneralFact 留空串") == std::string::npos);
}

TEST_CASE("QueryPlanner maps GeneralFact plan fields into QueryAnalysis (qp-v2 三件套流通)") {
    auto p = make_test_planner([](const std::string&, const std::string&) {
        return std::string(R"({"intent":"GeneralFact","key_terms":["环球法","软化点"],
            "section_hints":[],"sparse_text":"环球法 软化点","dense_text":"环球法测定沥青软化点"})");
    });
    QueryAnalysis a = p.plan("环球法测沥青软化点怎样控制");
    CHECK(a.intent == QueryIntent::GeneralFact);
    REQUIRE(a.key_terms.size() == 2);
    CHECK(a.key_terms[0] == "环球法");
    CHECK(a.sparse_text == "环球法 软化点");
    CHECK(a.dense_text == "环球法测定沥青软化点");
    std::filesystem::remove_all(p.cache_dir);
}
```

注：第二个用例是**回归护栏**（映射逻辑今天已存在、应直接绿）；第一个用例在改 prompt 前必红。

- [ ] **Step 2: 构建 + 跑新用例确认第一个失败**

Run: MSBuild 后 `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="qp-v2*"`
Expected: FAIL（版本仍是 qp-v1、prompt 无新锚点）。

- [ ] **Step 3: 替换 `src/query/query_planner.cpp:11-28` 的两个常量**

```cpp
const char* kQueryPlannerPromptVersion = "qp-v2";

const char* kQueryPlannerSystemPrompt =
    "你是公路工程标准问答系统的查询分析器。给定一个中文问题，只输出一个严格的 JSON 对象，"
    "描述检索计划，不要任何解释或代码块围栏。字段：\n"
    "- intent: 四选一 \"GeneralFact\"|\"ListByCondition\"|\"ClauseLookup\"|\"MethodLookup\"。"
    "问\"哪些/有哪些…用到/需要某仪器或材料\"这类要列举多个试验的→ListByCondition；其余普通问答→GeneralFact。\n"
    "- key_terms: 所有题型都抽 1-4 个最能定位答案的判别词（试验方法名/仪器/材料/指标名），用问题原文词形；"
    "绝不放\"试验/规程/方法/公路/工程/水泥/混凝土/沥青/集料\"这类满库通用词；确无判别词才给空数组。\n"
    "- section_hints: 判别词通常所在章节词（仪器→[\"仪具\",\"材料\"]）；无则空数组。\n"
    "- sparse_text: 给关键词检索的查询=判别词+章节词，空格分隔，去掉\"哪些/的/了\"等虚词与满库背景词；拿不准就留空串。\n"
    "- dense_text: 给向量检索的聚焦重述，一句话，必须保留问题里全部关键实体与限定条件；拿不准就留空串。\n"
    "拿不准或确无判别词时：key_terms 给空数组、sparse_text 和 dense_text 留空串——留空系统会回退用原句检索，不会更差。\n"
    "示例：\n"
    "问：公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平\n"
    "答：{\"intent\":\"ListByCondition\",\"key_terms\":[\"天平\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"天平 仪具 材料\",\"dense_text\":\"使用天平的试验仪具与材料\"}\n"
    "问：环球法测沥青软化点时，试样制备、加热速度和终点判定怎样控制？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"环球法\",\"软化点\"],\"section_hints\":[],\"sparse_text\":\"环球法 软化点 加热速度 终点判定\",\"dense_text\":\"环球法测定沥青软化点的试样制备、加热速度与终点判定\"}\n"
    "问：微型狄法尔法和洛杉矶法评价集料磨耗性能时，试验作用方式与结果指标有什么不同？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"微型狄法尔\",\"洛杉矶\",\"磨耗\"],\"section_hints\":[],\"sparse_text\":\"微型狄法尔 洛杉矶 磨耗\",\"dense_text\":\"微型狄法尔法与洛杉矶法评价集料磨耗性能的作用方式与结果指标区别\"}\n"
    "问：粗集料筛分试验需要准备哪些主要仪具？\n"
    "答：{\"intent\":\"GeneralFact\",\"key_terms\":[\"筛分\",\"仪具\"],\"section_hints\":[\"仪具\",\"材料\"],\"sparse_text\":\"筛分 仪具 材料\",\"dense_text\":\"粗集料筛分试验的仪具与材料\"}";
```

- [ ] **Step 4: 构建 + 跑 planner 全组测试确认全绿**

Run: MSBuild 后 `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="qp-v2*,QueryPlanner*,parse_llm_plan*,parse_planner_mode*,normalize_question*"`
Expected: 全 PASS（含既有 planner 用例零回归——尤其"numbered question bypasses LLM"和"rule mode never calls LLM"）。

- [ ] **Step 5: 全量 doctest**

Run: `.\rag2.0.tests\x64\Debug\rag2.0.tests.exe`
Expected: 全绿（此前 229 cases 上加 2 个新用例）。

- [ ] **Step 6: 提交（pathspec）**

```bash
git add src/query/query_planner.cpp tests/test_query_planner.cpp
git commit -m "feat(planner): qp-v2 prompt——GeneralFact 三件套+靶向 few-shot+安全阀（版本 bump 失效旧缓存）" -- src/query/query_planner.cpp tests/test_query_planner.cpp
```

---

## Task 2: 单查冒烟（验证 GeneralFact 真的产出三件套）

**Files:** 无代码改动。需要 PG+Milvus 在线 + DeepSeek 余额；服务不在则跳过并注明。

- [ ] **Step 1: 起服务（若未起）**

```powershell
docker start milvus-etcd milvus-minio milvus-standalone
```

- [ ] **Step 2: 跑一道 few-shot 里没有的普通题**

```powershell
.\rag2.0\x64\Debug\rag2.0.exe retrievecheck "沥青针入度试验的试验温度、荷载、贯入时间和结果取值有哪些规定？"
```

- [ ] **Step 3: 检查日志行**

Expected: `[queryplan] intent=GeneralFact sparse="…" dense="…"` 中 sparse/dense **非空**（qp-v1 时恒为空串），且 sparse 不含"试验/规程"等满库词。召回结果正常返回、不崩。若 LLM 对该题走了安全阀（全空）也算通过——换 1-2 道其他普通题再看（如"环球法"以外的 risk 清单题）。注意**不要**用 few-shot 里的原题（LLM 会背答案，验证无效）。

---

## Task 3: 四模式 rich eval（llm planner）+ 验收判定

**Files:** 无代码改动；循环改 `config.json` 的 `RAG_RERANK_MODE`（程序不读环境变量），跑完恢复 `off`。

- [ ] **Step 1: 后台串行跑四模式（Bash，项目根目录；单模式 15-40 分钟，总计 ~1.5h）**

```bash
OUT="C:/Users/86199/AppData/Local/Temp/claude/D--vs2022-code-rag2-0/9c881a16-6d8d-465c-952c-a28e18ba4620/scratchpad"
for m in off light model hybrid; do
  python -c "import json,sys;p='config.json';d=json.load(open(p,encoding='utf-8'));d['RAG_RERANK_MODE']=sys.argv[1];json.dump(d,open(p,'w',encoding='utf-8'),ensure_ascii=False,indent=2)" "$m"
  echo "=== MODE=$m START $(date '+%H:%M:%S') ==="
  ./rag2.0/x64/Debug/rag2.0.exe eval eval/retrieval_questions_100.annotated.json 30 llm --rich > "$OUT/eval_qpv2_$m.txt" 2>&1
  echo "=== MODE=$m DONE exit=$? $(date '+%H:%M:%S') ==="
done
python -c "import json;p='config.json';d=json.load(open(p,encoding='utf-8'));d['RAG_RERANK_MODE']='off';json.dump(d,open(p,'w',encoding='utf-8'),ensure_ascii=False,indent=2)"
```

- [ ] **Step 2: 合法性检查（每模式）**

```bash
for m in off light model hybrid; do
  grep -aci "402\|payment\|insufficient" "$OUT/eval_qpv2_$m.txt"   # 期望 0（偶发 read timeout 回退 1-2 次可接受）
  grep -ac "queryplanner\] LLM" "$OUT/eval_qpv2_$m.txt"            # 期望 ~194（真走了 LLM）
done
grep -aci "模型调用失败" "$OUT/eval_qpv2_model.txt"                 # 期望 0（rerank 未回退）
```
另抽查 off 输出里 GeneralFact 的 `sparse="…"` 非空占比——若绝大多数仍为空说明 prompt 没生效（查缓存/版本号）。

- [ ] **Step 3: 抽指标填表（对照 header 的 qp-v1 baseline）**

```bash
for m in off light model hybrid; do
  grep -aE "Group Recall@20|Complete@20|nDCG@(1|20):|Redundancy@(10|20)|Distractor Hit@20|Distractor-before-gold" "$OUT/eval_qpv2_$m.txt"
done
```

| mode | GR@20 | C@20 | nDCG@1 | nDCG@20 | Red@10 | Red@20 | DH@20 | DbG |
|---|---|---|---|---|---|---|---|---|
| off | | | | | | | | |
| light | | | | | | | | |
| model | | | | | | | | |
| hybrid | | | | | | | | |

- [ ] **Step 4: 按 spec 判定（不许拍脑袋）**

- **硬门槛**：各模式 GR@20/C@20 不低于同模式 qp-v1 baseline（off 0.9967/0.99、light 1.0/1.0、model/hybrid 0.9967/0.99）。任一模式跌 → **回滚**（`git revert` Task 1 的 commit，qp-v1 缓存仍在磁盘、立即复原），并在报告里记录数字。
- **成功判据（满足其一即保留）**：off 的 Red@10 < 0.2506、或 DbG < 0.0588、或 nDCG@20 > 0.7939。
- 顺带记录：light 是否因 key_terms 激活而改善（qp-v1 下 light nDCG@20=0.789 略输 off）；model/hybrid 是否受益于聚焦 query。

---

## Task 4: README + 记忆 + push

**Files:**
- Modify: `README.md`（M5.2 行之后或其内补 qp-v2 结论）
- 记忆文件由主会话维护，不在此 plan 内。

- [ ] **Step 1: 按 Task 3 实测数字更新 README**

在 README M5.2 那条 blockquote 之后补一条 qp-v2 记录：改了什么（GeneralFact 三件套+few-shot+安全阀）、四模式实测数字、结论（保留/回滚、planner 默认不变 auto==llm）。**禁止编造数字**；若回滚则如实写"qp-v2 尝试并回滚（原因+数字）"。

- [ ] **Step 2: 提交 + push（pathspec）**

```bash
git add README.md
git commit -m "docs(planner): README qp-v2 四模式实测结论" -- README.md
git push origin V4.0
```

---

## Self-Review（写完自查，已过）

- **Spec 覆盖**：改动范围（两常量+版本 bump）→T1；规则翻转+安全阀+few-shot 4 例→T1 Step3（全文给出）；行为影响面（GeneralFact 三件套流通/列举不误入/编号短路）→T1 Step1 第二用例+既有用例回归；验收四模式+硬门槛+成功判据+决策规则→T3；回滚→T3 Step4（revert 即复原）；README→T4。
- **占位符**：无 TBD；T3 指标表为"实测填入"（数据依赖，禁编造）。
- **一致性**：测试断言的锚点字符串（环球法/微型狄法尔/粗集料筛分/回退用原句）与 Step3 prompt 全文逐一对应；被删内容（路基沉降/哪些试验用到马歇尔/GeneralFact 留空串）在新 prompt 中确不出现。
- **外部依赖**：T2/T3 需服务+余额，plan 内已写跳过/合法性检查路径。
