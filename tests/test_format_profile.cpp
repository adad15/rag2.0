#include <doctest/doctest.h>
#include "structure/format_profile.h"

TEST_CASE("decimal_depth counts decimal separators") {
    CHECK(decimal_depth("5") == 0);
    CHECK(decimal_depth("5.1") == 1);
    CHECK(decimal_depth("5.1.1") == 2);
    CHECK(decimal_depth("5.1.1-1") == 2);
    CHECK(decimal_depth("T 0306—1994") == -1);
    CHECK(decimal_depth("") == -1);
}

TEST_CASE("is_test_number recognizes method numbers") {
    CHECK(is_test_number("T 0306—1994"));
    CHECK(is_test_number("T0301-2024"));
    CHECK(is_test_number("T 0302–2024"));
    CHECK_FALSE(is_test_number("5.1.1"));
    CHECK_FALSE(is_test_number("table 4.0.1"));
}

TEST_CASE("level_for A_decimal maps decimal numbers") {
    auto A = FormatProfile::A_decimal;
    CHECK(level_for(A, "5", false) == 1);
    CHECK(level_for(A, "5.1", false) == 2);
    CHECK(level_for(A, "5.1.1", false) == 3);
    CHECK(level_for(A, "1.0.1", false) == 2);
    CHECK(level_for(A, "2.0.4", false) == 2);
    CHECK(level_for(A, "T0306-2024", false) == 0);
}

TEST_CASE("level_for B_testno maps method scope numbers") {
    auto B = FormatProfile::B_testno;
    CHECK(level_for(B, "T 0306—1994", false) == 2);
    CHECK(level_for(B, "4", false) == 1);
    CHECK(level_for(B, "2.1", false) == 2);
    CHECK(level_for(B, "2", true) == 3);
    CHECK(level_for(B, "2.1", true) == 4);
    CHECK(level_for(B, "2.1.5", true) == 5);
}

TEST_CASE("retrieval_depth uses L3 for A and B") {
    CHECK(retrieval_depth(FormatProfile::A_decimal) == 3);
    CHECK(retrieval_depth(FormatProfile::B_testno) == 3);
}
