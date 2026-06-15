#include <doctest/doctest.h>
#include "util/text_utf8.h"

TEST_CASE("text_utf8 truncate leaves a short ascii string unchanged") {
    CHECK(text_utf8::truncate("hello", 10) == "hello");
}

TEST_CASE("text_utf8 truncate at exactly max_chars adds no ellipsis") {
    CHECK(text_utf8::truncate("hello", 5) == "hello");
}

TEST_CASE("text_utf8 truncate cuts ascii and appends an ellipsis") {
    CHECK(text_utf8::truncate("hello world", 5) == "hello\xE2\x80\xA6");
}

TEST_CASE("text_utf8 truncate counts chinese as one char each and never splits a byte") {
    std::string out = text_utf8::truncate("\xE5\xA4\xA9\xE5\xB9\xB3\xE7\xA0\x9D\xE7\xA0\x81", 2);
    CHECK(out == "\xE5\xA4\xA9\xE5\xB9\xB3\xE2\x80\xA6");
    for (size_t i = 0; i < out.size();) {
        unsigned char b = static_cast<unsigned char>(out[i]);
        CHECK((b & 0xC0) != 0x80);
        if (b < 0x80) i += 1;
        else if ((b >> 5) == 0x6) i += 2;
        else if ((b >> 4) == 0xE) i += 3;
        else i += 4;
    }
}

TEST_CASE("text_utf8 truncate returns empty for empty input") {
    CHECK(text_utf8::truncate("", 5).empty());
}
