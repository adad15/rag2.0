# -*- coding: utf-8 -*-
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


SYSTEM_PROMPT = (
    "你是公路工程标准检索评测的标注助手。给定一个问题、它的正确答案简述、"
    "以及若干候选 chunk，为每个候选打一个标签，只输出严格 JSON 数组，"
    "不要解释、不要代码块围栏。\n"
    "标签三选一：\n"
    '- "distractor"：主题/标题/仪器与正确答案高度相似，但试验对象、版本、适用条件或'
    "结论错误——会诱导检索器误判的"
    "“像但错”。\n"
    '- "acceptable"：与问题相关、对理解有帮助，但不是回答必需'
    "（如父条款概述、等价表格、背景说明）。\n"
    '- "irrelevant"：与问题无实质关系。\n'
    '输出格式：[{"chunk_id":"...","label":"distractor|acceptable|irrelevant",'
    '"reason":"简短理由"}]'
)


def build_gold_brief(case, gold_ctx):
    """正确答案简述：gold chunk 的 (方法号/条款号 + 标题)，去重保序；无上下文时退回扁平 gold 字段。"""
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
