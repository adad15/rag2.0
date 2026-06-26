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
