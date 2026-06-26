# 证据标注挖掘工具（distractor / acceptable）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 写一个独立 Python 脚本 `scripts/mine_annotations.py`，给现有 `eval/retrieval_questions_100.json` 每题自动挖出 `distractor_chunks`（像但错）与 `acceptable_chunks`（相关非必需）标注，产出 `eval/retrieval_questions_100.annotated.json`，为后续 Distractor/nDCG 指标刀供真实数据。

**Architecture:** 单模块脚本，内部拆成可单测的纯函数（gold 解析、候选池、LLM 输出解析、case 组装、prompt/请求体构造）+ 明确 IO 边界（PG via psycopg2，embedding/Milvus/DeepSeek via requests）。候选 = 问题 embedding 的 dense ANN 近邻（绕开 BM25/RRF/planner），LLM 单题一次调用判 distractor/acceptable/irrelevant；磁盘缓存 + temp0 + 增量续跑。

**Tech Stack:** Python 3.13、psycopg2（已装）、requests（已装）、pytest（需装）、Milvus REST v2、DeepSeek/SiliconFlow OpenAI 兼容 HTTP。

**Spec:** [`docs/superpowers/specs/2026-06-26-evidence-annotation-mining-design.md`](../specs/2026-06-26-evidence-annotation-mining-design.md)

**构建/测试（PowerShell 或 Bash，项目根目录）：**
```
python -m pip install pytest
python -m pytest scripts/test_mine_annotations.py -v
```
（pytest 默认 prepend import 模式会把测试文件所在目录加入 sys.path，故 `import mine_annotations` 可用。）

