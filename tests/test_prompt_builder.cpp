#include <doctest/doctest.h>
#include "generate/prompt_builder.h"

TEST_CASE("build_system_prompt enforces sourcing and refusal rules") {
    std::string sys = build_system_prompt();
    CHECK(sys.find("标准号") != std::string::npos);
    CHECK(sys.find("条款号") != std::string::npos);
    CHECK(sys.find("拒答") != std::string::npos);
}

TEST_CASE("build_user_prompt embeds question and JSON context fragments") {
    ContextFragment f;
    f.source_id = "S1";
    f.standard_no = "JTG D60-2015";
    f.clause_no = "4.2.1";
    f.status = "现行";
    f.text = "桥涵设计应符合规定。";
    std::string user = build_user_prompt("桥涵设计有什么要求？", {f});
    CHECK(user.find("桥涵设计有什么要求？") != std::string::npos);
    CHECK(user.find("S1") != std::string::npos);
    CHECK(user.find("4.2.1") != std::string::npos);
    CHECK(user.find("JTG D60-2015") != std::string::npos);
}

TEST_CASE("build_user_prompt with empty context still asks model to refuse") {
    std::string user = build_user_prompt("不在库的问题", {});
    CHECK(user.find("不在库的问题") != std::string::npos);
}
