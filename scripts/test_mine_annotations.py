# -*- coding: utf-8 -*-
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


def test_build_embedding_body():
    assert m.build_embedding_body("Qwen/Qwen3-Embedding-8B", "天平") == {
        "model": "Qwen/Qwen3-Embedding-8B", "input": "天平"}


def test_build_milvus_search_body():
    body = m.build_milvus_search_body("clause_text", [0.1, 0.2], 30, ["chunk_id", "standard_id"])
    assert body["collectionName"] == "clause_text"
    assert body["data"] == [[0.1, 0.2]]
    assert body["limit"] == 30
    assert body["outputFields"] == ["chunk_id", "standard_id"]
    assert body["annsField"] == "dense"   # 双向量集必须指定，否则 Milvus code 1801


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
