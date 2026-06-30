# M5 分阶段 Reranker 设计

- 日期：2026-06-30
- 类型：文本路增强设计
- 状态：设计确认，等待 implementation plan
- 关联阶段：M5.1 Reranker 框架与轻量规则重排；M5.2 大模型 Reranker 接入
- 关联代码：`src/retrieve/text_search.cpp`、`src/retrieve/rrf.*`、`src/retrieve/candidate.h`、`src/query/query_analysis.*`、`src/query/query_planner.*`、`src/db/pg_client.*`

## 1. 背景

当前文本检索链路已经具备：

1. `QueryPlanner` 生成 `QueryAnalysis`，包括 intent、标准号、条款号、方法号、`key_terms`、`sparse_text` 和 `dense_text`；
2. dense、BM25、PG exact 三路召回；
3. RRF 融合；
4. 列举题的 `key_terms` PG 直查补召回，以及无关键词候选下压；
5. 文档侧双文本路线：dense 使用 `embedding_text`，BM25 使用 `bm25_text`。

双文本后，当前主要矛盾已经从“正确 chunk 进不了候选池”转向“候选池里谁应该排得更靠前”。README 中记录的基线大致是：

- `Group Recall@20` 约 `0.997`；
- `Complete@20` 约 `0.99`；
- `nDCG@20` 约 `0.80`；
- `Redundancy@20` 约 `0.25`；
- `Distractor-before-gold` 约 `0.07`。

这说明 top20 召回已经接近天花板，但排序、重复块和干扰项仍有改进空间。

旧 M5 设计把文本 reranker、表格 cell 定位、引用图扩展、LLM 元数据和 token 预算放在同一个阶段。这个范围过大，不利于定位收益来源。新的 M5 先拆出 reranker 主线：

```text
M5.1：Reranker 框架 + 轻量规则 reranker
M5.2：大模型 reranker 接入同一接口
```

表格 cell 定位、引用图扩展、LLM 离线元数据和 token 预算继续保留为 M5 后续子阶段，不放进本 spec 的实现范围。

## 2. 目标

M5.1 要先把 reranker 这条链路搭稳：

1. 在 RRF 之后增加一个可配置的 reranker 阶段；
2. 定义稳定的 reranker 输入、输出和回退行为；
3. 实现 `LightReranker`，用可解释规则提升排序并降低重复；
4. 让普通问题里的 `key_terms` 以软信号参与排序；
5. 保留条款号、方法号、标准号等硬信号的保护逻辑；
6. 为 M5.2 的大模型 reranker 留出同一个接口；
7. 用现有 rich eval 指标验证收益。

M5.2 再接入模型服务：

1. 实现 `ModelReranker`，复用 M5.1 的候选构造和输出接口；
2. 支持超时、失败和关闭时回退到 `LightReranker`；
3. 与 `LightReranker` 做 A/B，对比是否真实提升排序质量。

## 3. 不做什么

M5.1 不做：

1. 不接入真实大模型 reranker 服务；
2. 不新增 Milvus 或 PG schema；
3. 不改变 dense、BM25、PG exact 的召回逻辑；
4. 不改变文档侧 `embedding_text` / `bm25_text` 生成逻辑；
5. 不做表格 cell 级定位；
6. 不做引用图扩展；
7. 不做 LLM 离线元数据生成；
8. 不改 prompt token 预算策略。

M5.2 只负责接入大模型 reranker，不把表格、引用图、元数据一起塞进来。

## 4. 总体架构

现有链路：

```text
QueryPlanner
  -> dense / BM25 / PG exact
  -> RRF
  -> method_no pin
  -> clause_no pin
  -> top_k
```

M5.1 后：

```text
QueryPlanner
  -> dense / BM25 / PG exact
  -> RRF candidate pool
  -> RerankCandidateBuilder
  -> LightReranker
  -> RerankPolicy
  -> method_no / clause_no hard pin
  -> top_k
```

