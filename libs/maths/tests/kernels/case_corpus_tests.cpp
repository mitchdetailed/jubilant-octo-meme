// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-02: every maths case file of tests/maths/cases passes (case_runner.hpp), and the corpus holds at
// least 150 of them. The expected values come from tools/maths_ref.py (D-041); `python -I tools/maths_ref.py
// --check tests/maths/cases` confirms the committed files are what the reference generates.

#include "case_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace esses::maths::test;

TEST_CASE("maths cases: the corpus holds at least 150 case files with unique ids", "[unit][cases]") {
    const std::vector<std::filesystem::path> files = caseFiles();
    CHECK(files.size() >= 150);
    std::set<std::string> stems;
    for (const std::filesystem::path& f : files) {
        const std::string name = f.filename().string();
        CHECK(stems.insert(name.substr(0, name.size() - std::string(".case.json").size())).second);
    }
}

TEST_CASE("maths cases: every tests/maths/cases/*.case.json passes", "[unit][cases]") {
    const std::vector<std::filesystem::path> files = caseFiles();
    REQUIRE_FALSE(files.empty());
    std::size_t passed = 0;
    std::size_t samples = 0;
    std::string report;
    for (const std::filesystem::path& f : files) {
        const CaseOutcome o = runCase(f);
        samples += o.samples;
        if (o.passed) {
            ++passed;
            continue;
        }
        report += o.id + ":\n";
        for (const std::string& p : o.problems) {
            report += "    " + p + "\n";
        }
    }
    // WARN is printed on every run, so the CI step "Maths case corpus" shows the count (AC-M2-02).
    WARN(passed << " of " << files.size() << " maths cases passed (" << samples << " samples compared)");
    INFO(report);
    CHECK(passed == files.size());
}