**关键事实（来自 spec/源码勘察）：**
- PG 表 `retrieval_chunks` 列：`chunk_id, node_id, standard_id, chunk_type, clause_no, method_no, title, path_text, atomic_text, embedding_text, context_text, page_start, page_end, ...`。
- `config.json` 端点：`RAG_PG_CONNINFO`、`RAG_MILVUS_BASE_URL`(http://localhost:19530)、`RAG_MILVUS_TOKEN`、`RAG_MILVUS_COLLECTION`(clause_text)、`RAG_EMBED_BASE_URL`(https://api.siliconflow.cn)+`RAG_EMBED_PATH`(/v1/embeddings)+`RAG_EMBED_MODEL`+`RAG_EMBED_KEY`、`RAG_DEEPSEEK_BASE_URL`(https://api.deepseek.com)+`RAG_DEEPSEEK_PATH`(/chat/completions)+`RAG_DEEPSEEK_MODEL`+`RAG_DEEPSEEK_KEY`。
- embedding 请求体仅 `{model, input}`，入库与查询同路径无前缀差异 → 查询向量与库内向量同空间。
- 100 集每题带 `source_standard_ids`（= `retrieval_chunks.standard_id`，字符串）；gold 在 `gold_method_no`/`gold_clause_no`/`gold_methods`（legacy 扁平）或 `must_have_groups[].stable_refs[]`。

**提交纪律（重要）：** 本仓库工作区有一批用户改动（`docs/.../specs/*.md`）和未跟踪垃圾（`build_out.txt`/`logs/`/`.tmp-ragflow-research/`）。**每次提交必须用 pathspec 显式列文件**（`git add <files>` 后 `git commit -m "..." -- <files>`），**绝不**用 `git add -A` 或裸 `git commit`，以免卷入他人改动。

---

## Task 1: 模块骨架 + case/gold 纯函数

**Files:**
- Create: `scripts/mine_annotations.py`
- Create: `scripts/test_mine_annotations.py`

- [ ] **Step 0: 一次性装 pytest**

Run: `python -m pip install pytest`
Expected: 安装成功（或 already satisfied）。

- [ ] **Step 1: 写失败测试 `scripts/test_mine_annotations.py`**

```python
import mine_annotations as m


def test_method_stem_strips_year():
    assert m.method_stem("T0301-2024") == "T0301"
    assert m.method_stem("T0521") == "T0521"
    assert m.method_stem("") == ""


def test_normalize_question_trims_collapses_lowercases_ascii():
    assert m.normalize_question("  JTG  3420  里  天平 ") == "jtg 3420 里 天平"
    assert m.normalize_question("天平") == "天平"


def test_load_cases_requires_array():
    assert m.load_cases('[{"question":"q"}]') == [{"question": "q"}]
    import pytest
    with pytest.raises(ValueError):
        m.load_cases('{"question":"q"}')


def test_gold_refs_from_legacy_flat():
    case = {"question": "q", "gold_method_no": "T0301-2024"}
    assert m.gold_refs_of(case) == [("method", "T0301")]
    case2 = {"question": "q", "gold_clause_no": "5.3"}
    assert m.gold_refs_of(case2) == [("clause", "5.3")]
    case3 = {"question": "q", "gold_methods": ["T0316-2024", "T0350-2005"]}
    assert m.gold_refs_of(case3) == [("method", "T0316"), ("method", "T0350")]


def test_gold_refs_from_groups_and_dedup():
    case = {
        "question": "q",
        "must_have_groups": [
            {"stable_refs": [{"method_no": "T0521-2005"}]},
            {"stable_refs": [{"clause_no": "4.2"}]},
        ],
        "gold_methods": ["T0521-2005"],  # 与组内重复
    }
    assert m.gold_refs_of(case) == [("method", "T0521"), ("clause", "4.2")]
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: FAIL —`ModuleNotFoundError: No module named 'mine_annotations'`。

- [ ] **Step 3: 创建 `scripts/mine_annotations.py`（导入 + 4 个纯函数）**

```python
"""证据标注挖掘工具：给评测题挖 distractor/acceptable chunk 标注。

见 docs/superpowers/specs/2026-06-26-evidence-annotation-mining-design.md
"""
import json
import os
import sys
import hashlib

try:
    import psycopg2
    import requests
except ImportError:  # 纯函数单测不需要这俩
    psycopg2 = None
    requests = None

GENERATOR_VERSION = "annot-v1"


def load_cases(json_text):
    data = json.loads(json_text)
    if not isinstance(data, list):
        raise ValueError("评测集应为 JSON 数组")
    return data


def method_stem(method_no):
    """方法号去年份：T0301-2024 -> T0301。"""
    if not method_no:
        return ""
    return method_no.split("-", 1)[0].strip()


def normalize_question(q):
    """trim + 折叠内部连续空白为单空格 + 仅 ASCII 大写转小写。镜像 C++ normalize_question。"""
    out = []
    in_space = False
    started = False
    for ch in q:
        if ch in " \t\r\n":
            in_space = True
            continue
        if started and in_space:
            out.append(" ")
        in_space = False
        started = True
        out.append(ch.lower() if ch.isascii() else ch)
    return "".join(out)


def gold_refs_of(case):
    """收集 gold 证据维度：[(kind, value)]，kind in {"method","clause"}，去重保序。"""
    refs = []
    for g in case.get("must_have_groups") or []:
        for r in g.get("stable_refs") or []:
            if r.get("method_no"):
                refs.append(("method", method_stem(r["method_no"])))
            elif r.get("clause_no"):
                refs.append(("clause", r["clause_no"]))
    if case.get("gold_method_no"):
        refs.append(("method", method_stem(case["gold_method_no"])))
    if case.get("gold_clause_no"):
        refs.append(("clause", case["gold_clause_no"]))
    for mth in case.get("gold_methods") or []:
        refs.append(("method", method_stem(mth)))
    return list(dict.fromkeys(refs))
```

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 6 passed。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): 模块骨架 + case/gold 纯函数 (method_stem/normalize/gold_refs)" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 2: build_candidate_pool 纯函数

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加失败测试**

```python
def test_build_candidate_pool_excludes_gold_dedups_truncates():
    hits = [
        {"chunk_id": "a"}, {"chunk_id": "g1"}, {"chunk_id": "b"},
        {"chunk_id": "a"}, {"chunk_id": "c"}, {"chunk_id": "d"},
    ]
    gold = {"g1"}
    # top_n=3：排除 gold g1、去重 a、按序取前 3 = a,b,c
    assert m.build_candidate_pool(hits, gold, 3) == ["a", "b", "c"]


def test_build_candidate_pool_handles_insufficient():
    hits = [{"chunk_id": "a"}, {"chunk_id": "g"}]
    assert m.build_candidate_pool(hits, {"g"}, 10) == ["a"]
    assert m.build_candidate_pool([], set(), 5) == []
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k build_candidate_pool -v`
Expected: FAIL —`AttributeError: module 'mine_annotations' has no attribute 'build_candidate_pool'`。

- [ ] **Step 3: 实现 `build_candidate_pool`（追加到 `scripts/mine_annotations.py`）**

```python
def build_candidate_pool(hits, gold_ids, top_n):
    """从已按分数降序的 hits 里排除 gold 及重复，截断 top_n。hits[i]={"chunk_id":...}。"""
    pool = []
    seen = set()
    for h in hits:
        cid = h["chunk_id"]
        if cid in gold_ids or cid in seen:
            continue
        seen.add(cid)
        pool.append(cid)
        if len(pool) >= top_n:
            break
    return pool
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed（含新 2 条）。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): build_candidate_pool 纯函数 (排除 gold/去重/截断)" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 3: parse_llm_labels 纯函数（含宽松 JSON）

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加失败测试**

```python
def test_parse_llm_labels_buckets_valid():
    resp = ('[{"chunk_id":"a","label":"distractor","reason":"像但错"},'
            '{"chunk_id":"b","label":"acceptable","reason":"相关"},'
            '{"chunk_id":"c","label":"irrelevant","reason":"无关"}]')
    out = m.parse_llm_labels(resp, ["a", "b", "c"])
    assert out == {"distractor": ["a"], "acceptable": ["b"]}


def test_parse_llm_labels_ignores_out_of_range_and_bad_label():
    resp = ('[{"chunk_id":"z","label":"distractor"},'   # 越界 id
            '{"chunk_id":"a","label":"nonsense"},'        # 非法 label
            '{"chunk_id":"b","label":"distractor"}]')
    out = m.parse_llm_labels(resp, ["a", "b"])
    assert out == {"distractor": ["b"], "acceptable": []}


def test_parse_llm_labels_empty_array_is_ok():
    assert m.parse_llm_labels("[]", ["a"]) == {"distractor": [], "acceptable": []}


def test_parse_llm_labels_garbage_returns_none():
    assert m.parse_llm_labels("not json", ["a"]) is None
    assert m.parse_llm_labels('{"intent":"x"}', ["a"]) is None  # 非数组
    assert m.parse_llm_labels(None, ["a"]) is None


def test_parse_llm_labels_strips_code_fence():
    resp = '```json\n[{"chunk_id":"a","label":"distractor"}]\n```'
    out = m.parse_llm_labels(resp, ["a"])
    assert out == {"distractor": ["a"], "acceptable": []}
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k parse_llm_labels -v`
Expected: FAIL（无 `parse_llm_labels`）。

