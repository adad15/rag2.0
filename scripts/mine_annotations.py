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
