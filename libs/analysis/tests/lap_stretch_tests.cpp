// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretch applied to a lap's DistanceIndex and the per-lap report (D-016, D-140; 06 section 13.3 item 1):
// the stretch is a scalar in the index key that scales nothing inside the index, the toolbar toggle turns it
// off without hiding the report's tolerance test, and laps without an index or a track length get no stretch.

#include "analysis_test_support.hpp"

#include <esses/analysis/lap_stretch.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using Catch::Approx;

namespace {

// A lap [0, 100 s) over which Corr Dist rises linearly to `lengthM`.
DistanceIndex lapOf(double lengthM, std::uint64_t corrDistVersion = 7) {
    const std::array<Micros, 3> t{0, 50'000'000, 100'000'000};
    const std::array<double, 3> d{0.0, lengthM / 2.0, lengthM};
    DistanceIndexKey key;
    key.corrDistVersion = corrDistVersion;
    key.start = 0;
    key.end = 100'000'000;
    auto index = DistanceIndex::fromSamples(t, d, key);
    REQUIRE(index.ok());
    return std::move(index).value();
}

model::RangeId lapId(int n) {
    return model::RangeId{uuidFromHash(hash128({"lap", std::to_string(n)}))};
}

} // namespace

TEST_CASE("lap stretch: apply sets the index's stretch and leaves its tables unstretched", "[unit]") {
    DistanceIndex index = lapOf(6000.0);
    const double distBefore = index.distAt(25'000'000);
    const double timeBefore = index.timeAt(4500.0);
    const LapStretchRow row = LapStretch::apply(index, lapId(3), 6213.0, LapStretch::kDefaultTolerance);
    CHECK(row.lap == lapId(3));
    CHECK(row.lengthM == Approx(6000.0));
    CHECK(row.trackLengthM == 6213.0);
    CHECK(row.applied);
    CHECK(row.withinTolerance);
    CHECK(row.stretch == Approx(6213.0 / 6000.0));
    CHECK(row.candidate == row.stretch);
    CHECK(row.percent() == Approx(3.55).margin(1e-9));
    CHECK(index.stretch() == row.stretch);
    CHECK(index.key().stretch == row.stretch); // part of the key (D-140)
    CHECK(index.distAt(25'000'000) == distBefore);
    CHECK(index.timeAt(4500.0) == timeBefore);

    // Outside the tolerance the stretch goes back to 1.
    const LapStretchRow outside = LapStretch::apply(index, lapId(3), 7000.0, 0.05);
    CHECK_FALSE(outside.applied);
    CHECK_FALSE(outside.withinTolerance);
    CHECK(outside.stretch == 1.0);
    CHECK(outside.candidate == 1.0);
    CHECK(index.stretch() == 1.0);
}

TEST_CASE("lap stretch: the toolbar toggle off keeps the report's tolerance test", "[unit]") {
    DistanceIndex index = lapOf(5900.0);
    const LapStretchSettings off{false, 0.05};
    const LapStretchRow row = LapStretch::apply(index, lapId(1), 6000.0, off);
    CHECK_FALSE(row.applied);
    CHECK(row.withinTolerance);
    CHECK(row.candidate == Approx(6000.0 / 5900.0));
    CHECK(row.stretch == 1.0);
    CHECK(row.percent() == 0.0);
    CHECK(index.stretch() == 1.0);
    // A wider tolerance (options.lapStretch.tolerancePct = 20) takes in a lap 15 % short.
    DistanceIndex shortLap = lapOf(5100.0);
    CHECK(LapStretch::apply(shortLap, lapId(2), 6000.0, LapStretchSettings{true, 0.20}).applied);
    CHECK(shortLap.stretch() == Approx(6000.0 / 5100.0));
}

TEST_CASE("lap stretch: the per-lap report in lap order", "[unit]") {
    const DistanceIndex l1 = lapOf(6150.0);
    const DistanceIndex l2 = lapOf(4000.0); // a pit lap cut short: outside the tolerance
    const std::vector<std::pair<model::RangeId, const DistanceIndex*>> laps = {
        {lapId(1), &l1}, {lapId(2), &l2}, {lapId(3), nullptr}};
    const std::vector<LapStretchRow> rows = LapStretch::report(laps, 6213.0, LapStretchSettings{});
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].lap == lapId(1));
    CHECK(rows[0].applied);
    CHECK(rows[0].stretch == Approx(6213.0 / 6150.0));
    CHECK_FALSE(rows[1].applied);
    CHECK(rows[1].lengthM == Approx(4000.0));
    CHECK(rows[2].lengthM == 0.0); // no index: no stretch
    CHECK_FALSE(rows[2].withinTolerance);
    CHECK(l1.stretch() == 1.0); // the report changes nothing

    // Without a track length (no track map, no Venue Length) nothing is stretched.
    for (const LapStretchRow& r : LapStretch::report(laps, 0.0, LapStretchSettings{})) {
        CHECK_FALSE(r.withinTolerance);
        CHECK(r.trackLengthM == 0.0);
        CHECK(r.stretch == 1.0);
    }
}