- [ ] **Step 3: 实现（追加到 `scripts/mine_annotations.py`）**

```python
def _loads_lenient(text):
    """先直接 json.loads；失败则截取首个 [ 到末个 ] 重试（容忍代码块围栏/前后赘语）。"""
    if not text:
        return None
    try:
        return json.loads(text)
    except (ValueError, TypeError):
        pass
    a = text.find("[")
    b = text.rfind("]")
    if a != -1 and b > a:
        try:
            return json.loads(text[a:b + 1])
        except (ValueError, TypeError):
            return None
    return None


def parse_llm_labels(resp_text, candidate_ids):
    """解析 LLM JSON 数组 → {"distractor":[...],"acceptable":[...]}。
    非数组/解析失败 → None（调用方记 generation_error）。越界 id/非法 label → 丢该条。"""
    data = _loads_lenient(resp_text)
    if not isinstance(data, list):
        return None
    allowed = set(candidate_ids)
    out = {"distractor": [], "acceptable": []}
    for item in data:
        if not isinstance(item, dict):
            continue
        cid = item.get("chunk_id")
        label = item.get("label")
        if cid not in allowed:
            continue
        if label == "distractor" and cid not in out["distractor"]:
            out["distractor"].append(cid)
        elif label == "acceptable" and cid not in out["acceptable"]:
            out["acceptable"].append(cid)
    return out
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): parse_llm_labels 纯函数 (宽松JSON/越界丢弃/失败None)" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 4: assemble_annotated_case 纯函数

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加失败测试**

```python
def test_assemble_annotated_case_writes_fields_and_provenance():
    case = {"case_id": "rq-1", "question": "q", "gold_method_no": "T0301-2024"}
    labels = {"distractor": ["d1"], "acceptable": ["a1", "a2"]}
    meta = {"generator_version": "annot-v1", "candidate_top_n": 30, "validation_status": "auto"}
    out = m.assemble_annotated_case(case, labels, meta)
    assert out["distractor_chunks"] == ["d1"]
    assert out["acceptable_chunks"] == ["a1", "a2"]
    assert out["question"] == "q"               # 原字段不丢
    assert out["gold_method_no"] == "T0301-2024"
    gen = out["generation"]
    assert gen["generator_version"] == "annot-v1"
    assert gen["annotation_source"] == "embedding+llm"
    assert gen["validation_status"] == "auto"
    assert gen["expert_review"] == "unreviewed"
    assert gen["candidate_top_n"] == 30


def test_assemble_preserves_existing_generation_keys():
    case = {"question": "q", "generation": {"expert_review": "reviewed", "extra": 1}}
    meta = {"generator_version": "annot-v1", "candidate_top_n": 5, "validation_status": "auto"}
    out = m.assemble_annotated_case(case, {"distractor": [], "acceptable": []}, meta)
    assert out["generation"]["extra"] == 1
    assert out["generation"]["expert_review"] == "reviewed"  # 已有值不覆盖
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k assemble -v`
Expected: FAIL（无 `assemble_annotated_case`）。

- [ ] **Step 3: 实现（追加到 `scripts/mine_annotations.py`）**

```python
def assemble_annotated_case(case, labels, meta):
    """复制 case，写回 distractor/acceptable 两字段 + generation 溯源块（保留已有键）。"""
    out = dict(case)
    out["distractor_chunks"] = labels["distractor"]
    out["acceptable_chunks"] = labels["acceptable"]
    gen = dict(out.get("generation") or {})
    gen["generator_version"] = meta["generator_version"]
    gen["annotation_source"] = "embedding+llm"
    gen["validation_status"] = meta["validation_status"]
    gen.setdefault("expert_review", "unreviewed")
    gen["candidate_top_n"] = meta["candidate_top_n"]
    out["generation"] = gen
    return out
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): assemble_annotated_case 纯函数 (写回字段+溯源块)" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 5: system prompt + gold_brief + user message 构造

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加失败测试**

```python
def test_build_gold_brief_from_context():
    case = {"question": "q", "gold_method_no": "T0301-2024"}
    gold_ctx = {"c1": {"chunk_id": "c1", "method_no": "T0301-2024", "clause_no": "",
                       "title": "细集料取样", "snippet": "..."}}
    assert m.build_gold_brief(case, gold_ctx) == "T0301-2024 细集料取样"


def test_build_gold_brief_fallback_when_no_context():
    case = {"question": "q", "gold_method_no": "T0301-2024"}
    assert m.build_gold_brief(case, {}) == "T0301-2024"


def test_build_user_message_lists_candidates():
    cands = [
        {"chunk_id": "c1", "method_no": "T0302-2024", "clause_no": "", "title": "粗集料取样", "snippet": "正文片段X"},
        {"chunk_id": "c2", "method_no": "", "clause_no": "5.3", "title": "养护", "snippet": "正文片段Y"},
    ]
    msg = m.build_user_message("怎么取样", "T0301-2024 细集料取样", cands)
    assert "怎么取样" in msg
    assert "T0301-2024 细集料取样" in msg
    assert "c1" in msg and "c2" in msg
    assert "粗集料取样" in msg and "养护" in msg
    assert "正文片段X" in msg


def test_system_prompt_and_version_present():
    assert "distractor" in m.SYSTEM_PROMPT
    assert "acceptable" in m.SYSTEM_PROMPT
    assert m.GENERATOR_VERSION == "annot-v1"
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k "gold_brief or user_message or system_prompt" -v`
Expected: FAIL（无 `SYSTEM_PROMPT`/`build_gold_brief`/`build_user_message`）。

