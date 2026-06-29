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


def _resolve_cid(item, candidate_ids, allowed):
    """把 LLM 条目映射回候选 chunk_id：优先 1-based index，回退 chunk_id 全等；匹配不到→None。
    （index 更稳：LLM 回整数远比 echo 40 字符复合 id 可靠，避免截断丢失。）"""
    idx = item.get("index")
    if isinstance(idx, str) and idx.strip().isdigit():
        idx = int(idx)
    if isinstance(idx, int) and not isinstance(idx, bool) and 1 <= idx <= len(candidate_ids):
        return candidate_ids[idx - 1]
    cid = item.get("chunk_id")
    if cid in allowed:
        return cid
    return None


def parse_llm_labels(resp_text, candidate_ids):
    """解析 LLM JSON 数组 → {"distractor":[...],"acceptable":[...]}（值为 chunk_id）。
    非数组/解析失败 → None（调用方记 generation_error）。条目经 index/chunk_id 映射，
    匹配不到/非法 label → 丢该条；同 chunk 只进一个桶（首个有效标签胜出）。"""
    data = _loads_lenient(resp_text)
    if not isinstance(data, list):
        return None
    allowed = set(candidate_ids)
    seen = set()
    out = {"distractor": [], "acceptable": []}
    for item in data:
        if not isinstance(item, dict):
            continue
        cid = _resolve_cid(item, candidate_ids, allowed)
        label = item.get("label")
        if cid is None or cid in seen:
            continue
        if label == "distractor":
            seen.add(cid)
            out["distractor"].append(cid)
        elif label == "acceptable":
            seen.add(cid)
            out["acceptable"].append(cid)
    return out


def reasons_from_response(resp_text, candidate_ids=None):
    """从 LLM 响应抽 {chunk_id: reason}，供人审导出；解析失败返回 {}。
    经 index/chunk_id 映射回候选 chunk_id（与 parse_llm_labels 同口径）。"""
    data = _loads_lenient(resp_text)
    if not isinstance(data, list):
        return {}
    cids = candidate_ids or []
    allowed = set(cids)
    out = {}
    for item in data:
        if not isinstance(item, dict):
            continue
        cid = _resolve_cid(item, cids, allowed)
        if cid is not None:
            out[cid] = item.get("reason", "")
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
    "用候选的序号（index，从 1 开始）回标签，所有候选都要出现，只输出严格 JSON 数组：\n"
    '[{"index":1,"label":"distractor|acceptable|irrelevant","reason":"简短理由"}]'
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
    """组装给 LLM 的 user 消息：问题 + 正确答案 + 编号候选表。
    用序号、不暴露长 chunk_id（避免 LLM 回传时截断/抄错；它只需按序号回标签）。"""
    lines = [f"问题：{question}", f"正确答案：{gold_brief}", "", "候选（按序号回标签）："]
    for i, c in enumerate(candidates, 1):
        tag = c["method_no"] or c["clause_no"] or "-"
        lines.append(f"{i}. [{tag}] {c['title']}")
        lines.append(f"   正文：{c['snippet']}")
    return "\n".join(lines)


def build_embedding_body(model, text):
    return {"model": model, "input": text}


def build_milvus_search_body(collection, vector, top_n, output_fields, anns_field="dense"):
    # clause_text 是 dense+sparse 双向量集，REST 搜索必须指定 annsField，否则报 code 1801。
    return {"collectionName": collection, "data": [vector], "annsField": anns_field,
            "limit": top_n, "outputFields": output_fields}


def build_deepseek_body(model, system, user):
    return {"model": model,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "temperature": 0}


def cache_key(question, candidate_ids, version):
    raw = normalize_question(question) + "\x1f" + ",".join(sorted(candidate_ids)) + "\x1f" + version
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()


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
    # 注：sids 缺失时下面退化为不按 standard 过滤（比 spec §6.1 的 standards 表查找更宽，
    # 即多排除 gold 而非少排除，偏安全）；100 集均带 source_standard_ids，此分支实际不触发。
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
    if data.get("code", 0) != 0:   # Milvus REST 错误：响铃失败，勿静默返回空（spec §8）
        raise RuntimeError(f"Milvus search 失败: code={data.get('code')} {data.get('message')}")
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


# ---------- 编排 / review / CLI ----------
def build_review_entry(case, gold_ids, candidates, labels, reasons=None):
    return {"question": case["question"],
            "gold_brief": case.get("gold_method_no") or case.get("gold_clause_no") or "",
            "candidates": candidates,
            "labels": labels,
            "reasons": reasons or {}}


def label_of(chunk_id, distractor_ids, acceptable_ids):
    """按隶属判定候选标签（drift 无关：标签来自标注集成员关系，不依赖候选池重建）。"""
    if chunk_id in distractor_ids:
        return "distractor"
    if chunk_id in acceptable_ids:
        return "acceptable"
    return "irrelevant"


def review_entry_from_annotated(case, candidates, reasons, cache_status):
    """从已标注 case + 重建候选池 + 缓存理由，构造一条 review entry。纯函数。"""
    d = set(case.get("distractor_chunks") or [])
    a = set(case.get("acceptable_chunks") or [])
    return {
        "question": case["question"],
        "gold_brief": case.get("gold_method_no") or case.get("gold_clause_no") or "",
        "candidates": candidates,
        "labels": {"distractor": list(case.get("distractor_chunks") or []),
                   "acceptable": list(case.get("acceptable_chunks") or [])},
        "reasons": reasons or {},
        "cache_status": cache_status,
    }


