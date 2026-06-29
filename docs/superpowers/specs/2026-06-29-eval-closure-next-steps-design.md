# 评估闭环下一步：先把标注用起来

- 日期：2026-06-29
- 类型：下一步决策 / 评估闭环 spec
- 状态：草案，确认后再拆 implementation plan
- 依据：项目现状勘察、`logs/eval100_fixed.txt`、`eval/retrieval_questions_100.annotated.json`

---

## 1. 背景

README 现在有点跟不上代码了。它还写着 M1 进行中、M3/M4 只是计划，但仓库里已经有这些东西：

1. 文本路三路召回：dense、BM25、PG 精确匹配，再走 RRF；
2. 列举类问题的关键词直查补召回，以及无关键词候选下压；
3. `retrievecheck`、`eval --gen`、`eval --rich`；
4. 统一评估 schema，已经能读 `must_have_groups`、`acceptable_chunks`、`distractor_chunks` 和 `generation`；
5. 100 题自动标注文件：`eval/retrieval_questions_100.annotated.json`；
6. 两条测试线都是绿的：C++ doctest 199/199，Python 标注脚本 pytest 33/33。

最近一版评估也说明问题不在“骨架有没有”。`logs/eval100_fixed.txt` 里，点查 `hit@30 = 74/75`，`MRR = 0.875444`；`Group Recall@20 = 0.986667`，`Complete@20 = 0.98`。剩下的失败样本很少，已经适合逐条看，而不是再大改一轮召回架构。

所以，下一步应该从评估资产入手：把自动标注变成能审、能复跑、能被 C++ evaluator 消费的数据，然后再按指标去修剩下的问题。

---

## 2. 目标

这轮只做一件事：把“100 题自动标注”接进评估闭环。

具体要达到：

1. `retrieval_questions_100.annotated.json` 能被完整审阅，不只是一份机器产物；
2. `eval --rich` 真正消费 `distractor_chunks` 和 `acceptable_chunks`；
3. 报告里除了召回覆盖，还能看到排序风险，比如干扰项是否排到 gold 前面；
4. 用这些新指标决定下一步要调词典、planner、BM25、small-to-big，还是先改数据。

---

## 3. 这轮不做什么

先把边界钉住，免得这件事一路滑到 M5/M6/M7。

本轮不做：

1. 不接 reranker；
2. 不做视觉路、both 模式、跨模态 auto 路由；
3. 不扩 PG schema 到完整版本管理、密级、表格 cell；
4. 不重做 1000 题评测集；
5. 不先调召回权重或 prompt，除非新指标指向明确问题；
6. 不把自动标注冒充成人工审核结果。

这些事都值得做，只是顺序不在这里。先把评估尺子做准。

---

## 4. 现在卡在哪里

### 4.1 review 文件只导出了 2 题

`eval/retrieval_questions_100.annotated.json` 已经覆盖 `rq-001` 到 `rq-100`，但 `eval/retrieval_questions_100.review.md` 现在只有 2 个问题。

原因基本可以定位到 `scripts/mine_annotations.py` 的续跑逻辑：已有 `--out` 时，脚本会复用 `done` case。这些 case 会进 `out_cases`，但不会重新构造 `review entry`。结果就是，数据文件有 100 条，审阅文件只有本次新处理的 2 条。

这不是小问题。没有完整 review，就没法说这 100 条标注已经到了可消费状态。

### 4.2 C++ evaluator 还没吃到新标注

`EvalCase` 已经能解析：

- `acceptable_chunks`
- `distractor_chunks`
- `must_have_groups`

但 `run_rich_eval` 现在只算 Group Recall@k / Complete@k。它还没有算：

- nDCG@k
- Distractor@k
- Distractor Hit@k
- Distractor-before-gold
- Redundancy@k

也就是说，标注数据已经准备好了，指标还没接上。

### 4.3 README 需要跟着改

