// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Hidden measurements of the non-elementwise kernels on long extents (07 section 12; AC-M2-10 records
// filter_lp order -2 on 21.6 M samples, its <= 150 ms gate is v1.0): `esses_maths_tests "[.kernel-bench]"`
// in a release build prints the median of 3 evaluateWhole runs per expression over 21.6 M samples (6 h at
// 1 kHz; `'X' * 1` is the gather and the result alone). No timing is asserted (the gate is bench/maths_bench
// on the reference machine, D-042).

#include "kernel_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

TEST_CASE("kernel budget: 21.6 M samples through the windowed, IIR and stateful kernels (measurement)",
          "[.kernel-bench]") {
    const std::size_t n = 21'600'000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = std::sin(static_cast<double>(i) * 0.001) * 50.0 + static_cast<double>(i % 97) * 0.01;
    }
    KernelFixture f;
    f.add("X", x, Rate::fromHz(1000));
    // 200 laps of 108 s over the 6 h extent.
    f.ranges().setGroup(model::RangeGroup::make("Outings"));
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    for (int k = 0; k < 200; ++k) {
        model::Range r;
        r.id = model::rangeId("Outings:Laps", model::MarkerId{}, std::to_string(k));
        r.kind = model::RangeKind::Lap;
        r.start = Micros{108'000'000} * k;
        r.end = r.start + 108'000'000;
        laps.ranges.push_back(r);
    }
    f.ranges().setGroup(laps);
    for (const char* expr : {"filter_lp('X', 5, -2)", "filter_cheby_lp('X', 5, -8, 0.5)", "smooth('X', 0.5)",
                             "derivative('X', 11)", "integrate('X', 'X' > 0)", "stat_mean('X')",
                             "range_change(\"Outings:Laps\")", "'X' * 1"}) {
        std::vector<double> runs;
        for (int r = 0; r < 3; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            KernelFixture::Outcome o = f.eval(expr);
            const auto t1 = std::chrono::steady_clock::now();
            REQUIRE(o.signal.ok());
            REQUIRE(o.signal->values.size() == n);
            runs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(runs.begin(), runs.end());
        std::printf("kernel-bench %-36s median %8.1f ms (21.6 M samples, gather and result included)\n", expr,
                    runs[1]);
    }
}
