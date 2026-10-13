// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretch::factor, the D-016 rule (06 section 13.3 item 1: blank outside the tolerance). The stretch
// applied to a DistanceIndex (LapStretch::apply) is tested by its owner.

#include <esses/analysis/lap_stretch.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using esses::analysis::LapStretch;
using esses::analysis::LapStretchRow;

TEST_CASE("lap stretch: the factor within and outside the tolerance", "[unit]") {
    CHECK(LapStretch::factor(6000.0, 6213.0, LapStretch::kDefaultTolerance) ==
          Catch::Approx(6213.0 / 6000.0));
    CHECK(LapStretch::factor(6213.0, 6213.0, 0.05) == 1.0);
    CHECK(LapStretch::factor(5000.0, 6213.0, 0.05) == 1.0); // 19.5 % short: not stretched
    CHECK(LapStretch::factor(0.0, 6213.0, 0.05) == 1.0);
    CHECK(LapStretch::factor(6000.0, 0.0, 0.05) == 1.0);
    CHECK(LapStretch::factor(6000.0, 6213.0, -1.0) == 1.0);
    LapStretchRow row;
    row.stretch = 1.025;
    CHECK(row.percent() == Catch::Approx(2.5));
}