README 里还把 `ingest` / `query` 写成 M1 建设中，把 M3/M4 写成计划。这会误导后续开发，尤其是接手的人会以为现在还在 walking skeleton 阶段。

README 不用在本轮第一步改，但富指标接完后要同步。

---

## 5. 推荐顺序

### 第一步：修 full review

先改 `scripts/mine_annotations.py` 的 review 流程。

需要支持：

1. 已存在于 `--out` 的 case，也能重新构造 review entry；
2. `--review-only` 之类的只读模式，专门从 annotated 文件生成 review；
3. review 候选按当前候选逻辑重建；
4. 优先读 `data/annotation_cache` 里的 LLM 响应，避免重复调用；
5. 缓存缺失时不要编理由。可以标 `cache_missing`，或者要求显式传 `--allow-llm` 才补调 DeepSeek。

验收很直接：

1. `eval/retrieval_questions_100.review.md` 有 100 个问题标题；
2. 抽检 10 条，看 gold 是否被排除、distractor 是否真是“像但错”、acceptable 是否只是相关补充；
3. 抽检通过后，可以把对应 case 标为 `reviewed_sample` 或类似状态。不要写成全量 reviewed，除非真的逐条看完。

### 第二步：接富排序指标

在现有 `--rich` 后面追加一段排序风险指标。首版先做这些：

1. `Distractor@k`：top-k 里命中的 distractor 数 / 该样本已标注 distractor 数；
2. `Distractor Hit@k`：top-k 是否出现任一 distractor；
3. `Distractor-before-gold`：首个 distractor 是否排在首个必要证据之前；
4. `Redundancy@k`：top-k 里重复命中同一证据组或同一事实的比例；
5. `nDCG@k`：必要证据组首命中给 2 分，acceptable chunk 给 1 分，重复不给分。

`k` 仍用 `{1,3,5,10,20}`，和现有 Group Recall/Complete 对齐。

实现上要克制：

1. 不改 `text_retrieve`；
2. 不改 base `run_eval`；
3. 复用 `run_rich_eval` 的独立 top-20 召回；
4. 指标函数先写成纯函数，用 doctest 覆盖；
5. 只在 `--rich` 打印新段。

### 第三步：拿新指标看剩余失败

不要一上来泛化调参。先看 `eval100_fixed.txt` 里已经暴露的两个问题：

1. “通用硅酸盐水泥安定性需要通过哪两种方法判定合格？”
2. “超薄罩面细集料优先选什么材料，纤维可选哪些种类并应满足哪些基本要求？”

每题按同一套流程查：

1. 跑 `retrievecheck "<问题>" 30`；
2. 看 gold 有没有进召回池；
3. 看 distractor 有没有排在 gold 前；
4. 看 acceptable 是否占了太多靠前位置；
5. 再决定问题在数据、词典、query planner、BM25 还是 small-to-big。

---

## 6. 指标口径

### 6.1 证据 key

沿用 `run_rich_eval` 现在的 key：

- `cid:<chunk_id>`
- `m:<method_no>`
- `c:<standard_id>|<clause_no>`

必要证据组从 `must_have_groups` 来。`acceptable_chunks` 和 `distractor_chunks` 首版只按 `chunk_id` 精确匹配，不先做 stable ref 展开。

### 6.2 nDCG gain

首版用简单口径：

| 候选类型 | gain |
|---|---:|
| 首次覆盖某个必要证据组 | 2 |
| 命中 acceptable chunk | 1 |
| 重复命中已覆盖必要组 | 0 |
| distractor / irrelevant | 0 |

几点说明：

- 同一必要组命中多次，只第一次加分；
- acceptable chunk 只按 `chunk_id` 记一次；
- distractor 不给负分。干扰项风险单独看 Distractor 指标。

### 6.3 Distractor 分母

只有 `distractor_chunks` 非空的样本进入 Distractor@k 分母。

没有标注 distractor 的样本不能算 0 风险。那只是没标，不是没风险。