- [ ] **Step 3: 实现（追加到 `scripts/mine_annotations.py`）**

```python
SYSTEM_PROMPT = (
    "你是公路工程标准检索评测的标注助手。给定一个问题、它的正确答案简述、"
    "以及若干候选 chunk，为每个候选打一个标签，只输出严格 JSON 数组，"
    "不要解释、不要代码块围栏。\n"
    "标签三选一：\n"
    "- \"distractor\"：主题/标题/仪器与正确答案高度相似，但试验对象、版本、适用条件或"
    "结论错误——会诱导检索器误判的“像但错”。\n"
    "- \"acceptable\"：与问题相关、对理解有帮助，但不是回答必需"
    "（如父条款概述、等价表格、背景说明）。\n"
    "- \"irrelevant\"：与问题无实质关系。\n"
    "输出格式：[{\"chunk_id\":\"...\",\"label\":\"distractor|acceptable|irrelevant\","
    "\"reason\":\"简短理由\"}]"
)


def build_gold_brief(case, gold_ctx):
    """正确答案简述：gold chunk 的 (方法号/条款号 + 标题)，去重；无上下文时退回扁平 gold 字段。"""
    parts = []
    for c in gold_ctx.values():
        tag = c["method_no"] or c["clause_no"]
        parts.append((tag + " " + c["title"]).strip())
    parts = [p for p in dict.fromkeys(parts) if p]
    if parts:
        return "; ".join(parts)
    return case.get("gold_method_no") or case.get("gold_clause_no") or "(未知)"


def build_user_message(question, gold_brief, candidates):
    """组装给 LLM 的 user 消息：问题 + 正确答案 + 编号候选表。"""
    lines = [f"问题：{question}", f"正确答案：{gold_brief}", "", "候选 chunk："]
    for i, c in enumerate(candidates, 1):
        tag = c["method_no"] or c["clause_no"] or "-"
        lines.append(f"{i}. chunk_id={c['chunk_id']} [{tag}] {c['title']}")
        lines.append(f"   正文：{c['snippet']}")
    return "\n".join(lines)
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): system prompt + gold_brief + user message 构造" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 6: HTTP 请求体构造 + cache_key 纯函数

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加失败测试**

```python
def test_build_embedding_body():
    assert m.build_embedding_body("Qwen/Qwen3-Embedding-8B", "天平") == {
        "model": "Qwen/Qwen3-Embedding-8B", "input": "天平"}


def test_build_milvus_search_body():
    body = m.build_milvus_search_body("clause_text", [0.1, 0.2], 30, ["chunk_id", "standard_id"])
    assert body["collectionName"] == "clause_text"
    assert body["data"] == [[0.1, 0.2]]
    assert body["limit"] == 30
    assert body["outputFields"] == ["chunk_id", "standard_id"]


def test_build_deepseek_body_temp0():
    body = m.build_deepseek_body("deepseek-v4-pro", "SYS", "USER")
    assert body["model"] == "deepseek-v4-pro"
    assert body["temperature"] == 0
    assert body["messages"][0] == {"role": "system", "content": "SYS"}
    assert body["messages"][1] == {"role": "user", "content": "USER"}


def test_cache_key_deterministic_and_order_independent():
    k1 = m.cache_key("  天平 ", ["b", "a"], "annot-v1")
    k2 = m.cache_key("天平", ["a", "b"], "annot-v1")
    assert k1 == k2                                  # 归一化 + 候选 id 排序后同键
    k3 = m.cache_key("天平", ["a", "b"], "annot-v2")
    assert k3 != k1                                  # 版本变 → 键变
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k "body or cache_key" -v`
Expected: FAIL。

- [ ] **Step 3: 实现（追加到 `scripts/mine_annotations.py`）**

```python
def build_embedding_body(model, text):
    return {"model": model, "input": text}


def build_milvus_search_body(collection, vector, top_n, output_fields):
    return {"collectionName": collection, "data": [vector], "limit": top_n,
            "outputFields": output_fields}


def build_deepseek_body(model, system, user):
    return {"model": model,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "temperature": 0}


def cache_key(question, candidate_ids, version):
    raw = normalize_question(question) + "\x1f" + ",".join(sorted(candidate_ids)) + "\x1f" + version
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): HTTP 请求体构造 + cache_key 纯函数" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 7: IO 层（config/缓存/PG/embedding/Milvus/DeepSeek）+ PG 假游标测试

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加 PG 纯逻辑测试（假游标，不连真库）**

