// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Rgba parsing and formatting (04 section 5.1, 06 section 16).
#include <esses/project/rgba.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace esses;

TEST_CASE("rgba: parse and format", "[unit]") {
    CHECK(project::parseRgba("#ff3b30") == project::rgba(0xFF, 0x3B, 0x30));
    CHECK(project::parseRgba("#FF3B3080") == project::rgba(0xFF, 0x3B, 0x30, 0x80));
    CHECK_FALSE(project::parseRgba("ff3b30").has_value());
    CHECK_FALSE(project::parseRgba("#ff3b3").has_value());
    CHECK_FALSE(project::parseRgba("#ff3b3g").has_value());
    CHECK(project::formatRgba(project::rgba(0xFF, 0x3B, 0x30)) == "#ff3b30");
    CHECK(project::formatRgba(project::rgba(0x01, 0x02, 0x03, 0x04)) == "#01020304");
    constexpr project::Rgba c = project::rgba(1, 2, 3, 4);
    STATIC_REQUIRE(project::red(c) == 1);
    STATIC_REQUIRE(project::green(c) == 2);
    STATIC_REQUIRE(project::blue(c) == 3);
    STATIC_REQUIRE(project::alpha(c) == 4);
}
