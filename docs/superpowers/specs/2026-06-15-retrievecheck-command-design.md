# retrievecheck 检索可观测性命令设计

- 日期：2026-06-15
- 类型：小功能 spec（检索调试命令，独立于 M3/M4）
- 来源：2026-06-15 讨论（"哪些试验用到天平"召回不足的诊断需求）
- 分支：V3.1（或后续新分支）
- 状态：设计已确认，待写实现计划
- 背景：`query` 命令把 RRF 融合后的 top_k=5 个 chunk 喂给 LLM，但只看最终回答无法判断"召回够不够、是哪一路找到的"。为支撑检索调参（top_k、列举意图、词典扩充），需要一个能直接打印召回 chunk 的命令。

---

## 1. 一句话目标

新增 CLI 命令 `rag2 retrievecheck "问题" [k]`：跑与 `query` 相同的三路召回 + RRF（窗口默认放大到 20），把融合后的 top-k 个 chunk 连同来源、分数、定位、文本片段打印成排名表。**不调用 LLM**，`query` 行为完全不变。

---

## 2. 核心设计决策（已与用户确认）

### 2.1 形态：新 CLI 命令，与 query 并列

`rag2 retrievecheck "问题" [k]`，`k` 可选默认 20。和 `treecheck`/`chunkcheck`/`query` 同风格。不做 HTTP API、不做前端、不改 query。

### 2.2 复用 text_retrieve，不写新检索逻辑

直接调用 `query` 用的同一个 `text_retrieve(question, mv, embed, pg, syn, collection, per_path_k, top_k)`，只是 `top_k=k`（默认 20）、`per_path_k=k*4`（与 `answer_query` 同比例）。返回的 `Candidate{chunk_id, score, source}` 已带 RRF 分数和合并后的来源标签（`dense` / `bm25` / `dense+bm25` / `exact` / `exact_pin` 等），正是展示所需。

### 2.3 融合后单一排名表 + 来源标签

一份 RRF 融合后的 top-k 排名表，每行标 source。不按路分块。`source` 列是诊断核心：直接看出每条是哪一路召回的。

### 2.4 文本片段 UTF-8 安全截断

每行下方展示正文前 ~120 字符片段。中文按字符截断，不能从多字节中间切断（否则乱码）。抽纯函数 `utf8_truncate` 处理，可单测。

---

## 3. 输出格式

```text
retrievecheck "<问题>" | 召回 N 条 (k=20)

#   rrf      source       clause/method      标题
1   0.0320   dense+bm25   T0517-2020 / 2     仪具与材料
    片段：2 仪具与材料 水泥净浆低速搅拌机…电子天平 最大量程不小于1000g…
2   0.0285   bm25         T0502-2005 / 2     仪具与材料
    片段：2 仪具与材料 天平：最大量程不小于2000g，感量不大于1g…
3   0.0210   dense        - / 5.1            水泥物理、化学性能试验
    片段：…
...（共 N 行，N = min(k, 实际召回数)）
```

字段来源：

| 列 | 来源 |
|---|---|
| `#` | 排名序号（1..N） |
| `rrf` | `Candidate.score`（RRF 分数，保留 4 位小数） |
| `source` | `Candidate.source`（融合来源标签） |
| `clause/method` | `RetrievalChunkRow.method_no` / `clause_no`（无方法号显示 `-`） |
| `标题` | `RetrievalChunkRow.title` |
| 片段 | `RetrievalChunkRow.atomic_text` 经 `utf8_truncate(120)`，换行替换为空格 |

`standard_no` 不单列（两本库时 method_no/clause_no 已足够定位；避免长名撑乱表格）。用 `std::cout` 打印（与 `query` 输出一致，便于对齐控制）。

---

## 4. 组件

### 4.1 `src/util/text_utf8.h`（新增，纯头文件 inline，仿 path_utf8.h）

```cpp
#pragma once
#include <string>

namespace text_utf8 {

// 按 UTF-8 字符数安全截断（不从多字节字符中间切断），超长追加 "…"。
// max_chars 指可见字符数（一个汉字算 1）。纯函数，可单测。
std::string truncate(const std::string& s, size_t max_chars);

}  // namespace text_utf8
```

