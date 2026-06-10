#include <doctest/doctest.h>
#include "retrieve/retrieval_chunk.h"

TEST_CASE("retrieval_chunk JSON round trip preserves cache and chunk fields") {
    RetrievalChunkCache cache;
    cache.standard_id = "sid";
    cache.standard_no = "JTC 5210-2018";

    RetrievalChunk c;
    c.chunk_id = "sid:5/5.1/5.1.2#main";
    c.node_id = "sid:5/5.1/5.1.2";
    c.standard_id = "sid";
    c.standard_no = "JTC 5210-2018";
    c.chunk_type = "body";
    c.clause_no = "5.1.2";
    c.method_no = "";
    c.title = "路基沉降";
    c.path_text = "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降";
    c.atomic_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.embedding_text = "5.1.2 路基沉降\n路基沉降应根据沉降深度评定。\n相关图表题：\n图5.1.2 路基沉降示意图";
    c.context_text = "路径：5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降\n5.1.2 路基沉降\n路基沉降应根据沉降深度评定。";
    c.captions = {"图5.1.2 路基沉降示意图"};
    c.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    c.page_start = 12;
    c.page_end = 13;
    c.has_table = true;
    c.has_formula = true;
    c.has_figure = true;
    c.suspect = "seq";
    cache.chunks.push_back(c);

    RetrievalChunkCache round_trip =
        retrieval_chunk_cache_from_json(retrieval_chunk_cache_to_json(cache));

    CHECK(round_trip.schema_version == 1);
    CHECK(round_trip.standard_id == "sid");
    CHECK(round_trip.standard_no == "JTC 5210-2018");
    REQUIRE(round_trip.chunks.size() == 1);

    const RetrievalChunk& r = round_trip.chunks[0];
    CHECK(r.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(r.node_id == "sid:5/5.1/5.1.2");
    CHECK(r.standard_id == "sid");
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.chunk_type == "body");
    CHECK(r.clause_no == "5.1.2");
    CHECK(r.method_no == "");
    CHECK(r.title == "路基沉降");
    CHECK(r.path_text.find("5.1 路基") != std::string::npos);
    CHECK(r.atomic_text.find("路基沉降") != std::string::npos);
    CHECK(r.embedding_text.find("图5.1.2") != std::string::npos);
    CHECK(r.context_text.find("路径：") != std::string::npos);
    REQUIRE(r.captions.size() == 1);
    CHECK(r.captions[0] == "图5.1.2 路基沉降示意图");
    REQUIRE(r.formulas.size() == 1);
    CHECK(r.formulas[0] == "MQI = SCI + PQI + BCI + TCI");
    CHECK(r.page_start == 12);
    CHECK(r.page_end == 13);
    CHECK(r.has_table);
    CHECK(r.has_formula);
    CHECK(r.has_figure);
    CHECK(r.suspect == "seq");
}

static TreeNode make_node(const std::string& id, const std::string& number,
                          const std::string& title, const std::string& text,
                          bool leaf, const std::string& parent = "") {
    TreeNode n;
    n.node_id = id;
    n.number = number;
    n.title = title;
    n.text = text;
    n.is_leaf = leaf;
    n.parent_id = parent;
    n.page_start = 10;
    n.page_end = 11;
    return n;
}

TEST_CASE("build_retrieval_chunk_cache creates one dense-safe chunk per leaf") {
    ClauseTree tree;
    tree.standard_id = "sid";
    tree.standard_no = "JTC 5210-2018";
    tree.format_profile = "A_decimal";

    TreeNode root = make_node("sid:5", "5", "技术状况评定", "", false);
    root.child_ids = {"sid:5/5.1"};

    TreeNode parent = make_node("sid:5/5.1", "5.1", "路基", "", false, "sid:5");
    parent.child_ids = {"sid:5/5.1/5.1.2"};

    TreeNode leaf = make_node("sid:5/5.1/5.1.2", "5.1.2", "路基沉降",
                              "路基沉降应根据沉降深度和影响范围评定。", true, "sid:5/5.1");
    leaf.captions = {"图5.1.2 路基沉降示意图"};
    leaf.formulas = {"MQI = SCI + PQI + BCI + TCI"};
    leaf.has_figure = true;
    leaf.has_formula = true;
    leaf.has_table = true;

    tree.nodes = {root, parent, leaf};

    RetrievalChunkCache cache = build_retrieval_chunk_cache(tree);

    CHECK(cache.standard_id == "sid");
    CHECK(cache.standard_no == "JTC 5210-2018");
    REQUIRE(cache.chunks.size() == 1);

    const RetrievalChunk& c = cache.chunks[0];
    CHECK(c.chunk_id == "sid:5/5.1/5.1.2#main");
    CHECK(c.node_id == "sid:5/5.1/5.1.2");
    CHECK(c.standard_id == "sid");
    CHECK(c.standard_no == "JTC 5210-2018");
    CHECK(c.chunk_type == "body");
    CHECK(c.clause_no == "5.1.2");
    CHECK(c.title == "路基沉降");
    CHECK(c.path_text == "5 技术状况评定 > 5.1 路基 > 5.1.2 路基沉降");

    CHECK(c.atomic_text.find("5.1.2 路基沉降") != std::string::npos);
    CHECK(c.atomic_text.find("路基沉降应根据沉降深度") != std::string::npos);
    CHECK(c.atomic_text.find("MQI = SCI") != std::string::npos);
    CHECK(c.atomic_text.find("图5.1.2") == std::string::npos);
    CHECK(c.atomic_text.find("JTC 5210") == std::string::npos);
    CHECK(c.atomic_text.find("技术状况评定 >") == std::string::npos);

    CHECK(c.embedding_text.find("5.1.2 路基沉降") != std::string::npos);
    CHECK(c.embedding_text.find("路基沉降应根据沉降深度") != std::string::npos);
    CHECK(c.embedding_text.find("图5.1.2 路基沉降示意图") != std::string::npos);
    CHECK(c.embedding_text.find("MQI = SCI") != std::string::npos);
    CHECK(c.embedding_text.find("JTC 5210") == std::string::npos);
    CHECK(c.embedding_text.find("技术状况评定 >") == std::string::npos);
    CHECK(c.embedding_text.find("<table") == std::string::npos);

    CHECK(c.page_start == 10);
    CHECK(c.page_end == 11);
    CHECK(c.has_table);
    CHECK(c.has_formula);
    CHECK(c.has_figure);
    REQUIRE(c.captions.size() == 1);
    CHECK(c.captions[0] == "图5.1.2 路基沉降示意图");
    REQUIRE(c.formulas.size() == 1);
    CHECK(c.formulas[0] == "MQI = SCI + PQI + BCI + TCI");
}
