#include <doctest/doctest.h>
#include "parse/method_no.h"

TEST_CASE("extract_method_no keeps full method number with year") {
    CHECK(extract_method_no("T 0302-2024 集料筛分试验") == "T0302-2024");
    CHECK(extract_method_no("见 T0501-2005 水泥取样方法") == "T0501-2005");
}

TEST_CASE("extract_method_no accepts year-less codes from queries") {
    CHECK(extract_method_no("T0302 需要哪些仪具") == "T0302");
    CHECK(extract_method_no("T 0604") == "T0604");
}

TEST_CASE("extract_method_no normalizes full-width dash") {
    CHECK(extract_method_no("T0302\xE2\x80\x94" "2024") == "T0302-2024"); // em dash
}

TEST_CASE("extract_method_no returns empty when no method number") {
    CHECK(extract_method_no("路基沉降怎么评定").empty());
    CHECK(extract_method_no("第5.1.2条").empty());
}
