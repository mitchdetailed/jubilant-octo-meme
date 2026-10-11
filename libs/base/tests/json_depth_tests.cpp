// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/json_depth.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using esses::jsonNestingDepth;
using esses::kMaxJsonDepth;

TEST_CASE("jsonNestingDepth counts arrays and objects outside strings", "[unit][json_depth]") {
    CHECK(jsonNestingDepth("") == 0);
    CHECK(jsonNestingDepth("42") == 0);
    CHECK(jsonNestingDepth("[]") == 1);
    CHECK(jsonNestingDepth(R"({"a": [1, {"b": []}]})") == 4);
    CHECK(jsonNestingDepth(R"({"a": "[[[[{{{{"})") == 1);         // brackets inside a string
    CHECK(jsonNestingDepth(R"({"a": "quote \" [[[ \\"} )") == 1); // escaped quote, then an escaped backslash
    CHECK(jsonNestingDepth("]]][[") == 2);                        // malformed text is the parser's business
}

TEST_CASE("jsonNestingDepth stops early above the limit", "[unit][json_depth]") {
    const std::string deep = std::string(100'000, '[') + std::string(100'000, ']');
    CHECK(jsonNestingDepth(deep) == kMaxJsonDepth + 1);
    CHECK(jsonNestingDepth(deep, 1'000'000) == 100'000);
    const std::string ok = std::string(kMaxJsonDepth, '[') + std::string(kMaxJsonDepth, ']');
    CHECK(jsonNestingDepth(ok) == kMaxJsonDepth);
}
