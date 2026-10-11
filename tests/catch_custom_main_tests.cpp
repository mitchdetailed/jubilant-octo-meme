// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses_add_test(... CUSTOM_MAIN) links esses::catch2_nomain: the test executable brings its own main(), for
// example to set up process-wide state before Catch2 runs. On Windows such a main() receives its arguments in
// the ANSI code page, so test names in an executable with a custom main() should stay ASCII.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

bool gCustomMainRan = false;

} // namespace

TEST_CASE("the executable's own main() runs the Catch2 session", "[unit]") {
    CHECK(gCustomMainRan);
}

int main(int argc, char* argv[]) {
    gCustomMainRan = true;
    return Catch::Session().run(argc, argv);
}