M5.2 后：

```text
QueryPlanner
  -> dense / BM25 / PG exact
  -> RRF candidate pool
  -> RerankCandidateBuilder
  -> LightReranker
  -> ModelReranker
  -> RerankPolicy
  -> method_no / clause_no hard pin
  -> top_k
```

`method_no` 和 `clause_no` 继续保持硬置顶。模型和轻量规则都不能把明确编号命中的 chunk 压下去。

## 5. 核心组件

### 5.1 RerankCandidate

`Candidate` 当前只有 `standard_id`、`chunk_id`、`score` 和 `source`，不足以做内容重排。M5.1 新增内部结构 `RerankCandidate`，由 PG 回查补齐字段：

```cpp
struct RerankCandidate {
    Candidate base;
    std::string title;
    std::string path_text;
    std::string atomic_text;
    std::string context_text;
    std::string bm25_text;
    std::string clause_no;
    std::string method_no;
    int page_start = 0;
    int page_end = 0;
};
```

这是 reranker 的输入，不替代数据库里的 `RetrievalChunkRow`。

### 5.2 RerankCandidateBuilder

`RerankCandidateBuilder` 负责把 RRF 输出的 `Candidate` 转成 `RerankCandidate`：

1. 按 `chunk_id` 从 PG 回查 `retrieval_chunks`；
2. 查不到 chunk 时跳过，并打日志；
3. 保留原始 `Candidate.score` 和 `Candidate.source`；
4. 不修改 PG 和 Milvus。

它让排序器只关心排序，不关心数据从哪里来。

### 5.3 IReranker

M5.1 引入一个小接口，供轻量规则和大模型共用：

```cpp
class IReranker {
public:
    virtual ~IReranker() = default;
    virtual std::vector<Candidate> rerank(
        const QueryAnalysis& query,
        const std::vector<RerankCandidate>& candidates,
        int top_k) = 0;
};
```

接口约束：

1. 输入顺序是 RRF 顺序；
2. 输出仍是 `Candidate`，方便接回现有 `text_retrieve`；
3. reranker 不负责重新召回；
4. reranker 不负责方法号、条款号最终 pin；
5. reranker 失败时调用方必须能回退到输入顺序。

### 5.4 LightReranker

`LightReranker` 是 M5.1 默认实现，使用确定性规则打分。它的目标不是取代大模型，而是先把“排序框架”和“可解释信号”跑通。

加分信号：

1. `key_terms` 出现在 `title`、`path_text`、`atomic_text`、`context_text` 或 `bm25_text`；
2. 问题中的 `method_no` 与候选 `method_no` 匹配；
3. 问题中的 `clause_no` 与候选 `clause_no` 匹配；
4. 候选同时来自 dense 和 BM25；
5. 候选来自 PG exact；
6. 标题或路径含有问题中的核心词。

降权信号：

1. 同一 `standard_id + clause_no` 下已经出现多个候选时，后续重复块轻微降权；
2. 只有泛词命中而没有核心词命中时，不给强加分；
3. `source` 单一路径且原始 RRF 排名靠后时，不因为单个弱词命中大幅上浮。

普通问题的 `key_terms` 只做软加分。列举题仍保留现有“关键词直查补召回 + 无关键词候选下压”逻辑。

### 5.5 RerankPolicy

`RerankPolicy` 负责排序后的保护和截断规则：

1. 保护 `method_no` 精确命中；
2. 保护 `clause_no` 精确命中；
3. 控制同一条款的重复候选数量；
4. 截断到最终 `top_k`；
5. 保持排序稳定：同分时保留 RRF 原始顺序。

M5.1 可以先把 `method_no` 和 `clause_no` pin 继续留在 `text_search.cpp` 现有位置，避免一次性改动过大；但设计上它们属于 policy 层，M5.2 前可以再收敛边界。

## 6. 配置

新增 rerank 模式：

