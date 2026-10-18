// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RangeTables (01 section 5.3 rule 9, 03 section 6.1): immutable per-path interval tables of a layer.
#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::MarkerClass;

TEST_CASE("range tables: intervals per group path with versions", "[unit]") {
    model::DataSource src = sourceWith(
        layerWith(sec(70), {intrinsic(MarkerClass::SOL, 0), beacon(sec(10), "B1"), beacon(sec(30), "B2"),
                            beacon(sec(55), "B3"), intrinsic(MarkerClass::EOL, sec(70))}));
    RuleRunner::circuitDefault().run(src);
    model::DataLayer& layer = src.layers[0];
    model::RangeGroup* laps = layer.ranges.find("Outings:Laps");
    REQUIRE(laps != nullptr);
    laps->ranges[2].enabled = false; // disabled ranges are left out
    laps->version = 7;

    const auto tables = RangeTables::build(layer);
    REQUIRE(tables != nullptr);
    const auto lapIntervals = tables->group("Outings:Laps");
    REQUIRE(lapIntervals.size() == 3);
    CHECK(lapIntervals[0].start == 0);
    CHECK(lapIntervals[0].end == sec(10));
    CHECK(lapIntervals[0].lapClass == model::LapClass::Out);
    CHECK_FALSE(lapIntervals[0].trusted);
    CHECK(lapIntervals[1].number == 1);
    CHECK(lapIntervals[1].id == laps->ranges[1].id);
    CHECK(lapIntervals[2].lapClass == model::LapClass::In);
    CHECK(tables->group("Outings").size() == 1);
    CHECK(tables->rangeVersion("Outings:Laps") == 7);
    CHECK(tables->rangeVersion("Outings") == 1);
    CHECK(tables->rangeVersion("Nope") == 0);
    CHECK(tables->group("Nope").empty());

    // find: [start, end), gaps where a range is disabled.
    REQUIRE(tables->find("Outings:Laps", 0) != nullptr);
    CHECK(tables->find("Outings:Laps", sec(10) - 1)->number == 0);
    CHECK(tables->find("Outings:Laps", sec(10))->number == 1);
    CHECK(tables->find("Outings:Laps", sec(40)) == nullptr); // L2 is disabled
    CHECK(tables->find("Outings:Laps", sec(55))->lapClass == model::LapClass::In);
    CHECK(tables->find("Outings:Laps", sec(70)) == nullptr);
    CHECK(tables->find("Outings:Laps", -1) == nullptr);
    CHECK(tables->find("Nope", 0) == nullptr);

    // contains/paths (M2, the maths range functions' E-RANGE-UNKNOWN): every group path, even an empty one.
    CHECK(tables->contains("Outings:Laps"));
    CHECK(tables->contains("Outings"));
    CHECK_FALSE(tables->contains("Nope"));
    CHECK_FALSE(tables->contains("outings:laps")); // exact match
    const std::vector<std::string> paths = tables->paths();
    CHECK(std::is_sorted(paths.begin(), paths.end()));
    CHECK(std::find(paths.begin(), paths.end(), "Outings:Laps") != paths.end());

    const auto empty = RangeTables::build(model::DataLayer{});
    REQUIRE(empty != nullptr);
    CHECK(empty->group("Outings").empty());
    CHECK_FALSE(empty->contains("Outings"));
    CHECK(empty->paths().empty());
}
