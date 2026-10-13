// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The whole-outing rule (02 section 4.5 circuit.whole-outing) that esses-cli laps uses in M0.
#include "model_test_support.hpp"

#include <esses/model/whole_outing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::model;

namespace {

Marker intrinsic(MarkerClass cls, Micros t, std::uint32_t ordinal = 0) {
    return makeLoggedMarker(cls, t, "", "", "", 0, ordinal, Provenance::Derived);
}

std::vector<Detail> fileName(const char* name) {
    return {Detail{"File Name", std::string(name), Provenance::Derived, std::nullopt}};
}

} // namespace

TEST_CASE("whole outing: one outing from SOL to EOL", "[unit]") {
    DataLayer layer;
    layer.duration = 1'834'560'000;
    const Marker sol = intrinsic(MarkerClass::SOL, 0);
    const Marker eol = intrinsic(MarkerClass::EOL, 1'834'560'000);
    layer.markers = {sol, makeLoggedMarker(MarkerClass::BCN, 83'412'340, "Beacons", "Manual.1"),
                     makeDeviceEventMarker(3, 0, 0), eol};
    sortMarkers(layer.markers);
    const RangeGroup g = buildWholeOutingGroup(layer, fileName("bathurst_r1.ld"));
    CHECK(g.path == "Outings");
    CHECK(g.name == "Outings");
    CHECK_FALSE(g.supportsTrackGeneration);
    CHECK(g.supportsReporting);
    REQUIRE(g.ranges.size() == 1);
    const Range& r = g.ranges[0];
    CHECK(r.kind == RangeKind::Outing);
    CHECK(r.lapClass == LapClass::None);
    CHECK(r.startMarker == sol.id);
    CHECK(r.endMarker == eol.id);
    CHECK(r.start == 0);
    CHECK(r.end == 1'834'560'000);
    CHECK(r.name == "bathurst_r1.ld");
    CHECK(r.abbrev == "1");
    CHECK(r.number == 1);
    CHECK(r.trusted);
    CHECK(r.id == rangeId("Outings", sol.id));
}

TEST_CASE("whole outing: without SOL the id equals that of a synthesised SOL", "[unit]") {
    DataLayer layer;
    layer.duration = 5'000;
    layer.markers = {makeLoggedMarker(MarkerClass::BCN, 7'000, "Beacons", "late")};
    const RangeGroup g = buildWholeOutingGroup(layer, {});
    REQUIRE(g.ranges.size() == 1);
    CHECK(g.ranges[0].start == 0);
    CHECK(g.ranges[0].end == 7'000); // max(duration, last marker)
    CHECK(g.ranges[0].endMarker.isNil());
    CHECK(g.ranges[0].name == "Outing");

    DataLayer synthesised = layer;
    synthesised.markers.insert(synthesised.markers.begin(), intrinsic(MarkerClass::SOL, 0));
    CHECK(buildWholeOutingGroup(synthesised, {}).ranges[0].id == g.ranges[0].id);

    DataLayer eolOnly;
    const Marker eol = intrinsic(MarkerClass::EOL, 4'000);
    eolOnly.markers = {eol};
    const RangeGroup e = buildWholeOutingGroup(eolOnly, {});
    CHECK(e.ranges[0].end == 4'000);
    CHECK(e.ranges[0].endMarker == eol.id);
}

TEST_CASE("whole outing: several outings, restarts and disabled markers", "[unit]") {
    DataLayer layer;
    layer.duration = 100;
    Marker ignoredSol = intrinsic(MarkerClass::SOL, 5, 1);
    ignoredSol.enabled = false;
    layer.markers = {intrinsic(MarkerClass::EOL, 0),   // EOL with none open: ignored
                     intrinsic(MarkerClass::SOL, 1),   // outing 1
                     ignoredSol,                       // ignored edit: not a boundary
                     intrinsic(MarkerClass::SOL, 10),  // restart closes outing 1 at 10
                     intrinsic(MarkerClass::EOL, 40),  // closes outing 2
                     intrinsic(MarkerClass::SOL, 60)}; // open at the end: closes at the duration
    sortMarkers(layer.markers);
    WholeOutingOptions opt;
    opt.trusted = false;
    const RangeGroup g = buildWholeOutingGroup(layer, fileName("x.ld"), opt);
    REQUIRE(g.ranges.size() == 3);
    CHECK(g.ranges[0].start == 1);
    CHECK(g.ranges[0].end == 10);
    CHECK(g.ranges[0].endMarker == layer.markers[3].id);
    CHECK(g.ranges[1].start == 10);
    CHECK(g.ranges[1].end == 40);
    CHECK(g.ranges[2].start == 60);
    CHECK(g.ranges[2].end == 100);
    CHECK(g.ranges[2].endMarker.isNil());
    CHECK(g.ranges[2].abbrev == "3");
    CHECK(g.ranges[2].number == 3);
    CHECK_FALSE(g.ranges[1].trusted);
}

TEST_CASE("whole outing: an outing that ends where the next one starts", "[unit]") {
    DataLayer layer;
    layer.duration = 100;
    layer.markers = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::SOL, 50, 1),
                     intrinsic(MarkerClass::EOL, 50), intrinsic(MarkerClass::EOL, 100, 1)};
    sortMarkers(layer.markers); // EOL(50) before SOL(50)
    const RangeGroup g = buildWholeOutingGroup(layer, fileName("joint.ld"));
    REQUIRE(g.ranges.size() == 2);
    CHECK(g.ranges[0].start == 0);
    CHECK(g.ranges[0].end == 50);
    CHECK(g.ranges[1].start == 50);
    CHECK(g.ranges[1].end == 100); // not a zero-length outing at 50
}

TEST_CASE("whole outing: rebuild applies overrides and keeps versions per path", "[unit]") {
    DataSource src;
    src.layers.resize(1);
    DataLayer& layer = src.layers[0];
    layer.duration = 1'000;
    const Marker sol = intrinsic(MarkerClass::SOL, 0);
    const Marker eol = intrinsic(MarkerClass::EOL, 1'000);
    layer.markers = layer.baseMarkers = {sol, eol};
    src.details = fileName("run.ld");

    SidecarApplyReport rep = rebuildWholeOutingRanges(src);
    CHECK(rep.notes.empty());
    CHECK(layer.ranges.rangeVersion("Outings") == 1);
    CHECK(layer.ranges.find("Outings")->ranges[0].trusted);

    // Same markers: same version.
    rep = rebuildWholeOutingRanges(src);
    CHECK(layer.ranges.rangeVersion("Outings") == 1);

    // An override by name heals to the start marker and bumps the version once.
    src.sidecar.setRangeOverride(
        "Normal", RangeOverride{"Outings", std::nullopt, "run.ld", false, std::nullopt, std::nullopt});
    rep = rebuildWholeOutingRanges(src);
    CHECK_FALSE(layer.ranges.find("Outings")->ranges[0].trusted);
    CHECK(layer.ranges.rangeVersion("Outings") == 2);
    REQUIRE(rep.heals.size() == 1);
    CHECK(rep.heals[0].startMarker == sol.id);
    rep = rebuildWholeOutingRanges(src);
    CHECK(layer.ranges.rangeVersion("Outings") == 2);
}