实现要点：遍历字节，遇到 UTF-8 首字节（非 `0x80~0xBF` 续字节）才计 1 个字符；到达 `max_chars` 即停在字符边界；若发生截断追加 `"…"`（U+2026）。换行/制表符的压平（替换为空格）在命令侧做，不放进 truncate。

### 4.2 `src/main.cpp` 新增 `cmd_retrievecheck`

签名与流程：

```cpp
static int cmd_retrievecheck(const Config& cfg, const std::string& question, int k);
```

1. 构造 `PgClient` / `MilvusRest` / `CloudEmbedding`；加载 `SynonymDict`（`config/synonyms.txt`），与 `cmd_query` 同款。**不构造 DeepSeekClient。**
2. `auto cands = text_retrieve(question, mv, embed, pg, syn, cfg.milvus_collection, k*4, k);`
3. 打印表头 `retrievecheck "<问题>" | 召回 <cands.size()> 条 (k=<k>)`。
4. 逐 `Candidate`：`pg.get_chunk(c.chunk_id)`（取不到则该行标注 `[缺失]` 跳过正文）；打印 `#/score/source/method_no|clause_no/title`，再缩进打印 `片段：` + `utf8_truncate(压平换行(atomic_text), 120)`。
5. 异常 try/catch、`missing_required()` 校验，与既有命令一致。返回 0；无召回也返回 0（打印"召回 0 条"）。

### 4.3 命令派发与 usage（`main.cpp` `main`）

usage 行追加 `retrievecheck`；新增派发块：

```cpp
if (cmd == "retrievecheck") {
    if (argc < 3) { std::cout << "usage: rag2 retrievecheck \"问题\" [k]\n"; return 1; }
    auto missing = cfg.missing_required();
    if (!missing.empty()) { for (auto& m : missing) spdlog::error("config.json 缺少必填项: {}", m); return 1; }
    int k = (argc >= 4) ? std::max(1, std::atoi(argv[3])) : 20;
    return cmd_retrievecheck(cfg, argv[2], k);
}
```

---

## 5. 测试

- **`tests/test_text_utf8.cpp`（doctest）**：纯英文不截断；恰好 max_chars 不加省略号；超长在字符边界截断且加 `"…"`；含中文不切断多字节（截断后能正常 UTF-8 解析、长度符合预期）；空串返回空串。
- 命令本身依赖 PG/Milvus/embedding，沿用项目惯例不单测；由真库手动验收覆盖。

端到端验收（真库）：

```text
rag2 retrievecheck "公路工程水泥及水泥混凝土试验规程中哪些混凝土试验用到了天平" 20
```

预期：打印 20 行，多行 source 含 `bm25`，能看到多个不同 method_no 的"仪具与材料"chunk（即库中 24 个含天平试验里被召回的部分），直观印证召回情况。

---

## 6. 不做

- 不改 `query` / `answer_query` / `text_retrieve` 行为（只新增命令调用 `text_retrieve`）。
- 不做 HTTP API、不做前端 UI。
- 不做按路分块展示。
- 不改 `query` 的 top_k 默认值（仍 5）。
- 不做列举意图自动放大 k（那是后续检索调参的事，本命令只提供观测手段）。
- 不展示完整 chunk 正文（只片段；需要全文仍可查 PG / chunk_cache）。

---

## 7. 验收标准

1. `rag2 retrievecheck "问题"` 打印融合 top-20 排名表，含 `#/rrf/source/clause-method/title/片段`。
2. `source` 标签正确反映召回路（dense/bm25/dense+bm25/exact 等）。
3. 中文片段无乱码（UTF-8 字符边界截断）。
4. `query` 行为不变（不调用 LLM；同问题回答与改动前一致）。
5. 可选 `k` 参数生效（`retrievecheck "问题" 30` 召回 30 条）。
6. 全量 doctest 通过；`utf8_truncate` 单测覆盖 §5 列表。

---

## 8. 后续衔接

本命令为检索调参提供观测手段，直接服务后续：列举意图识别（按 source 分布判断该不该放大 k）、M4 评估集（人工核对召回质量）、M5 reranker（对比 rerank 前后排名变化）。
