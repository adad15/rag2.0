#include <doctest/doctest.h>
#include "generate/context.h"
#include <nlohmann/json.hpp>

TEST_CASE("ContextFragment serializes to the §11.3 schema") {
    ContextFragment f;
    f.source_id = "S1";
    f.standard_no = "JTG D60-2015";
    f.standard_name = "公路桥涵设计通用规范";
    f.status = "现行";
    f.clause_no = "4.2.1";
    f.path = "第4章 / 4.2 xxxx / 4.2.1";
    f.is_mandatory = true;
    f.text = "条款原文……";
    auto j = to_json(f);
    CHECK(j["source_id"] == "S1");
    CHECK(j["standard_no"] == "JTG D60-2015");
    CHECK(j["status"] == "现行");
    CHECK(j["clause_no"] == "4.2.1");
    CHECK(j["is_mandatory"] == true);
    CHECK(j.contains("tables"));
    CHECK(j.contains("formulas"));
}
