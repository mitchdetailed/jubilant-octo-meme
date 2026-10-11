// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Regression tests of the test discovery (cmake/catch_discover.cmake). Every test case below must become one
// ctest entry with its exact name, and the spec ctest passes must select exactly that test case: each case
// checks that it is the only one run in its process and that Catch2 reports its own name. Run the executable
// one test case at a time, as ctest does. The hidden [.reject-*] cases have names that a Catch2 test spec
// cannot select exactly; the lint.catch_discover.reject_* tests list them and expect discovery to refuse
// them.
#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <string>

namespace {

int gCasesRun = 0;

void checkSelectedAlone(const std::string& name) {
    ++gCasesRun;
    INFO("ctest runs one test case per process; its spec must select exactly this case");
    CHECK(gCasesRun == 1);
    CHECK(Catch::getResultCapture().getCurrentTestName() == name);
}

} // namespace

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define ESSES_DISCOVERY_CASE(name)                                                                           \
    TEST_CASE(name, "[unit]") {                                                                              \
        checkSelectedAlone(name);                                                                            \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

ESSES_DISCOVERY_CASE("semicolon; inside a name")
ESSES_DISCOVERY_CASE("semicolons;; doubled and trailing;")
ESSES_DISCOVERY_CASE("comma, inside a name")
ESSES_DISCOVERY_CASE("[brackets] at the start")
ESSES_DISCOVERY_CASE("brackets [in the middle] and unbalanced ] [")
ESSES_DISCOVERY_CASE("\"quoted\" at the start and \" in the middle")
ESSES_DISCOVERY_CASE("back\\slash, \\\" and a trailing back\\")
ESSES_DISCOVERY_CASE("~tilde at the start")
ESSES_DISCOVERY_CASE("  surrounding spaces  ")
ESSES_DISCOVERY_CASE("star * in the middle")
ESSES_DISCOVERY_CASE("not exclude: at the start")
ESSES_DISCOVERY_CASE("cmake syntax ${VAR} @VAR@ $<1:genex> $ENV{PATH}")
ESSES_DISCOVERY_CASE("bracket closers ]] ]=] ]==] ]===]")
ESSES_DISCOVERY_CASE("shell characters %PATH% ^ & | < > ' `")
ESSES_DISCOVERY_CASE("non-ASCII: µs, °C, Ω, ü, 日本")
ESSES_DISCOVERY_CASE("tab\tinside")

// Names that discovery must reject (listed only through the [reject-*] specs of the lint tests).
TEST_CASE("*leading star", "[.reject-wildcard]") {
    SUCCEED();
}
TEST_CASE("trailing star*", "[.reject-wildcard]") {
    SUCCEED();
}
TEST_CASE("exclude:prefixed name", "[.reject-exclude]") {
    SUCCEED();
}
TEST_CASE("Name In Mixed Case", "[.reject-case]") {
    SUCCEED();
}
TEST_CASE("name in mixed case", "[.reject-case]") {
    SUCCEED();
}
TEST_CASE("repeated name", "[.reject-case][first]") {
    SUCCEED();
}
TEST_CASE("repeated name", "[.reject-case][second]") {
    SUCCEED();
}
TEST_CASE("line\nbreak", "[.reject-linebreak]") {
    SUCCEED();
}
