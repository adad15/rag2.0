#include <doctest/doctest.h>
#include "structure/tree_builder.h"

static ParseElement body_node(const std::string& no, const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Body;
    e.clause_no = no;
    e.text = text;
    e.page_no = page;
    return e;
}

static ParseElement plain_node(const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Body;
    e.text = text;
    e.page_no = page;
    return e;
}

static ParseElement caption_node(const std::string& text, int page = 1) {
    ParseElement e = plain_node(text, page);
    e.is_caption = true;
    return e;
}

static ParseElement table_node(int page = 1) {
    ParseElement e;
    e.region = Region::Body;
    e.type = ElementType::Table;
    e.table_html = "<table></table>";
    e.page_no = page;
    return e;
}

static ParseElement formula_node(const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Body;
    e.type = ElementType::Formula;
    e.text = text;
    e.page_no = page;
    return e;
}

static ParseElement appendix_heading(const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Appendix;
    e.type = ElementType::Heading;
    e.title = text;
    e.text = text;
    e.raw_label = "paragraph_title";
    e.page_no = page;
    return e;
}

static ParseElement appendix_text(const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Appendix;
    e.type = ElementType::Text;
    e.text = text;
    e.page_no = page;
    return e;
}

static ParseElement appendix_caption(const std::string& text, int page = 1) {
    ParseElement e = appendix_heading(text, page);
    e.raw_label = "table_title";
    e.is_caption = true;
    return e;
}

static ParseElement appendix_table(const std::string& html, int page = 1) {
    ParseElement e;
    e.region = Region::Appendix;
    e.type = ElementType::Table;
    e.table_html = html;
    e.page_no = page;
    return e;
}

static ParseElement test_number_node(const std::string& text, int page = 1) {
    ParseElement e = plain_node(text, page);
    return e;
}

static ParseElement explanation_text(const std::string& text, int page = 1) {
    ParseElement e;
    e.region = Region::Explanation;
    e.type = ElementType::Text;
    e.text = text;
    e.page_no = page;
    return e;
}

static ParseElement front_node(const std::string& no, const std::string& text, int page = 1) {
    ParseElement e = body_node(no, text, page);
    e.region = Region::FrontMatter;
    return e;
}

static ParseElement toc_node(const std::string& no, const std::string& text, int page = 1) {
    ParseElement e = body_node(no, text, page);
    e.region = Region::Toc;
    return e;
}

static const TreeNode* find_node(const ClauseTree& t, const std::string& id) {
    for (const auto& n : t.nodes) {
        if (n.node_id == id) return &n;
    }
    return nullptr;
}