def render_review(reviews):
    out = ["# 标注人审导出\n"]
    for r in reviews:
        cs = r.get("cache_status", "")
        tag = f" [{cs}]" if cs else ""
        out.append(f"## {r['question']}{tag}")
        out.append(f"- gold: {r['gold_brief']}")
        d = set(r["labels"]["distractor"])
        a = set(r["labels"]["acceptable"])
        reasons = r.get("reasons", {})
        for c in r["candidates"]:
            cid = c["chunk_id"]
            lab = "distractor" if cid in d else ("acceptable" if cid in a else "irrelevant")
            tagm = c.get("method_no") or c.get("clause_no") or "-"
            title = c.get("title", "")
            reason = " ".join(reasons.get(cid, "").split())
            snip = " ".join(c.get("snippet", "").split())[:80]
            out.append(f"  - [{lab}] {cid} [{tagm}] {title} — {reason} — {snip}")
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
    reasons = reasons_from_response(resp, cand_ids) if resp is not None else {}
    status = "auto"
    if labels is None:
        labels = {"distractor": [], "acceptable": []}
        status = "generation_error"
    meta = {"generator_version": GENERATOR_VERSION, "candidate_top_n": top_n,
            "validation_status": status}
    return (assemble_annotated_case(case, labels, meta),
            build_review_entry(case, gold_ids, candidates, labels, reasons))


def rebuild_review_for_case(case, conn, cfg, cache_dir, top_n, allow_llm):
    """为已标注 case 重建 review：复用候选逻辑(embed+Milvus)→候选池；
    优先按 cache_key 读 data/annotation_cache 取理由；缓存缺失标 cache_missing，
    不编造理由；allow_llm 时才补调 DeepSeek。"""
    question = case["question"]
    gold_ids = resolve_gold_chunks(conn, case)
    vec = embed_text(cfg, question)
    hits = milvus_search(cfg, vec, top_n + len(gold_ids) + 10)
    pool = build_candidate_pool(hits, gold_ids, top_n)
    ctx = fetch_chunk_context(conn, pool)
    candidates = [ctx[c] for c in pool if c in ctx]
    cand_ids = [c["chunk_id"] for c in candidates]

    key = cache_key(question, cand_ids, GENERATOR_VERSION)
    resp = read_cache(cache_dir, key)
    cache_status = "hit"
    if resp is None:
        if allow_llm:
            gold_ctx = fetch_chunk_context(conn, gold_ids)
            user = build_user_message(question, build_gold_brief(case, gold_ctx), candidates)
            resp = llm_call_with_retry(cfg, SYSTEM_PROMPT, user, cand_ids)
            if resp is not None:
                write_cache(cache_dir, key, resp)
                cache_status = "llm_backfill"
            else:
                cache_status = "cache_missing"
        else:
            cache_status = "cache_missing"
    reasons = reasons_from_response(resp, cand_ids) if resp is not None else {}
    return review_entry_from_annotated(case, candidates, reasons, cache_status)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="证据标注挖掘 (distractor/acceptable)")
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", default="")
    ap.add_argument("--top-n", type=int, default=30)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--review", default="")
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--review-only", action="store_true",
                    help="只从 --in 的标注文件重建 review，不改标注 json")
    ap.add_argument("--allow-llm", action="store_true",
                    help="review 重建时缓存缺失允许补调 DeepSeek（默认只读缓存）")
    args = ap.parse_args(argv)

    cfg = load_config(args.config)

    if args.review_only:
        with open(args.inp, encoding="utf-8") as f:
            cases = load_cases(f.read())
        if args.limit:
            cases = cases[:args.limit]
        conn = pg_connect(cfg["RAG_PG_CONNINFO"])
        cache_dir = "data/annotation_cache"
        reviews = []
        for case in cases:
            reviews.append(rebuild_review_for_case(case, conn, cfg, cache_dir, args.top_n, args.allow_llm))
            sys.stderr.write(f"[review] {case.get('case_id') or case['question'][:20]}: {reviews[-1]['cache_status']}\n")
        out_review = args.review or (args.inp.rsplit('.', 1)[0] + ".review.md")
        with open(out_review, "w", encoding="utf-8") as f:
            f.write(render_review(reviews))
        sys.stderr.write(f"[review] 完成 {len(reviews)} 题 → {out_review}\n")
        return

    if not args.out:
        sys.stderr.write("[annot] 错误：非 --review-only 模式需要 --out\n")
        sys.exit(1)

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

    # 收尾必写：循环内的增量写只在“处理了某题”后触发，若末尾若干题是 resume 复用
    # （在 done 里）则不会落盘 → 文件会缺这几题。这里无条件全量落盘兜底。
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out_cases, f, ensure_ascii=False, indent=1)

    if args.review and reviews:
        with open(args.review, "w", encoding="utf-8") as f:
            f.write(render_review(reviews))
    sys.stderr.write(f"[annot] 完成 {len(out_cases)} 题，输出 {args.out}\n")


if __name__ == "__main__":
    main()
