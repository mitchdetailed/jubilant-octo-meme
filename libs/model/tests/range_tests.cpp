// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Ranges, the range-group tree with per-path versions (D-140) and RangeRef identity (02 section 4.4).
#include "model_test_support.hpp"

#include <esses/model/range.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>
#include <vector>

using namespace esses;
using namespace esses::model;

namespace {

Range lap(int n, Micros start, Micros end) {
    Range r;
    r.startMarker = markerIdLogged(MarkerClass::BCN, start, "Beacons", 0);
    r.id = rangeId("Outings:Laps", r.startMarker);
    r.kind = RangeKind::Lap;
    r.lapClass = LapClass::Flying;
    r.start = start;
    r.end = end;
    r.number = n;
    r.name = "Lap " + std::to_string(n);
    r.abbrev = "L" + std::to_string(n);
    return r;
}

RangeGroupTree sampleTree() {
    RangeGroupTree t;
    RangeGroup outings = RangeGroup::make("Outings");
    Range o;
    o.kind = RangeKind::Outing;
    o.end = 10'000;
    outings.ranges.push_back(o);
    CHECK(t.setGroup(outings));
    RangeGroup laps = RangeGroup::make("Outings:Laps");
    laps.ranges = {lap(1, 1'000, 2'000), lap(2, 2'000, 3'000)};
    CHECK(t.setGroup(laps));
    CHECK(t.setGroup(RangeGroup::make("Outings:Laps:Track Sections:Default")) == false); // parent missing
    CHECK(t.setGroup(RangeGroup::make("Outings:Laps:Track Sections")));
    CHECK(t.setGroup(RangeGroup::make("Outings:Laps:Track Sections:Default")));
    return t;
}

} // namespace

TEST_CASE("range: kind and lap-class names round-trip", "[unit]") {
    for (const RangeKind k : {RangeKind::Outing, RangeKind::Lap, RangeKind::Run, RangeKind::Stage,
                              RangeKind::Section, RangeKind::Split, RangeKind::Data}) {
        CHECK(parseRangeKind(rangeKindName(k)) == k);
    }
    for (const LapClass c :
         {LapClass::Flying, LapClass::Out, LapClass::In, LapClass::Partial, LapClass::Pit, LapClass::None}) {
        CHECK(parseLapClass(lapClassName(c)) == c);
    }
    CHECK_FALSE(parseRangeKind("lap").has_value());
}

TEST_CASE("range: defaults and duration", "[unit]") {
    const Range r = lap(3, 1'000, 3'500);
    CHECK(r.duration() == 2'500);
    CHECK(r.trusted);
    CHECK(r.enabled);
    CHECK(r.stint == 0);
    CHECK_FALSE(r.pitStop);
    CHECK(r.offsetUs == 0);
    CHECK(r.offsetM == 0.0);
    CHECK_FALSE(r.stretch.has_value());
}

TEST_CASE("range: group paths", "[unit]") {
    CHECK(parentGroupPath("Outings:Laps:Track Sections:Default") == "Outings:Laps:Track Sections");
    CHECK(parentGroupPath("Outings").empty());
    CHECK(lastGroupSegment("Outings:Laps") == "Laps");
    CHECK(lastGroupSegment("Outings") == "Outings");
    const RangeGroup g = RangeGroup::make("Outings:Laps:Track Sections:Default:Corners");
    CHECK(g.name == "Corners");
    CHECK(g.supportsReporting);
    CHECK_FALSE(g.supportsTrackGeneration);
}

TEST_CASE("range: the tree finds groups by full path", "[unit]") {
    RangeGroupTree t = sampleTree();
    REQUIRE(t.find("Outings:Laps") != nullptr);
    CHECK(t.find("Outings:Laps")->ranges.size() == 2);
    CHECK(t.find("Outings:Laps:Track Sections:Default")->name == "Default");
    CHECK(t.find("Outings:Lap") == nullptr);
    CHECK(t.find("Laps") == nullptr);
    CHECK(t.rangeVersion("Nope") == 0);
    std::vector<std::string> visited;
    t.forEach([&visited](const RangeGroup& g) { visited.push_back(g.path); });
    CHECK(visited == std::vector<std::string>{"Outings", "Outings:Laps", "Outings:Laps:Track Sections",
                                              "Outings:Laps:Track Sections:Default"});
    // Replacing a group keeps its children when the replacement has none.
    RangeGroup laps = RangeGroup::make("Outings:Laps");
    CHECK(t.setGroup(laps));
    CHECK(t.find("Outings:Laps")->ranges.empty());
    CHECK(t.find("Outings:Laps:Track Sections") != nullptr);
    CHECK(t.removeGroup("Outings:Laps:Track Sections"));
    CHECK(t.find("Outings:Laps:Track Sections:Default") == nullptr);
    CHECK_FALSE(t.removeGroup("Outings:Laps:Track Sections"));
}

TEST_CASE("range: versions are kept per group path (D-140)", "[unit]") {
    RangeGroupTree first = sampleTree();
    first.inheritVersions(RangeGroupTree{});
    first.forEach([](const RangeGroup& g) { CHECK(g.version == 1); });

    // A rebuild that changes only the laps bumps only Outings:Laps.
    RangeGroupTree second = sampleTree();
    second.find("Outings:Laps")->ranges[1].trusted = false;
    second.inheritVersions(first);
    CHECK(second.rangeVersion("Outings") == 1);
    CHECK(second.rangeVersion("Outings:Laps") == 2);
    CHECK(second.rangeVersion("Outings:Laps:Track Sections:Default") == 1);

    // A track-section rebuild never bumps Outings:Laps.
    RangeGroupTree third = second;
    RangeGroup* sections = third.find("Outings:Laps:Track Sections:Default");
    Range turn;
    turn.kind = RangeKind::Section;
    turn.name = "Turn 3";
    sections->ranges.push_back(turn);
    third.inheritVersions(second);
    CHECK(third.rangeVersion("Outings:Laps") == 2);
    CHECK(third.rangeVersion("Outings:Laps:Track Sections:Default") == 2);

    // Identical rebuild: nothing moves.
    RangeGroupTree fourth = third;
    fourth.inheritVersions(third);
    CHECK(fourth == third);
}

TEST_CASE("range: RangeRef equality ignores the name and maps the empty layer to Normal", "[unit]") {
    const RangeId id = rangeId("Outings:Laps", markerIdLogged(MarkerClass::BCN, 1, "Beacons", 0));
    const RangeRef a{test::testSource(), "Normal", "Outings:Laps", id, "Lap 3"};
    const RangeRef b{test::testSource(), "", "Outings:Laps", id, "Lap 4"};
    CHECK(a == b);
    CHECK(std::hash<RangeRef>{}(a) == std::hash<RangeRef>{}(b));
    RangeRef c = a;
    c.groupPath = "Outings";
    CHECK_FALSE(a == c);
    RangeRef d = a;
    d.src = SourceId{9, 9};
    CHECK_FALSE(a == d);
    std::unordered_set<RangeRef> set{a, b, c, d};
    CHECK(set.size() == 3);
}