```python
class _FakeCursor:
    def __init__(self, rows_by_call):
        self._rows_by_call = list(rows_by_call)
        self.executed = []
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False
    def execute(self, sql, params=None):
        self.executed.append((sql, params))
    def fetchall(self):
        return self._rows_by_call.pop(0) if self._rows_by_call else []


class _FakeConn:
    def __init__(self, cur):
        self._cur = cur
    def cursor(self):
        return self._cur


def test_resolve_gold_chunks_method_uses_stem_prefix_and_standard_filter():
    cur = _FakeCursor([[("ck1",), ("ck2",)]])
    conn = _FakeConn(cur)
    case = {"question": "q", "gold_method_no": "T0301-2024",
            "source_standard_ids": ["sid-1"]}
    out = m.resolve_gold_chunks(conn, case)
    assert out == {"ck1", "ck2"}
    sql, params = cur.executed[0]
    assert "method_no LIKE" in sql and "standard_id = ANY" in sql
    assert params == ("T0301%", ["sid-1"])


def test_resolve_gold_chunks_clause_without_standard():
    cur = _FakeCursor([[("ck9",)]])
    conn = _FakeConn(cur)
    case = {"question": "q", "gold_clause_no": "5.3"}
    out = m.resolve_gold_chunks(conn, case)
    assert out == {"ck9"}
    sql, params = cur.executed[0]
    assert "clause_no = " in sql and "ANY" not in sql
    assert params == ("5.3",)


def test_fetch_chunk_context_shapes_rows():
    cur = _FakeCursor([[("c1", "T0302-2024", "", "粗集料取样", "embE", "atomA")]])
    conn = _FakeConn(cur)
    out = m.fetch_chunk_context(conn, ["c1"])
    assert out["c1"]["title"] == "粗集料取样"
    assert out["c1"]["method_no"] == "T0302-2024"
    assert out["c1"]["snippet"] == "atomA"   # atomic_text 优先于 embedding_text


def test_fetch_chunk_context_empty_input():
    assert m.fetch_chunk_context(_FakeConn(_FakeCursor([])), []) == {}
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k "resolve_gold or fetch_chunk" -v`
Expected: FAIL（无 `resolve_gold_chunks`/`fetch_chunk_context`）。

- [ ] **Step 3: 实现 IO 层（追加到 `scripts/mine_annotations.py`）**

```python
# ---------- config / cache ----------
def load_config(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_cache(cache_dir, key):
    p = os.path.join(cache_dir, key + ".json")
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as f:
        return f.read()


def write_cache(cache_dir, key, content):
    os.makedirs(cache_dir, exist_ok=True)
    with open(os.path.join(cache_dir, key + ".json"), "w", encoding="utf-8") as f:
        f.write(content)


# ---------- PG ----------
def pg_connect(conninfo):
    return psycopg2.connect(conninfo)


def resolve_gold_chunks(conn, case):
    """gold 方法号(stem 前缀)/条款号 → 该标准下全部 chunk_id 集（排除自身用）。"""
    sids = case.get("source_standard_ids") or []
    ids = set()
    with conn.cursor() as cur:
        for kind, val in gold_refs_of(case):
            if kind == "method":
                if sids:
                    cur.execute(
                        "SELECT chunk_id FROM retrieval_chunks "
                        "WHERE method_no LIKE %s AND standard_id = ANY(%s)",
                        (val + "%", list(sids)))
                else:
                    cur.execute(
                        "SELECT chunk_id FROM retrieval_chunks WHERE method_no LIKE %s",
                        (val + "%",))
            else:  # clause
                if sids:
                    cur.execute(
                        "SELECT chunk_id FROM retrieval_chunks "
                        "WHERE clause_no = %s AND standard_id = ANY(%s)",
                        (val, list(sids)))
                else:
                    cur.execute(
                        "SELECT chunk_id FROM retrieval_chunks WHERE clause_no = %s",
                        (val,))
            for row in cur.fetchall():
                ids.add(row[0])
    return ids


def fetch_chunk_context(conn, chunk_ids):
    """回查候选/gold chunk 的展示上下文。返回 {chunk_id: {chunk_id,method_no,clause_no,title,snippet}}。"""
    out = {}
    ids = list(chunk_ids)
    if not ids:
        return out
    with conn.cursor() as cur:
        cur.execute(
            "SELECT chunk_id, method_no, clause_no, title, embedding_text, atomic_text "
            "FROM retrieval_chunks WHERE chunk_id = ANY(%s)", (ids,))
        for row in cur.fetchall():
            cid, method_no, clause_no, title, emb, atom = row
            snippet = (atom or emb or "")[:300]
            out[cid] = {"chunk_id": cid, "method_no": method_no or "",
                        "clause_no": clause_no or "", "title": title or "",
                        "snippet": snippet}
    return out


# ---------- HTTP: embedding / Milvus / DeepSeek ----------
def _http_post_json(url, token, body, timeout=60):
    headers = {"Authorization": "Bearer " + token}
    r = requests.post(url, headers=headers, json=body, timeout=timeout)
    r.raise_for_status()
    return r.json()


def embed_text(cfg, text):
    url = cfg["RAG_EMBED_BASE_URL"].rstrip("/") + cfg["RAG_EMBED_PATH"]
    body = build_embedding_body(cfg["RAG_EMBED_MODEL"], text)
    data = _http_post_json(url, cfg["RAG_EMBED_KEY"], body)
    return data["data"][0]["embedding"]


def milvus_search(cfg, vector, top_n):
    url = cfg["RAG_MILVUS_BASE_URL"].rstrip("/") + "/v2/vectordb/entities/search"
    body = build_milvus_search_body(cfg["RAG_MILVUS_COLLECTION"], vector, top_n,
                                    ["chunk_id", "standard_id"])
    data = _http_post_json(url, cfg["RAG_MILVUS_TOKEN"], body)
    hits = []
    for row in data.get("data", []):
        hits.append({"chunk_id": row.get("chunk_id", ""),
                     "standard_id": row.get("standard_id", ""),
                     "score": row.get("distance", 0.0)})
    return hits


def llm_call(cfg, system, user):
    url = cfg["RAG_DEEPSEEK_BASE_URL"].rstrip("/") + cfg["RAG_DEEPSEEK_PATH"]
    body = build_deepseek_body(cfg["RAG_DEEPSEEK_MODEL"], system, user)
    data = _http_post_json(url, cfg["RAG_DEEPSEEK_KEY"], body, timeout=120)
    return data["choices"][0]["message"]["content"]


def llm_call_with_retry(cfg, system, user, candidate_ids, retries=2):
    """调 LLM，校验输出可解析；失败重试 retries 次；仍失败返回 None。"""
    for attempt in range(retries + 1):
        try:
            resp = llm_call(cfg, system, user)
        except Exception as e:  # 网络/HTTP 异常
            sys.stderr.write(f"[annot] LLM 调用异常(第{attempt + 1}次): {e}\n")
            continue
        if parse_llm_labels(resp, candidate_ids) is not None:
            return resp
        sys.stderr.write(f"[annot] LLM 输出非法(第{attempt + 1}次)，重试\n")
    return None
```

