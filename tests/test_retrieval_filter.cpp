#include <doctest/doctest.h>
#include "retrieve/retrieval_filter.h"

TEST_CASE("to_milvus_expr defaults to current-status filter") {
    RetrievalFilter f;   // status 默认 "现行"
    CHECK(to_milvus_expr(f) == "status == \"现行\"");
}

TEST_CASE("to_milvus_expr conjoins status and standard_id when both set") {
    RetrievalFilter f;
    f.standard_id = "12215131224082667446";
    CHECK(to_milvus_expr(f) ==
          "status == \"现行\" and standard_id == \"12215131224082667446\"");
}

TEST_CASE("to_milvus_expr returns empty when both status and standard_id are empty") {
    RetrievalFilter f;
    f.status = "";
    CHECK(to_milvus_expr(f).empty());
}

TEST_CASE("to_milvus_expr with only standard_id and empty status") {
    RetrievalFilter f;
    f.status = "";
    f.standard_id = "S1";
    CHECK(to_milvus_expr(f) == "standard_id == \"S1\"");
}
