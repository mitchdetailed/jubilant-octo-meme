// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/utf8.hpp>

#include <catch2/catch_test_macros.hpp>

namespace utf8 = esses::utf8;

TEST_CASE("utf8: strict validation", "[unit][utf8]") {
    CHECK(utf8::isValid(""));
    CHECK(utf8::isValid("plain ASCII"));
    CHECK(utf8::isValid("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8F\x81")); // e-acute, euro, flag
    CHECK(utf8::isValid("\xEF\xBF\xBD"));                         // a literal U+FFFD is valid
    CHECK_FALSE(utf8::isValid("\xC0\xAF"));                       // overlong '/'
    CHECK_FALSE(utf8::isValid("\xE0\x80\xAF"));                   // overlong
    CHECK_FALSE(utf8::isValid("\xED\xA0\x80"));                   // surrogate D800
    CHECK_FALSE(utf8::isValid("\xF4\x90\x80\x80"));               // > U+10FFFF
    CHECK_FALSE(utf8::isValid("\xC3"));                           // truncated
    CHECK_FALSE(utf8::isValid("\x80"));                           // lone continuation
    CHECK_FALSE(utf8::isValid("\xFF"));
}

TEST_CASE("utf8: decode, encode and sanitize", "[unit][utf8]") {
    std::size_t pos = 0;
    const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8F\x81";
    CHECK(utf8::decodeNext(s, pos) == U'a');
    CHECK(utf8::decodeNext(s, pos) == char32_t{0xE9});
    CHECK(utf8::decodeNext(s, pos) == char32_t{0x20AC});
    CHECK(utf8::decodeNext(s, pos) == U'\U0001F3C1');
    CHECK(pos == s.size());
    CHECK(utf8::countCodePoints(s) == 4u);
    CHECK(utf8::encode(char32_t{0x20AC}) == "\xE2\x82\xAC");
    CHECK(utf8::encode(0xD800) == "\xEF\xBF\xBD");
    CHECK(utf8::encode(0x110000) == "\xEF\xBF\xBD");
    CHECK(utf8::sanitize("ok\xE2\x82"
                         "x") == "ok\xEF\xBF\xBD"
                                 "x"); // maximal invalid prefix -> one U+FFFD
    CHECK(utf8::sanitize("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(utf8::sanitize("fine") == "fine");
    CHECK(utf8::isAscii("abc"));
    CHECK_FALSE(utf8::isAscii("\xC3\xA9"));
}

TEST_CASE("utf8: UTF-16 and UTF-32 round trips", "[unit][utf8]") {
    const std::string s = "M\xC3\xBCller \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8F\x81";
    CHECK(utf8::fromUtf16(utf8::toUtf16(s)) == s);
    CHECK(utf8::fromUtf32(utf8::toUtf32(s)) == s);
    CHECK(utf8::toUtf16("\xF0\x9F\x8F\x81") == std::u16string{0xD83C, 0xDFC1});
    CHECK(utf8::fromUtf16(std::u16string{0xD83C}) == "\xEF\xBF\xBD"); // unpaired surrogate
#if defined(ESSES_OS_WINDOWS)
    CHECK(utf8::fromWide(utf8::toWide(s)) == s);
#endif
}

TEST_CASE("utf8: paths round-trip through UTF-8", "[unit][utf8]") {
    const std::string name = "logs/M\xC3\xBCller/\xE6\x97\xA5\xE6\x9C\xAC.ld";
    const std::filesystem::path p = esses::pathFromUtf8(name);
    CHECK(esses::pathToUtf8(p) == esses::pathToUtf8(esses::pathFromUtf8(esses::pathToUtf8(p))));
    CHECK(esses::pathToUtf8(p.filename()) == "\xE6\x97\xA5\xE6\x9C\xAC.ld");
    CHECK(esses::pathToUtf8(p.parent_path().filename()) == "M\xC3\xBCller");
}
