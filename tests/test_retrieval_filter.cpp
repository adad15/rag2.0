#include <doctest/doctest.h>
#include "retrieve/retrieval_filter.h"

TEST_CASE("to_milvus_expr returns empty string for an empty filter") {
    RetrievalFilter f;
    CHECK(to_milvus_expr(f).empty());
}

TEST_CASE("to_milvus_expr builds a standard_id predicate when set") {
    RetrievalFilter f;
    f.standard_id = "12215131224082667446";
    CHECK(to_milvus_expr(f) == "standard_id == \"12215131224082667446\"");
}
