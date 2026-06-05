#include <doctest/doctest.h>
#include "parse/clause_no.h"

TEST_CASE("parse_clause_no: 多级号（宽松）") {
    auto r = parse_clause_no("5.2.1龟裂应按面积计算", false);
    CHECK(r.matched);
    CHECK(r.clause_no == "5.2.1");
    CHECK(r.rest == "龟裂应按面积计算");

    CHECK(parse_clause_no("2.0.1公路技术状况指数", false).clause_no == "2.0.1");
    CHECK(parse_clause_no("5.2.10泛油", false).clause_no == "5.2.10");
    CHECK(parse_clause_no("6.3.10路面结构强度", false).clause_no == "6.3.10");
    auto bare = parse_clause_no("4.2.1-1", false);
    CHECK(bare.matched);
    CHECK(bare.clause_no == "4.2.1-1");
    CHECK(bare.rest.empty());                                       // 纯条款号，rest 为空
    CHECK(parse_clause_no("5.2沥青路面", false).clause_no == "5.2");
    // 多级号优先于单级章号：即便 is_heading=true，也不会把 5.2.1 误判成 5
    CHECK(parse_clause_no("5.2.1总则", true).clause_no == "5.2.1");
}

TEST_CASE("parse_clause_no: 单级号（章，仅 Heading + 后跟汉字）") {
    auto r = parse_clause_no("1总则", true);
    CHECK(r.matched);
    CHECK(r.clause_no == "1");
    CHECK(r.rest == "总则");

    CHECK(parse_clause_no("7公路技术状况评定", true).clause_no == "7");
    CHECK_FALSE(parse_clause_no("5 个试样", false).matched);
}

TEST_CASE("parse_clause_no: T 方法号（试验规程）") {
    auto r = parse_clause_no("T 0301—2024集料取样法", true);   // 全角破折号
    CHECK(r.matched);
    CHECK(r.clause_no == "T0301-2024");
    CHECK(r.rest == "集料取样法");
    CHECK(parse_clause_no("T0355-2000填料加热", true).clause_no == "T0355-2000");
    CHECK(parse_clause_no("T 0301集料取样法", true).clause_no == "T0301");  // 无年份
    CHECK_FALSE(parse_clause_no("AC 13沥青混合料", true).matched);          // 仅 2 位数字，非方法号
    CHECK_FALSE(parse_clause_no("T 0301 sampling", true).matched);          // 号后非汉字
}

TEST_CASE("parse_clause_no: 数值负例不误判") {
    CHECK_FALSE(parse_clause_no("200kN加到", false).matched);   // 无点
    CHECK_FALSE(parse_clause_no("0.5%。", false).matched);      // 顶层段为 0
    CHECK_FALSE(parse_clause_no("5.2%。", false).matched);      // rest 以 % 起，判数值
    CHECK_FALSE(parse_clause_no("3.5mm 厅", false).matched);    // rest 以单位起
}