- [ ] **Step 4: 运行确认通过**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed（含 4 条假游标用例）。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): IO 层 (config/cache/PG/embedding/Milvus/DeepSeek) + 假游标测试" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 8: 编排 process_case + review 渲染 + main/CLI

**Files:**
- Modify: `scripts/mine_annotations.py`
- Modify: `scripts/test_mine_annotations.py`

- [ ] **Step 1: 追加 review 渲染纯函数测试**

```python
def test_render_review_labels_each_candidate():
    reviews = [{
        "question": "怎么取样",
        "gold_brief": "T0301-2024",
        "candidates": [
            {"chunk_id": "d1", "method_no": "T0302-2024", "clause_no": "", "title": "粗集料取样", "snippet": "x"},
            {"chunk_id": "a1", "method_no": "", "clause_no": "2", "title": "概述", "snippet": "y"},
            {"chunk_id": "i1", "method_no": "", "clause_no": "", "title": "无关", "snippet": "z"},
        ],
        "labels": {"distractor": ["d1"], "acceptable": ["a1"]},
    }]
    md = m.render_review(reviews)
    assert "怎么取样" in md
    assert "[distractor] d1" in md
    assert "[acceptable] a1" in md
    assert "[irrelevant] i1" in md


def test_build_review_entry_shape():
    case = {"question": "q", "gold_method_no": "T0301-2024"}
    entry = m.build_review_entry(case, {"g"}, [{"chunk_id": "c1"}], {"distractor": [], "acceptable": []})
    assert entry["question"] == "q"
    assert entry["gold_brief"] == "T0301-2024"
    assert entry["candidates"] == [{"chunk_id": "c1"}]
    assert entry["labels"] == {"distractor": [], "acceptable": []}
```

- [ ] **Step 2: 运行确认失败**

Run: `python -m pytest scripts/test_mine_annotations.py -k "render_review or review_entry" -v`
Expected: FAIL。

- [ ] **Step 3: 实现编排 + review + main（追加到 `scripts/mine_annotations.py`）**