### 6.4 Redundancy

首版先保守一点：

```text
Redundancy@k = top-k 中重复证据数 / top-k 中有效证据数
```

重复证据包括：

1. 同一 `chunk_id` 重复；
2. 同一必要证据组被多个候选重复命中；
3. 同一 `method_no` 重复命中，且没有带来新的必要组。

---

## 7. 报告怎么长

原来的汇总继续保留：

```text
Group Recall@1/3/5/10/20
Complete@1/3/5/10/20
```

后面加一段：

```text
--- 富排序指标 (--rich) ---
nDCG@1/3/5/10/20: ...
Distractor Hit@1/3/5/10/20: ...
Distractor@1/3/5/10/20: ...
Distractor-before-gold: ...
Redundancy@1/3/5/10/20: ...
```

逐题输出不要刷满 100 条。只打异常就够：

```text
[risk] distractor-before-gold  rq-xxx  <question>
[risk] redundancy@10=0.40      rq-yyy  <question>
[miss] complete@20=0           rq-zzz  <question>
```

---

## 8. 测试

### 8.1 Python 标注脚本

补 pytest：

1. 已有 out 文件时，`--review` 也能覆盖 done case；
2. `--review-only` 不改写 annotated json；
3. 缓存命中时不调 LLM；
4. 缓存缺失时，review 能标出 `cache_missing`。

### 8.2 C++ 纯指标

补 doctest：

1. nDCG：必要组首命中、acceptable、重复不加分；
2. Distractor@k：空 distractor 不进分母；
3. Distractor-before-gold：gold 前、gold 后、无 gold；
4. Redundancy@k：同组重复、同方法重复、无重复。

### 8.3 集成验收

跑：

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
python -m pytest scripts/test_mine_annotations.py -v
.\rag2.0\x64\Debug\rag2.0.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich
```

验收看这几项：

1. C++ doctest 全绿；
2. Python pytest 全绿；
3. `--rich` 仍输出原来的 Group Recall/Complete；
4. 新增 nDCG / Distractor / Redundancy 汇总；
5. base 指标不回归。

---

## 9. README 同步

富指标接完后更新 README。

要改的点：

1. 路线图状态改成真实状态；
2. 用法补上这些命令：

```powershell
rag2.exe retrievecheck "问题" 20
rag2.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich
rag2.exe eval eval/dataset_seed.json 30 rule --gen
```

3. 明确当前文本路 MVP 已经成型，后续改进由富指标和失败样本驱动。

---

## 10. 风险

| 风险 | 影响 | 处理 |
|---|---|---|
| 自动标注误判 | Distractor/nDCG 失真 | 先修 full review，再抽检 |
| acceptable 标太宽 | nDCG 虚高 | 首版 gain 只给 1，并单独看 Group Recall |
| distractor 标太少 | 风险被低估 | 空 distractor 样本不进分母，报告打印有效样本数 |
| review 重建又调 LLM | 成本高，还可能漂移 | 默认只读缓存，显式参数才允许补调 |
| README 继续滞后 | 后续接手者误判阶段 | 指标二期完成后同步 |

---

## 11. 下一步拆法

确认这个 spec 后，implementation plan 建议拆成四个任务：

1. 修 `mine_annotations.py` 的 full review / review-only；
2. 写 C++ 富排序指标纯函数；
3. 接入 `run_rich_eval` 和报告段；
4. 更新 README，并用 100 题 annotated 集跑 baseline。

---

## 12. Self-review

1. 没有 TODO/TBD，也没有“以后再说”的占位任务。
2. 范围只覆盖标注复核和富指标消费，没有混进 reranker、视觉路或 PG schema 大改。
3. 指标口径已经写死：nDCG gain、Distractor 分母、Redundancy 首版定义都明确。
4. 这份 spec 可以直接拆一个 implementation plan；更大的 M5/M6 能力另开文档。