TEST_CASE("tree_builder builds A_decimal hierarchy and path ids") {
    ParsedDoc d;
    d.standard_no = "JTC 5210";
    d.elements = {
        body_node("5", "classification"),
        body_node("5.1", "subgrade"),
        body_node("5.1.1", "5.1.1 shoulder damage"),
        body_node("5.1.2", "5.1.2 slope collapse"),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    CHECK(t.standard_no == "JTC 5210");
    CHECK(t.format_profile == "A_decimal");
    const TreeNode* c5 = find_node(t, "sid:5");
    const TreeNode* c51 = find_node(t, "sid:5/5.1");
    const TreeNode* c511 = find_node(t, "sid:5/5.1/5.1.1");
    REQUIRE(c5);
    REQUIRE(c51);
    REQUIRE(c511);
    CHECK(c5->level == 1);
    CHECK(c51->level == 2);
    CHECK(c511->level == 3);
    CHECK(c51->parent_id == "sid:5");
    REQUIRE(c5->child_ids.size() == 1);
    CHECK(c5->child_ids[0] == "sid:5/5.1");
}

TEST_CASE("tree_builder folds N.0.K under the chapter") {
    ParsedDoc d;
    d.elements = {
        body_node("1", "general"),
        body_node("1.0.1", "1.0.1 purpose"),
        body_node("1.0.2", "1.0.2 scope"),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c101 = find_node(t, "sid:1/1.0.1");
    REQUIRE(c101);
    CHECK(c101->level == 2);
    CHECK(c101->parent_id == "sid:1");
}

TEST_CASE("tree_builder aggregates continuations captions and table flags") {
    ParsedDoc d;
    d.elements = {
        body_node("5.1", "subgrade"),
        body_node("5.1.2", "5.1.2 slope collapse criteria:"),
        plain_node("1 slight damage"),
        plain_node("2 moderate damage"),
        table_node(),
        caption_node("figure 5.1 sketch"),
        body_node("5.1.3", "5.1.3 washout"),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c512 = find_node(t, "sid:5/5.1/5.1.2");
    const TreeNode* c51 = find_node(t, "sid:5/5.1");
    REQUIRE(c512);
    REQUIRE(c51);
    CHECK(c512->text.find("1 slight") != std::string::npos);
    CHECK(c512->text.find("2 moderate") != std::string::npos);
    CHECK(c512->text.find("5.1.2") == std::string::npos);
    REQUIRE(c512->captions.size() == 1);
    CHECK(c512->captions[0] == "figure 5.1 sketch");
    CHECK(c512->has_table);
    CHECK(c512->is_leaf);
    CHECK_FALSE(c51->is_leaf);
}

TEST_CASE("tree_builder keeps table html and formula text as node attachments") {
    ParsedDoc d;
    d.elements = {
        body_node("7", "evaluation", 20),
        body_node("7.2.1", "7.2.1 MQI calculation", 20),
        table_node(20),
        formula_node("MQI = SCI + PQI + BCI + TCI", 20),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c721 = find_node(t, "sid:7/7.2/7.2.1");
    REQUIRE(c721);
    CHECK(c721->has_table);
    CHECK(c721->has_formula);
    REQUIRE(c721->table_htmls.size() == 1);
    CHECK(c721->table_htmls[0].find("<table>") != std::string::npos);
    REQUIRE(c721->formulas.size() == 1);
    CHECK(c721->formulas[0] == "MQI = SCI + PQI + BCI + TCI");
}

TEST_CASE("tree_builder backfills standalone formula lines from page text when elements omit them") {
    ParsedDoc d;
    d.pages.push_back({
        15,
        "7公路技术状况评定\n"
        "7.2公路技术状况（MQI）评定\n"
        "7.2.1公路技术状况应采用公路技术状况指数MQI评定。MQI应按式（7.2.1）计算：\n"
        "\\mathrm{MQI}=w_{\\mathrm{SCI}}\\mathrm{SCI}+w_{\\mathrm{PQI}}\\mathrm{PQI}+w_{\\mathrm{BCI}}\\mathrm{BCI}+w_{\\mathrm{TCI}}\\mathrm{TCI}\n"
        "式中：$w_{\\mathrm{SCI}}$ SCI在MQI中的权重。\n"
        "7.2.2其他条款正文。\n"
    });
    d.elements = {
        body_node("7", "7公路技术状况评定", 15),
        body_node("7.2", "7.2公路技术状况（MQI）评定", 15),
        body_node("7.2.1", "7.2.1公路技术状况应采用公路技术状况指数MQI评定。MQI应按式（7.2.1）计算：", 15),
        plain_node("式中：$w_{\\mathrm{SCI}}$ SCI在MQI中的权重。", 15),
        body_node("7.2.2", "7.2.2其他条款正文。", 15),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c721 = find_node(t, "sid:7/7.2/7.2.1");
    REQUIRE(c721);
    CHECK(c721->has_formula);
    REQUIRE(c721->formulas.size() == 1);
    CHECK(c721->formulas[0].find("\\mathrm{MQI}") != std::string::npos);
    CHECK(c721->text.find("\\mathrm{MQI}") != std::string::npos);
}

TEST_CASE("tree_builder treats table-prefixed caption as table even when raw label says figure") {
    ParsedDoc d;
    ParseElement cap = caption_node("表4.0.1公路技术状况等级划分标准", 11);
    cap.raw_label = "figure_title";
    d.elements = {
        body_node("4", "rating", 11),
        body_node("4.0.1", "4.0.1 grade table", 11),
        cap,
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c401 = find_node(t, "sid:4/4.0.1");
    REQUIRE(c401);
    CHECK(c401->has_table);
    CHECK_FALSE(c401->has_figure);
}

TEST_CASE("tree_builder treats figure-internal legend text as body text") {
    ParsedDoc d;
    ParseElement legend = plain_node("图中：MQI——公路技术状况指数（Highway Maintenance Quality Indicator);", 9);
    legend.is_caption = true;             // legacy parse_cache may already contain this wrong flag
    legend.raw_label = "vision_footnote";
    d.elements = {
        body_node("3", "indicator chapter", 9),
        body_node("3.0.3", "3.0.3 indicator system", 9),
        caption_node("图3.0.3公路技术状况指标体系", 9),
        legend,
        plain_node("SCI——路基技术状况指数", 9),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c303 = find_node(t, "sid:3/3.0.3");
    REQUIRE(c303);
    REQUIRE(c303->captions.size() == 1);
    CHECK(c303->captions[0] == "图3.0.3公路技术状况指标体系");
    CHECK(c303->text.find("图中：MQI") != std::string::npos);
    CHECK(c303->text.find("SCI") != std::string::npos);
}

TEST_CASE("tree_builder does not trust legacy caption flags on numbered headings") {
    ParsedDoc d;
    ParseElement bad = body_node("2.2", "符号", 11);
    bad.type = ElementType::Heading;
    bad.title = "2.2符号";
    bad.raw_label = "figure_title";
    bad.is_caption = true;
    d.elements = {
        body_node("2", "术语和符号", 8),
        bad,
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c22 = find_node(t, "sid:2/2.2");
    REQUIRE(c22);
    CHECK(c22->title == "符号");
    CHECK(c22->captions.empty());
}

TEST_CASE("tree_builder records page ranges and page clause map") {
    ParsedDoc d;
    d.elements = {
        body_node("5.1", "subgrade", 12),
        body_node("5.1.6", "5.1.6 settlement", 12),
        plain_node("continued judgment", 13),
        body_node("5.1.7", "5.1.7 drainage", 13),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* c516 = find_node(t, "sid:5/5.1/5.1.6");
    REQUIRE(c516);
    CHECK(c516->page_start == 12);
    CHECK(c516->page_end == 13);
    CHECK(t.page_clause_map.count(12) == 1);
    CHECK(t.page_clause_map.count(13) == 1);
    CHECK(t.page_clause_map.at(12).size() == 1);
    CHECK(t.page_clause_map.at(13).size() == 2);
}

TEST_CASE("tree_builder supports B_testno L3 retrieval units") {
    ParsedDoc d;
    d.elements = {
        body_node("4", "aggregate tests", 20),
        test_number_node("T 0302—2024 sieve analysis", 20),
        body_node("2", "apparatus", 20),
        body_node("2.1", "2.1 balance", 20),
        test_number_node("T 0306—1994 moisture test", 46),
        body_node("2", "apparatus", 46),
        body_node("2.1", "2.1 balance not greater than", 46),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    CHECK(t.format_profile == "B_testno");
    const TreeNode* t0302 = find_node(t, "sid:4/T0302-2024");
    const TreeNode* a = find_node(t, "sid:4/T0302-2024/2");
    const TreeNode* b = find_node(t, "sid:4/T0306-1994/2");
    REQUIRE(t0302);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(t0302->level == 2);
    CHECK(a->level == 3);
    CHECK(b->level == 3);
    CHECK(find_node(t, "sid:4/T0302-2024/2/2.1") == nullptr);
    CHECK(find_node(t, "sid:4/T0306-1994/2/2.1") == nullptr);
    CHECK(a->node_id != b->node_id);
    CHECK(a->is_leaf);
    CHECK(a->text.find("2.1 balance") != std::string::npos);
    CHECK(b->text.find("2.1 balance") != std::string::npos);
}

TEST_CASE("tree_builder keeps B_testno parent across explanation groups") {
    ParsedDoc d;
    d.elements = {
        body_node("3", "cement tests", 12),
        test_number_node("T0501—2005 sampling method", 12),
        body_node("1", "purpose", 12),
        explanation_text("条文说明", 14),
        explanation_text("本方法参照《水泥标准稠度用水量、凝结时间、安定性检验方法》（GB/T1346—2011）编制。", 14),
        test_number_node("T0502—2005 fineness test", 15),
        body_node("1", "purpose", 15),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* t0501 = find_node(t, "sid:3/T0501-2005");
    const TreeNode* t0502 = find_node(t, "sid:3/T0502-2005");
    REQUIRE(t0501);
    REQUIRE(t0502);
    CHECK(t0501->parent_id == "sid:3");
    CHECK(t0502->parent_id == "sid:3");
    CHECK(find_node(t, "sid:T0502-2005") == nullptr);
}

TEST_CASE("tree_builder does not promote referenced GB/T or DL/T numbers to B_testno methods") {
    ParsedDoc d;
    d.elements = {
        body_node("3", "cement tests", 12),
        test_number_node("T0501—2005 sampling method", 12),
        body_node("1", "purpose", 12),
        explanation_text("本方法参照《水泥标准稠度用水量、凝结时间、安定性检验方法》（GB/T1346—2011）编制。", 14),
        explanation_text("本方法参照《水工混凝土试验规程》（DL/T5150—2017）编制。", 279),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    CHECK(find_node(t, "sid:explanation:T1346-2011") == nullptr);
    CHECK(find_node(t, "sid:explanation:T5150-2017") == nullptr);
}

TEST_CASE("tree_builder ignores front matter and TOC candidates when building nodes") {
    ParsedDoc d;
    d.elements = {
        front_node("5", "．增加水泥砂浆相关试验方法10项。", 3),
        toc_node("T0501-2005", "T0501—2005水泥取样方法.....6", 4),
        toc_node("T0537-2020", "T0537—2020水泥混凝土拌合物水下抗分散性试验方法...118", 5),
        body_node("1", "1总则", 7),
        test_number_node("T0501—2005 水泥取样方法", 12),
        body_node("1", "1目的、适用范围和引用标准", 12),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    CHECK(find_node(t, "sid:5") == nullptr);
    CHECK(find_node(t, "sid:T0501-2005") == nullptr);
    CHECK(find_node(t, "sid:T0537-2020") == nullptr);
    CHECK(find_node(t, "sid:1"));
    CHECK(find_node(t, "sid:1/T0501-2005"));
}

TEST_CASE("tree_builder builds formal appendix roots and lettered appendix clauses") {
    ParsedDoc d;
    d.elements = {
        appendix_heading("附录A公路技术状况调查及评定表", 29),
        appendix_caption("表A-1路基损坏调查表", 29),
        appendix_text("调查时间：调查人员：", 29),
        appendix_table("<table><tr><td>路肩损坏</td></tr></table>", 29),
        appendix_heading("附录B 路面跳车计算方法", 36),
        appendix_text("B. 0.1路面跳车应根据路面纵断面高差确定。", 36),
        appendix_text("B.0.2路面跳车应按表B.0.2的规定划分跳车程度。", 36),
        appendix_heading("附录C 路面弯沉标准值计算方法", 37),
        appendix_text("C.0.1路面弯沉标准值应根据公路技术等级确定。", 37),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* app_a = find_node(t, "sid:appendix:A");
    const TreeNode* app_b = find_node(t, "sid:appendix:B");
    const TreeNode* b001 = find_node(t, "sid:appendix:B/B.0.1");
    const TreeNode* b002 = find_node(t, "sid:appendix:B/B.0.2");
    const TreeNode* c001 = find_node(t, "sid:appendix:C/C.0.1");
    REQUIRE(app_a);
    REQUIRE(app_b);
    REQUIRE(b001);
    REQUIRE(b002);
    REQUIRE(c001);
    CHECK(app_a->level == 1);
    CHECK(app_a->is_leaf);
    CHECK(app_a->has_table);
    REQUIRE(app_a->captions.size() == 1);
    CHECK(app_a->captions[0] == "表A-1路基损坏调查表");
    REQUIRE(app_a->table_htmls.size() == 1);
    CHECK(app_a->table_htmls[0].find("路肩损坏") != std::string::npos);
    CHECK(b001->parent_id == "sid:appendix:B");
    CHECK(b001->level == 2);
    CHECK(b001->text.find("B. 0.1") == std::string::npos);
    CHECK(b001->text.find("路面跳车") != std::string::npos);
    CHECK(c001->parent_id == "sid:appendix:C");
}

TEST_CASE("tree_builder separates explanation appendix node ids from body appendix") {
    ParsedDoc d;
    d.elements = {
        appendix_heading("附录B 路面跳车计算方法", 36),
        appendix_text("B.0.1路面跳车应根据路面纵断面高差确定。", 36),
        explanation_text("条文说明", 54),
        appendix_heading("附录B 路面跳车计算方法", 55),
        appendix_text("B.0.1本标准采用10m路面纵断面高程作为路面跳车计算依据。", 55),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* body_b = find_node(t, "sid:appendix:B");
    const TreeNode* body_b001 = find_node(t, "sid:appendix:B/B.0.1");
    const TreeNode* explanation_b = find_node(t, "sid:explanation:appendix:B");
    const TreeNode* explanation_b001 = find_node(t, "sid:explanation:appendix:B/B.0.1");
    REQUIRE(body_b);
    REQUIRE(body_b001);
    REQUIRE(explanation_b);
    REQUIRE(explanation_b001);
    CHECK(body_b->page_start == 36);
    CHECK(explanation_b->page_start == 55);
    CHECK(body_b001->parent_id == body_b->node_id);
    CHECK(explanation_b001->parent_id == explanation_b->node_id);
}

TEST_CASE("tree_builder keeps node ids unique for repeated sibling numbers") {
    ParsedDoc d;
    d.elements = {
        body_node("3", "cement tests", 12),
        test_number_node("T0535—2020 washing test", 119),
        body_node("5", "5 结果计算", 119),
        body_node("5", "5 试验报告", 119),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    std::vector<std::string> repeated_ids;
    for (const auto& n : t.nodes) {
        if (n.parent_id == "sid:3/T0535-2020" && n.number == "5") {
            repeated_ids.push_back(n.node_id);
        }
    }
    REQUIRE(repeated_ids.size() == 2);
    CHECK(repeated_ids[0] == "sid:3/T0535-2020/5");
    CHECK(repeated_ids[1] != repeated_ids[0]);
}

TEST_CASE("tree_builder creates virtual appendix parent for orphan appendix clauses") {
    ParsedDoc d;
    d.elements = {
        appendix_text("A.1技术要求", 67),
    };

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* app_a = find_node(t, "sid:appendix:A");
    const TreeNode* a1 = find_node(t, "sid:appendix:A/A.1");
    REQUIRE(app_a);
    REQUIRE(a1);
    CHECK(app_a->suspect == "gap");
    CHECK(app_a->level == 1);
    CHECK(a1->parent_id == app_a->node_id);
}

TEST_CASE("tree_builder creates virtual gap nodes and preserves suspect") {
    ParsedDoc d;
    ParseElement c5 = body_node("5", "classification");
    ParseElement c511 = body_node("5.1.1", "5.1.1 shoulder damage");
    c511.suspect = "seq";
    d.elements = {c5, c511};

    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* virt = find_node(t, "sid:5/5.1");
    const TreeNode* leaf = find_node(t, "sid:5/5.1/5.1.1");
    REQUIRE(virt);
    REQUIRE(leaf);
    CHECK(virt->number == "5.1");
    CHECK(virt->suspect == "gap");
    CHECK(virt->title.empty());
    CHECK(leaf->suspect == "seq");
    CHECK(leaf->parent_id == "sid:5/5.1");
}