```python
def build_review_entry(case, gold_ids, candidates, labels):
    return {"question": case["question"],
            "gold_brief": case.get("gold_method_no") or case.get("gold_clause_no") or "",
            "candidates": candidates,
            "labels": labels}


def render_review(reviews):
    out = ["# 标注人审导出\n"]
    for r in reviews:
        out.append(f"## {r['question']}")
        out.append(f"- gold: {r['gold_brief']}")
        d = set(r["labels"]["distractor"])
        a = set(r["labels"]["acceptable"])
        for c in r["candidates"]:
            cid = c["chunk_id"]
            lab = "distractor" if cid in d else ("acceptable" if cid in a else "irrelevant")
            tag = c.get("method_no") or c.get("clause_no") or "-"
            title = c.get("title", "")
            snip = c.get("snippet", "")[:80]
            out.append(f"  - [{lab}] {cid} [{tag}] {title} — {snip}")
        out.append("")
    return "\n".join(out)


def process_case(case, conn, cfg, cache_dir, top_n):
    """单题流水线：gold 排除 → embedding 候选 → LLM 复核（缓存）→ 组装。返回 (annotated, review)。"""
    question = case["question"]
    gold_ids = resolve_gold_chunks(conn, case)
    if not gold_ids:
        sys.stderr.write(f"[annot] 警告：gold 解析为空: {question[:40]}\n")
    vec = embed_text(cfg, question)
    hits = milvus_search(cfg, vec, top_n + len(gold_ids) + 10)   # 过量取，扣掉 gold 后仍够 top_n
    pool = build_candidate_pool(hits, gold_ids, top_n)
    ctx = fetch_chunk_context(conn, pool)
    candidates = [ctx[c] for c in pool if c in ctx]
    cand_ids = [c["chunk_id"] for c in candidates]

    key = cache_key(question, cand_ids, GENERATOR_VERSION)
    resp = read_cache(cache_dir, key)
    if resp is None:
        gold_ctx = fetch_chunk_context(conn, gold_ids)
        user = build_user_message(question, build_gold_brief(case, gold_ctx), candidates)
        resp = llm_call_with_retry(cfg, SYSTEM_PROMPT, user, cand_ids)
        if resp is not None:
            write_cache(cache_dir, key, resp)

    labels = parse_llm_labels(resp, cand_ids) if resp is not None else None
    status = "auto"
    if labels is None:
        labels = {"distractor": [], "acceptable": []}
        status = "generation_error"
    meta = {"generator_version": GENERATOR_VERSION, "candidate_top_n": top_n,
            "validation_status": status}
    return (assemble_annotated_case(case, labels, meta),
            build_review_entry(case, gold_ids, candidates, labels))


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="证据标注挖掘 (distractor/acceptable)")
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", required=True)
    ap.add_argument("--top-n", type=int, default=30)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--review", default="")
    ap.add_argument("--config", default="config.json")
    args = ap.parse_args(argv)

    cfg = load_config(args.config)
    with open(args.inp, encoding="utf-8") as f:
        cases = load_cases(f.read())
    if args.limit:
        cases = cases[:args.limit]

    # 增量续跑：已 auto 标注且版本匹配的题直接复用
    done = {}
    if os.path.exists(args.out):
        with open(args.out, encoding="utf-8") as f:
            for c in load_cases(f.read()):
                gen = c.get("generation") or {}
                if (gen.get("generator_version") == GENERATOR_VERSION
                        and gen.get("validation_status") == "auto"):
                    done[c.get("case_id") or c["question"]] = c

    conn = pg_connect(cfg["RAG_PG_CONNINFO"])
    cache_dir = "data/annotation_cache"
    out_cases, reviews = [], []
    for case in cases:
        cid = case.get("case_id") or case["question"]
        if cid in done:
            out_cases.append(done[cid])
            continue
        annotated, review = process_case(case, conn, cfg, cache_dir, args.top_n)
        out_cases.append(annotated)
        reviews.append(review)
        with open(args.out, "w", encoding="utf-8") as f:   # 增量写检查点
            json.dump(out_cases, f, ensure_ascii=False, indent=1)
        sys.stderr.write(
            f"[annot] {cid}: distractor={len(annotated['distractor_chunks'])} "
            f"acceptable={len(annotated['acceptable_chunks'])}\n")

    if args.review and reviews:
        with open(args.review, "w", encoding="utf-8") as f:
            f.write(render_review(reviews))
    sys.stderr.write(f"[annot] 完成 {len(out_cases)} 题，输出 {args.out}\n")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: 运行确认通过 + 编译检查**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed。
Run: `python -c "import scripts.mine_annotations"` 或 `python scripts/mine_annotations.py --help`
Expected: 打印 usage（无导入错误；证明 main/argparse 正常）。

- [ ] **Step 5: Commit**

```
git add scripts/mine_annotations.py scripts/test_mine_annotations.py
git commit -m "feat(annot): 编排 process_case + review 渲染 + main/CLI (增量续跑)" -- scripts/mine_annotations.py scripts/test_mine_annotations.py
```

---

## Task 9: requirements + 集成验收（真实服务，小样本）

**Files:**
- Create: `scripts/requirements.txt`

- [ ] **Step 1: 写 `scripts/requirements.txt`**

```
psycopg2-binary>=2.9
requests>=2.31
pytest>=8.0
```

- [ ] **Step 2: 确认服务可用**

Run: `docker ps --filter "name=milvus-standalone" --format "{{.Names}} {{.Status}}"`
Expected: milvus-standalone healthy。
（`config.json` 的 `RAG_DEEPSEEK_KEY`/`RAG_EMBED_KEY` 已填；PG 在 localhost:5432。）

- [ ] **Step 3: 全量纯函数测试**

Run: `python -m pytest scripts/test_mine_annotations.py -v`
Expected: 全 passed（约 24 条）。

- [ ] **Step 4: 集成跑 5 题 + review 导出**

Run:
```
python scripts/mine_annotations.py --in eval/retrieval_questions_100.json --out eval/_smoke5.annotated.json --limit 5 --review eval/_smoke5.review.md
```
Expected：
- stderr 逐题打印 `distractor=N acceptable=M`；末行 `完成 5 题`。
- `eval/_smoke5.annotated.json` 是 5 条数组，每条含 `distractor_chunks`/`acceptable_chunks`/`generation`。

- [ ] **Step 5: 人工核对 + 缓存/续跑验证**

- 打开 `eval/_smoke5.review.md`，抽看 2~3 题：distractor 是否确为“像但错”、acceptable 是否确为“相关非必需”、gold 是否未被列入候选。
- 缓存命中：再跑一次同命令，stderr 不应再出现 `LLM 调用`相关日志的等待（秒级返回），且输出逐字不变。
- 增量续跑：手动删除 `eval/_smoke5.annotated.json` 里最后 1 条后重跑（不删缓存），应只补这 1 条、其余复用。
- 验证 C++ 可读：`.\rag2.0\x64\Debug\rag2.0.exe`（若已构建）或下个指标刀用 `parse_dataset` 读 `eval/_smoke5.annotated.json` 不报错（字段名对齐）。

> 验收门槛（spec §11.6）：抽检 distractor/acceptable 错误率 ≤ 20%。若超标，回头调 `SYSTEM_PROMPT` 判据或 `--top-n`，重跑（缓存键含版本，改 prompt 须同时升 `GENERATOR_VERSION` 令缓存失效）。

- [ ] **Step 6: 清理 smoke 产物 + Commit requirements**

```
rm -f eval/_smoke5.annotated.json eval/_smoke5.review.md
git add scripts/requirements.txt
git commit -m "chore(annot): scripts/requirements.txt (psycopg2/requests/pytest)" -- scripts/requirements.txt
```

> 注：`eval/_smoke5.*` 是临时验收产物，不入库。正式全量标注（`--in eval/retrieval_questions_100.json --out eval/retrieval_questions_100.annotated.json`）在验收通过后单独跑并按需提交标注数据集（见下方“全量运行”）。

---

## 全量运行（验收通过后，非本计划自动步骤）

```
python scripts/mine_annotations.py --in eval/retrieval_questions_100.json --out eval/retrieval_questions_100.annotated.json --review eval/retrieval_questions_100.review.md
git add eval/retrieval_questions_100.annotated.json
git commit -m "data(eval): 100 题 distractor/acceptable 标注 (annot-v1)" -- eval/retrieval_questions_100.annotated.json
```
（`data/annotation_cache/` 与 `eval/*.review.md` 是中间产物，建议 gitignore，不入库。）

---

## Self-Review

**1. Spec 覆盖：**
- §2 目标（挖 distractor/acceptable → 新文件）→ Task 8 main + Task 4 assemble。
- §3 范围（Python 脚本、100 集、dense ANN 候选、LLM 复核、缓存/续跑、--review）→ Task 6/7/8。
- §4 核心决策（决策 1-6）→ 候选锚点=问题 embedding(Task 8 process_case)、全库无 standard 过滤(milvus_search 不传 filter)、Python(全程)、只 auto+人审(Task 8 status/review)。
- §5 数据流 → Task 8 process_case 顺序与图一致。
- §6 组件表 → load_config/load_cases/resolve_gold_chunks/embed_text/milvus_search/build_candidate_pool/fetch_chunk_context/llm_classify(=llm_call_with_retry)/parse_llm_labels/assemble_annotated_case/write_review 全部落到 Task 1-8。§6.1 gold 解析(stem 前缀/条款/合并来源)→ Task 1 gold_refs_of + Task 7 resolve_gold_chunks。embedding 同空间 → Task 6/7 body 仅 model+input。
- §7 LLM 契约（system prompt 判据、JSON 输出、越界/非法丢弃）→ Task 5 SYSTEM_PROMPT + Task 3 parse_llm_labels。
- §8 确定性/可恢复（版本常量、磁盘缓存、增量写、重试、非法→generation_error）→ Task 6 cache_key + Task 7 read/write_cache + llm_call_with_retry + Task 8 main 续跑 + status。
- §9 产物格式（两字段 + generation 溯源块；--review md）→ Task 4 + Task 8 render_review。
- §10 测试（纯函数 pytest 列表 + 集成验收）→ Task 1-8 单测 + Task 9 集成。
- §11 验收 → Task 9 Step 4-5（含 ≤20% 门槛、缓存命中、续跑、C++ 可读）。

**2. Placeholder 扫描：** 无 TBD/TODO；每个 code step 给完整代码与确切命令。

**3. 类型一致性：**
- candidate dict `{chunk_id,method_no,clause_no,title,snippet}`：fetch_chunk_context 产出 → build_user_message / render_review / build_candidate_pool(只用 chunk_id) 消费，一致。
- hits `[{chunk_id,standard_id,score}]`：milvus_search 产出 → build_candidate_pool 用 `h["chunk_id"]`，一致。
- labels `{distractor:[],acceptable:[]}`：parse_llm_labels 产出 → assemble_annotated_case / render_review / build_review_entry 消费，一致。
- meta `{generator_version,candidate_top_n,validation_status}`：process_case 造 → assemble_annotated_case 读三键，一致。
- `GENERATOR_VERSION`/`SYSTEM_PROMPT`/`cache_key(question,candidate_ids,version)`/`llm_call_with_retry(...,candidate_ids,...)` 跨任务签名一致。
- gold_refs_of 返回 `[(kind,value)]` → resolve_gold_chunks 解构 `for kind,val in`，一致。

**4. 已知限制（实现者须知）：**
- Milvus REST v2 search 响应里相似度字段名按 `distance` 解析；若该 Milvus 版本返回 `score`，在 `milvus_search` 里改读 `row.get("score", row.get("distance",0.0))`（不影响排序，仅日志）。
- DeepSeek `temperature:0` + 磁盘缓存共同保证同题确定；个别题 LLM 仍可能漂移，靠缓存冻结首个有效产物。
- 候选锚点为问题 embedding；若某题 dense 召回质量差，候选池可能缺好干扰项 → 该题 distractor 偏少（属已知，spec 承认“无标注样本不进分母”）。
