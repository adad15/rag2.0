#include <doctest/doctest.h>
#include "eval/generation_metrics.h"

TEST_CASE("normalize_for_match strips whitespace, folds fullwidth digits, lowercases ascii") {
    CHECK(normalize_for_match("JTG 3420") == "jtg3420");
    CHECK(normalize_for_match("１００") == "100");        // 全角数字
    CHECK(normalize_for_match(" T0521 \n") == "t0521");
}

TEST_CASE("citation_hit needs both standard code and ref present") {
    std::string ans = "依据《公路工程水泥及水泥混凝土试验规程》（JTG 3420-2020）第 5.1.2 条……";
    CHECK(citation_hit(ans, "JTG 3420-2020", "5.1.2"));     // 双含
    CHECK_FALSE(citation_hit(ans, "JTG 3420-2020", "9.9.9")); // 缺条款号
    CHECK_FALSE(citation_hit(ans, "JTG 9999", "5.1.2"));      // 缺标准号
}

TEST_CASE("citation_hit with empty standard code checks only the ref") {
    std::string ans = "T0521 水泥混凝土拌合物试验需要……";
    CHECK(citation_hit(ans, "", "T0521"));                   // 无标准号要求，只验方法号
    CHECK_FALSE(citation_hit(ans, "", "T9999"));
}

TEST_CASE("citation_hit empty ref is never a hit") {
    CHECK_FALSE(citation_hit("任意答案", "JTG 3420", ""));
}

TEST_CASE("count_value_hits counts normalized substring matches") {
    std::string ans = "压力机示值精度不低于 1%，最大荷载 1000 kN。";
    CHECK(count_value_hits(ans, {"1%", "1000"}) == 2);       // 全中
    CHECK(count_value_hits(ans, {"1%", "2000"}) == 1);       // 部分中
    CHECK(count_value_hits(ans, {"１０００"}) == 1);          // 全角等价（归一化后 1000）
    CHECK(count_value_hits(ans, {}) == 0);                   // 空 gold
}
