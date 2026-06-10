#include <doctest/doctest.h>
#include "structure/clause_tree.h"

TEST_CASE("clause_tree json round trip preserves fields") {
    ClauseTree t;
    t.standard_id = "sid1";
    t.standard_no = "JTC 5210-2018";
    t.format_profile = "A_decimal";

    TreeNode a;
    a.node_id = "sid1:5";
    a.level = 1;
    a.number = "5";
    a.title = "classification";
    a.child_ids = {"sid1:5/5.1"};

    TreeNode b;
    b.node_id = "sid1:5/5.1";
    b.level = 2;
    b.number = "5.1";
    b.title = "subgrade";
    b.parent_id = "sid1:5";
    b.is_leaf = true;
    b.text = "body";
    b.page_start = 12;
    b.page_end = 13;
    b.has_table = true;
    b.has_formula = true;
    b.has_figure = true;
    b.captions = {"figure 5.1"};
    b.table_htmls = {"<table><tr><td>body</td></tr></table>"};
    b.formulas = {"MQI = SCI + PQI"};
    b.suspect = "seq";

    t.nodes = {a, b};
    t.page_clause_map[12] = {"sid1:5/5.1"};

    ClauseTree r = clause_tree_from_json(clause_tree_to_json(t));

    REQUIRE(r.nodes.size() == 2);
    CHECK(r.standard_id == "sid1");
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.format_profile == "A_decimal");
    CHECK(r.nodes[0].child_ids.size() == 1);
    CHECK(r.nodes[1].is_leaf);
    CHECK(r.nodes[1].has_table);
    CHECK(r.nodes[1].has_formula);
    CHECK(r.nodes[1].has_figure);
    REQUIRE(r.nodes[1].captions.size() == 1);
    CHECK(r.nodes[1].captions[0] == "figure 5.1");
    REQUIRE(r.nodes[1].table_htmls.size() == 1);
    CHECK(r.nodes[1].table_htmls[0].find("<table>") != std::string::npos);
    REQUIRE(r.nodes[1].formulas.size() == 1);
    CHECK(r.nodes[1].formulas[0] == "MQI = SCI + PQI");
    CHECK(r.nodes[1].suspect == "seq");
    CHECK(r.nodes[1].page_start == 12);
    CHECK(r.nodes[1].page_end == 13);
    CHECK(r.page_clause_map.at(12).size() == 1);
}
