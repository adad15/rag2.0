# qp-v2：LLM 查询规划 prompt 升级（GeneralFact 三件套）设计

## 背景与根因

qp-v1 prompt（`src/query/query_planner.cpp` 的 `kQueryPlannerSystemPrompt`）明确要求 GeneralFact 题 `key_terms` 可为空、`sparse_text`/`dense_text` 留空串。空值在 `text_retrieve` 里回退原句——于是占评测集大头的普通题至今仍把**啰嗦整句**原样喂给 dense 与 BM25 两路。这正是 2026-06-16 确诊的"啰嗦查询两路同病"病根：当时的查询计划只治了列举题，GeneralFact 为零回归而零处理。

两个实测证据（2026-07-01 llm planner 四模式 rich eval）：

- eval 日志中 GeneralFact 全部 `sparse="" dense=""`；
- `[risk]` 清单集中在两类 GeneralFact：**对比题**（"A 法与 B 法有什么区别"）与**仪器题**（"某试验需要哪些仪具"），Redundancy@10 普遍 0.4-0.8。

连带问题：M5.1 light 重排的关键词加分依赖 `qa.key_terms` 非空。qp-v1 把 GeneralFact 的 key_terms 清零，导致 light 对普通题恒 inert——换 llm planner 也救不了，根子在 prompt。

## 目标

让 LLM 对**所有意图**产出三件套（key_terms / sparse_text / dense_text），针对失败题型加靶向 few-shot，带"留空=回退原句"安全阀。预期：普通题查询降噪 → Redundancy/排序风险下降；light 的关键词加分首次对普通题生效。

## 改动范围

**只改 `src/query/query_planner.cpp` 两处，下游零改动：**

1. `kQueryPlannerSystemPrompt` 字符串（新规则 + 新 few-shot，见下）；
2. `kQueryPlannerPromptVersion`：`"qp-v1"` → `"qp-v2"`（plan 缓存键含版本号，bump 后旧缓存自动失效、强制按新 prompt 重问）。

不改 `parse_llm_plan`（输出契约不变）、不改 `text_retrieve`（空值回退逻辑已存在）、不加新 intent、不动 section_hints 消费逻辑。

## qp-v2 prompt 规则

与 qp-v1 的差异：

| 字段 | qp-v1 | qp-v2 |
|---|---|---|
| intent | 四选一，列举→List，其余→General | **不变** |
| key_terms | 仅列举题必填，GeneralFact 可为空 | **所有题型都抽** 1-4 个判别词（试验方法名/仪器/材料/指标名，用原文词形）；**明确禁止**满库通用词（试验/规程/方法/公路/工程/水泥/混凝土/沥青/集料等）；确无判别词才空数组 |
| section_hints | 仪器→[仪具,材料] | **不变** |
| sparse_text | GeneralFact 留空串 | 判别词+章节词，空格分隔，去虚词与满库词；**拿不准留空串（留空即回退原句，安全）** |
| dense_text | GeneralFact 留空串 | 聚焦重述一句话，**必须保留问题里全部关键实体与限定条件**；拿不准留空串 |

安全阀以规则句表述（不占 few-shot 名额）："拿不准或确无判别词时：key_terms 给空数组、sparse_text 和 dense_text 留空串——留空系统会回退用原句检索，不会更差。"

## few-shot（3 例 → 4 例）

1. **列举题（保留，锚定 List 判据）**：天平题，输出同 qp-v1。
2. **普通流程题（新增，General 重写样板）**：
   问：环球法测沥青软化点时，试样制备、加热速度和终点判定怎样控制？
   答：`{"intent":"GeneralFact","key_terms":["环球法","软化点"],"section_hints":[],"sparse_text":"环球法 软化点 加热速度 终点判定","dense_text":"环球法测定沥青软化点的试样制备、加热速度与终点判定"}`
3. **对比题（新增，靶向 risk 清单主力题型）**：
   问：微型狄法尔法和洛杉矶法评价集料磨耗性能时，试验作用方式与结果指标有什么不同？
   答：`{"intent":"GeneralFact","key_terms":["微型狄法尔","洛杉矶","磨耗"],"section_hints":[],"sparse_text":"微型狄法尔 洛杉矶 磨耗","dense_text":"微型狄法尔法与洛杉矶法评价集料磨耗性能的作用方式与结果指标区别"}`
4. **仪器题（新增，教会区分"单试验要哪些仪器"=General 与"哪些试验用某仪器"=List，两者都带"哪些"易混）**：
   问：粗集料筛分试验需要准备哪些主要仪具？
   答：`{"intent":"GeneralFact","key_terms":["筛分","仪具"],"section_hints":["仪具","材料"],"sparse_text":"筛分 仪具 材料","dense_text":"粗集料筛分试验的仪具与材料"}`

qp-v1 的另外两例删除：**"哪些试验用到马歇尔"**（与天平例同构的 List 样板，留一个锚定即可，省 prompt 长度）；**"路基沉降怎么评定"全空示例**（与新规则矛盾——qp-v2 下它应抽出"路基沉降"；安全阀已由规则句覆盖）。净变化：3 例（天平/马歇尔/路基沉降）→ 4 例（天平/环球法/对比/仪器）。

## 行为影响面

- GeneralFact 的 dense/sparse 非空 → 两路检索吃聚焦文本（空则回退原句，与现状等价）；
- GeneralFact 的 key_terms 非空 → light 重排关键词加分对普通题首次生效；`list_recall` 判据含 `intent==ListByCondition`，普通题不会误入列举补全/下压链路；
- 编号题（条款号/方法号）在 `QueryPlanner::plan` 入口短路走规则路，完全不受影响；
- rule planner 路径不调 LLM，不受影响；
- rerank 打分缓存键含 query 文本，dense_text 变化 → model/hybrid 轮缓存 miss、按新查询重打（预期内成本）。

## 验收（四模式，llm planner）

实现后依次跑（切 `config.json` 的 `RAG_RERANK_MODE`，程序不读环境变量）：

```powershell
rag2.exe eval eval/retrieval_questions_100.annotated.json 30 llm --rich   # off/light/model/hybrid 各一轮
```

成本预估：首轮重打 ~100 次 DeepSeek plan（qp-v2 缓存键变；后三轮命中）；model 轮重打一轮 SiliconFlow rerank（hybrid 复用其缓存）。

**硬门槛（逐模式对比 2026-07-01 qp-v1 llm baseline）**：Group Recall@20 / Complete@20 不下降（off 0.9967/0.99；light 1.0/1.0；model/hybrid 0.9967/0.99）。

**成功判据（满足其一即值得保留，需无召回回退）**：off 模式 Redundancy@10 较 0.2506 下降；或 Distractor-before-gold 较 0.0588 下降；或 nDCG@20 较 0.7939 上升。同时观察 light 是否因 key_terms 激活而改善（qp-v1 下 light 的 nDCG@20=0.789 略低于 off）。

**决策规则**：qp-v2 优于或持平 qp-v1 且无召回损失 → 保留并更新 README baseline；任何模式召回下降 → 回滚。

## 回滚

revert 该 commit 即可（prompt + 版本号一并退回 qp-v1）。qp-v1 的 plan 缓存仍在磁盘（`data/query_plan_cache/`，键含版本号互不覆盖），回滚后立即恢复原行为，零重建成本。

## 非目标

不加新 intent、不做多路查询扩展（multi-query）、不动 section_hints 的消费逻辑、不改 rule planner、不动 reranker 层、不换 planner 默认值（auto==llm 已是默认）。