```text
rerank.mode = off | light | model | hybrid
```

含义：

| 模式 | 行为 |
|---|---|
| `off` | 保持当前 RRF 行为 |
| `light` | 启用 `LightReranker`，M5.1 默认 |
| `model` | 启用 `ModelReranker`，失败回退 RRF 或 light |
| `hybrid` | 先 light 降噪，再 model 重排，M5.2 之后评估是否推荐 |

M5.1 只需要实现 `off` 和 `light`。`model`、`hybrid` 可以先只在配置文档中保留，不要求可运行。

建议默认值：

```text
rerank.mode = light
rerank.candidate_pool_k = top_k * 4
rerank.max_per_clause = 2
```

`candidate_pool_k` 控制 RRF 输出给 reranker 的候选池大小。现在非列举题会直接 RRF 截断到 `top_k`，M5.1 需要把池子放大，否则 reranker 没有调整空间。

## 7. 数据流

M5.1 的具体数据流：

```text
1. QueryPlanner 生成 QueryAnalysis
2. dense 使用 dense_text 或 clean_text
3. BM25 使用 sparse_text 或 clean_text
4. PG exact 按 method_no 补召回
5. ListByCondition 继续按 key_terms 做 PG 直查补召回
6. RRF 融合到 candidate_pool_k
7. RerankCandidateBuilder 回查 PG，补齐候选文本字段
8. LightReranker 计算 rerank_score
9. RerankPolicy 做重复控制和稳定截断
10. method_no / clause_no 精确命中最终置顶
11. 返回 top_k
```

重要边界：

1. Reranker 不改变召回集合之外的数据；
2. Reranker 不创建新 chunk；
3. Reranker 不写库；
4. Reranker 的输出必须仍可被 `answer_pipeline` 用 `chunk_id` 回查 PG。

## 8. key_terms 规则

M5.1 明确区分两类使用方式。

列举题：

```text
QueryIntent::ListByCondition
  -> key_terms 参与 PG 直查补召回
  -> 未命中 key_terms 的候选下压
```

普通问题：

```text
QueryIntent::GeneralFact / ClauseLookup / MethodLookup
  -> key_terms 不做过滤
  -> key_terms 命中候选文本时加分
```

这样可以避免“用户问题很模糊，关键词没命中就误删正确答案”的风险，同时让普通问题也能从关键词中受益。

## 9. 大模型 Reranker 的 M5.2 接入方式

M5.2 新增 `ModelReranker`，实现同一个 `IReranker` 接口。

输入：

```text
query.clean_text
candidate.title
candidate.path_text
candidate.context_text 或 atomic_text
candidate.bm25_text 的必要摘要
candidate.source
```

输出：

```text
chunk_id -> model_score
```

调用策略：

1. 默认只对 RRF top20 或配置的 `candidate_pool_k` 调用；
2. 超时直接回退；
3. 返回缺失 chunk 时保留这些 chunk 的 light/RRF 顺序；
4. 模型分数只改变软排序，不覆盖方法号、条款号硬保护；
5. 日志记录模型前后排名，便于 case review。

M5.2 不默认承诺模型一定优于 light。是否启用 `model` 或 `hybrid`，必须由 rich eval 决定。

## 10. 错误处理与回退

M5.1：

1. PG 回查某个 chunk 失败：跳过该 chunk，并记录 warn；
2. 所有候选都回查失败：回退 RRF 原始候选；
3. `rerank.mode=off`：完全走旧链路；
4. 规则打分异常：回退 RRF 原始顺序。

M5.2：

1. 模型服务不可用：回退 `LightReranker`；
2. 模型超时：回退 `LightReranker`；
3. 模型返回非法 JSON 或缺失分数：回退 `LightReranker`；
4. 模型排序导致 hard pin 后移：由 `RerankPolicy` 纠正。

## 11. 测试设计

### 11.1 单元测试

新增测试覆盖：

1. `RerankCandidateBuilder` 能把 `Candidate` 补成 `RerankCandidate`；
2. `LightReranker` 对命中 `key_terms` 的候选加分；
3. 普通问题的 `key_terms` 不会过滤候选；
4. 列举题仍保留现有关键词下压行为；
5. 同一条款重复候选会被稳定降权；
6. 同分时保留 RRF 原始顺序；
7. `rerank.mode=off` 行为与旧链路一致。

### 11.2 集成测试

在现有 retrieval tests 基础上增加：

1. RRF topN 中 gold 存在但排序靠后时，LightReranker 能把它上提；
2. 明确方法号问题中，method pin 仍然最终置顶；
3. 明确条款号问题中，clause pin 仍然最终置顶；
4. 重复 chunk 不会占满 top_k。

### 11.3 评估验证

使用现有 `eval --rich` 对比：

```text
baseline: rerank.mode=off
M5.1:    rerank.mode=light
M5.2:    rerank.mode=model / hybrid
```

关注指标：

1. `Group Recall@20` 不明显下降；
2. `Complete@20` 不明显下降；
3. `nDCG@20` 提升或至少不退步；
4. `Redundancy@20` 下降；
5. `Distractor-before-gold` 不升高；
6. 重点 case `rq-081` 仍保持前排命中。

## 12. 验收标准

M5.1 完成条件：

1. 存在可配置 rerank 阶段，至少支持 `off` 和 `light`；
2. `LightReranker` 能使用 `key_terms`、标题、路径、正文、`bm25_text` 和 source 做可解释打分；
3. 普通问题的 `key_terms` 作为软信号生效；
4. 列举题的现有关键词补召回和下压行为不退化；
5. 方法号、条款号精确命中仍最终置顶；
6. 单元测试和现有测试通过；
7. rich eval 有 `off` 和 `light` 对比日志；
8. 关键指标不出现明显回退。

M5.2 完成条件：

1. `ModelReranker` 实现同一 `IReranker` 接口；
2. 模型服务失败、超时、返回异常时可以回退；
3. 可以通过配置切换 `off`、`light`、`model`、`hybrid`；
4. rich eval 证明 `model` 或 `hybrid` 相对 `light` 有净收益，才建议默认启用。

## 13. 风险与对策

| 风险 | 对策 |
|---|---|
| 规则加分过猛，把噪声推到前面 | 所有加分保持小步，hard pin 只给明确编号 |
| 普通问题 key_terms 误伤正确答案 | 普通问题只软加分，不过滤 |
| 重复控制误压连续条款证据 | M5.1 默认按同一 `standard_id + clause_no` 控制，不跨条款压制 |
| candidate_pool_k 放大导致 PG 回查变慢 | 默认 `top_k * 4`，评估后再调整 |
| 大模型 reranker 不稳定 | M5.2 必须可回退，默认由 eval 决定是否启用 |
| 框架一次改太多 | M5.1 先保留现有 method/clause pin 位置，后续再收敛到 policy |

## 14. 给小白看的解释

可以把现在的检索想成三个人先各自找资料：

```text
dense：看意思像不像
BM25：看关键词有没有
PG exact：看方法号、条款号这种硬编号
```

RRF 做的是把三个人找来的资料合并。但 RRF 只知道“这个资料在某一路排第几”，它不太会细看资料内容。

M5.1 要加的是一个“二次整理员”：

```text
这个 chunk 标题更像问题，加一点分
这个 chunk 路径里有核心词，加一点分
这个 chunk 和前一个是同一条款的重复内容，稍微往后放
这个 chunk 是用户明确问的方法号，必须保护
```

M5.2 再把这个整理员升级成“会读语义的大模型整理员”。但即使上了大模型，硬编号和重复控制也不能完全交给模型猜，程序还是要守住底线。

所以这条路线不是绕远路，而是先把排序管道修好，再把大模型接进来。
