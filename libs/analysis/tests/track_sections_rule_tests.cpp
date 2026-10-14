// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackSectionsRule (02 section 4.5, 04 section 13 item 5, section 14 item 5; D-066, D-140): the rule-1
// document, the origin rule, the Track Sections groups of a hand-computed outing (an out lap measured
// backwards from its end beacon, a flying lap, a stretched lap, an in lap that stops short) and the phase-2
// pass of RuleRunner with its version rules.
#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::MarkerClass;

namespace {

std::filesystem::path rulesDir() {
    return repoRoot() / "data" / "rules";
}

RuleDocument rule(std::string_view members) {
    auto doc = RuleDocument::parse(R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1,
        "id": "test.sections", "kind": "track-sections", )" +
                                   std::string(members) + "}");
    REQUIRE(doc);
    return std::move(doc).value();
}

std::string detailOf(std::string_view members) {
    auto made = TrackSectionsRule::fromDocument(rule(members));
    REQUIRE_FALSE(made);
    CHECK(made.error().code == ErrorCode::Invalid);
    return made.error().context.detail.value_or("?");
}

// A 300 m track: Straight 1 [0, 100), Turn 1 [100, 150), Straight 2 [150, 300); and a user set.
TrackMap track300() {
    TrackMap m;
    m.lengthM = 300.0;
    m.centre = {{0, 0, 0}, {300, 0, 300}};
    m.sections = {TrackSection{"s1", "Default", "Straight 1", 0.0, 100.0, "straight", false},
                  TrackSection{"t1", "Default", "Turn 1", 100.0, 150.0, "corner", false},
                  TrackSection{"s2", "Default", "Straight 2", 150.0, 300.0, "straight", false},
                  TrackSection{"u1", "Driver Notes", "Fast bit", 50.0, 200.0, "Fast", true}};
    return m;
}

// A lap at constant speed: its Corr Dist every 0.1 s.
std::shared_ptr<const DistanceIndex> indexOf(const model::Range& lap, double speedMps) {
    std::vector<Micros> t;
    std::vector<double> d;
    for (Micros u = lap.start; u <= lap.end; u += 100'000) {
        t.push_back(u);
        d.push_back(speedMps * static_cast<double>(u - lap.start) * 1e-6);
    }
    DistanceIndexKey key;
    key.start = lap.start;
    key.end = lap.end;
    key.startMarker = lap.startMarker;
    auto idx = DistanceIndex::fromSamples(t, d, key);
    REQUIRE(idx);
    return std::make_shared<const DistanceIndex>(std::move(idx).value());
}

// Outing: SOL 0, BCN 20 s, 30 s, 40 s, 45 s, EOL 50 s on the 300 m track (the lap lengths come from
// lookup()): the out lap [0, 20 s) is 200 m (measured backwards), Lap 1 [20, 30) and Lap 2 [30, 40) are 300
// m, Lap 3 [40, 45) is 250 m stretched to 300 m, the in lap [45, 50) stops short after 125 m.
model::DataSource outing() {
    std::vector<model::Marker> m = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::EOL, sec(50))};
    for (const double b : {20.0, 30.0, 40.0, 45.0}) {
        m.push_back(beacon(sec(b), "B" + std::to_string(static_cast<int>(b))));
    }
    model::DataLayer layer = layerWith(sec(50), std::move(m));
    layer.name = "Normal";
    return sourceWith(std::move(layer));
}

// Speeds per lap (by start time) that give the lap lengths of the comment above.
LapIndex lookup(const model::Range& lap) {
    LapIndex out;
    out.startsAtBeacon = lap.lapClass != model::LapClass::Out && lap.lapClass != model::LapClass::Partial;
    if (lap.start == sec(0)) {
        out.index = indexOf(lap, 10.0); // 200 m
    } else if (lap.start == sec(20) || lap.start == sec(30)) {
        out.index = indexOf(lap, 30.0); // 300 m
    } else if (lap.start == sec(40)) {
        auto idx = indexOf(lap, 50.0); // 250 m, stretched by 1.2 to 300 m
        DistanceIndex copy = *idx;
        copy.setStretch(1.2);
        out.index = std::make_shared<const DistanceIndex>(copy);
    } else {
        out.index = indexOf(lap, 25.0); // the in lap: 125 m of the track
    }
    return out;
}

const model::Range* rangeNamed(const model::RangeGroup& g, Micros lapStart, const std::string& name) {
    for (const model::Range& r : g.ranges) {
        if (r.name == name && r.start >= lapStart && r.start < lapStart + sec(5) + 1) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("track sections rule: the shipped document and its errors", "[unit]") {
    auto doc = RuleDocument::load(rulesDir() / "circuit.track-sections.rule.json");
    REQUIRE(doc);
    auto made = TrackSectionsRule::fromDocument(*doc);
    REQUIRE(made);
    const TrackSectionsSettings& s = (*made)->settings();
    const TrackSectionsSettings defaults;
    CHECK(s.produces == defaults.produces);
    CHECK(s.categoryChild == defaults.categoryChild);
    CHECK(s.parent == "Outings:Laps");
    CHECK(s.alwaysSets == std::vector<std::string>{"Default"});
    CHECK(s.categoryGroups == defaults.categoryGroups);
    CHECK(s.otherCategoryGroup == "{category}");
    CHECK(s.requiredChannel == "Corr Dist");
    CHECK_FALSE(s.supportsTrackGeneration);
    CHECK(s.supportsReporting);
    CHECK((*made)->id() == "circuit.track-sections");
    CHECK((*made)->phase() == 2);
    CHECK((*made)->appliesTo("Burst0"));
    auto viaFactory = makeRangeRule(*doc);
    REQUIRE(viaFactory);
    CHECK((*viaFactory)->kind() == "track-sections");

    CHECK(detailOf(R"("produces": "Outings:Laps:Sections")") == "/produces");
    CHECK(detailOf(R"("produces": "{set}")") == "/produces");
    CHECK(detailOf(R"("produces": "Outings:Laps:Track Sections:{set}", "parent": "Outings")") == "/parent");
    CHECK(detailOf(R"("produces": "Outings:Laps:Track Sections:{set}", "categoryChild": "Corners")") ==
          "/categoryChild");
    CHECK(detailOf(R"("produces": "Outings:Laps:Track Sections:{set}", "sets": {"from": "file"})") ==
          "/sets/from");
    CHECK(
        detailOf(R"("produces": "Outings:Laps:Track Sections:{set}", "categoryGroups": {"corner": "A:B"})") ==
        "/categoryGroups/corner");
    CHECK(detailOf(R"("produces": "Outings:Laps:Track Sections:{set}", "colour": "red")") == "/colour");
    auto beacons = RuleDocument::load(rulesDir() / "circuit.beacon-pair.rule.json");
    REQUIRE(beacons);
    CHECK(TrackSectionsRule::fromDocument(*beacons).error().context.detail == "/kind");
}

TEST_CASE("track sections rule: the origin rule of laps that do not start at a beacon", "[unit]") {
    // Forward laps measure from their start.
    CHECK(lapDistanceOfTrackDistance(120.0, 310.0, 300.0, true) == 120.0);
    CHECK(trackDistanceOfLapDistance(120.0, 310.0, 300.0, true) == 120.0);
    // An out lap of 200 m on a 300 m track: track 150 is lap 50; track 50 falls before the lap start.
    CHECK(lapDistanceOfTrackDistance(150.0, 200.0, 300.0, false) == 50.0);
    CHECK(lapDistanceOfTrackDistance(100.0, 200.0, 300.0, false) == 0.0);
    CHECK_FALSE(lapDistanceOfTrackDistance(50.0, 200.0, 300.0, false).has_value());
    CHECK(trackDistanceOfLapDistance(50.0, 200.0, 300.0, false) == 150.0);
    CHECK(trackDistanceOfLapDistance(200.0, 200.0, 300.0, false) == 300.0);
    CHECK(trackDistanceOfLapDistance(0.0, 400.0, 300.0, false) == -100.0); // pointAt wraps it
}

TEST_CASE("track sections rule: hand-computed sections of every lap", "[unit][AC-M3-03]") {
    model::DataSource src = outing();
    const RuleRunner phase1 = RuleRunner::circuitDefault();
    phase1.run(src);
    const model::RangeGroupTree& tree = src.layers[0].ranges;
    const model::RangeGroup* laps = tree.find("Outings:Laps");
    REQUIRE(laps != nullptr);
    REQUIRE(laps->ranges.size() == 5);

    const TrackSectionsRule rule;
    auto groups = rule.sections(tree, track300(), lookup, {});
    REQUIRE(groups);
    std::vector<std::string> paths;
    for (const model::RangeGroup& g : *groups) {
        paths.push_back(g.path);
    }
    CHECK(paths ==
          std::vector<std::string>{
              "Outings:Laps:Track Sections", "Outings:Laps:Track Sections:Default",
              "Outings:Laps:Track Sections:Default:Straights", "Outings:Laps:Track Sections:Default:Corners",
              "Outings:Laps:Track Sections:Driver Notes", "Outings:Laps:Track Sections:Driver Notes:Fast"});
    CHECK((*groups)[0].ranges.empty());
    const model::RangeGroup& def = (*groups)[1];
    // Out lap [0, 20 s), 200 m at 10 m/s, measured backwards (track s at lap s - 100): Straight 1 omitted,
    // Turn 1 [0, 5 s), Straight 2 [5 s, 20 s).
    // Lap 1 [20, 30 s), 300 m at 30 m/s: Straight 1 [20, 23.333), Turn 1 [23.333, 25), Straight 2 [25, 30).
    // Lap 3 [40, 45 s), 250 m at 50 m/s stretched 1.2: track 100 m is lap 83.33 m (1.667 s), track 150 m is
    // 125 m (2.5 s). In lap [45, 50 s), 125 m at 25 m/s: Straight 1 [45, 49), Turn 1 [49, 50) clamped,
    // Straight 2 omitted.
    struct Expect {
        Micros lapStart;
        const char* name;
        Micros start;
        Micros end;
    };
    const std::vector<Expect> expected = {
        {sec(0), "Turn 1", sec(0), sec(5)},           {sec(0), "Straight 2", sec(5), sec(20)},
        {sec(20), "Straight 1", sec(20), 23'333'333}, {sec(20), "Turn 1", 23'333'333, sec(25)},
        {sec(20), "Straight 2", sec(25), sec(30)},    {sec(30), "Straight 1", sec(30), 33'333'333},
        {sec(30), "Turn 1", 33'333'333, sec(35)},     {sec(30), "Straight 2", sec(35), sec(40)},
        {sec(40), "Straight 1", sec(40), 41'666'667}, {sec(40), "Turn 1", 41'666'667, sec(42.5)},
        {sec(40), "Straight 2", sec(42.5), sec(45)},  {sec(45), "Straight 1", sec(45), sec(49)},
        {sec(45), "Turn 1", sec(49), sec(50)},
    };
    REQUIRE(def.ranges.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const model::Range& r = def.ranges[i];
        const Expect& e = expected[i];
        INFO(i << ": " << r.name << " [" << r.start << ", " << r.end << ")");
        CHECK(r.name == e.name);
        CHECK(std::abs(r.start - e.start) <= 1);
        CHECK(std::abs(r.end - e.end) <= 1);
        CHECK(r.kind == model::RangeKind::Section);
        const model::Range* lap = nullptr;
        for (const model::Range& l : laps->ranges) {
            if (l.start == e.lapStart) {
                lap = &l;
            }
        }
        REQUIRE(lap != nullptr);
        CHECK(r.startMarker == lap->startMarker);
        CHECK(r.lapClass == lap->lapClass);
        CHECK(r.number == lap->number);
        CHECK(r.trusted == lap->trusted);
        CHECK(r.id == model::rangeId(def.path, lap->startMarker, r.name));
    }
    CHECK(def.ranges[0].abbrev == "t1");
    // The category children hold the same sections with their own ids.
    const model::RangeGroup& corners = (*groups)[3];
    REQUIRE(corners.ranges.size() == 5);
    CHECK(corners.ranges[1].start == def.ranges[3].start);
    CHECK(corners.ranges[1].id == model::rangeId(corners.path, laps->ranges[1].startMarker, "Turn 1"));
    CHECK(corners.ranges[1].id != def.ranges[3].id);
    // The user set: Fast bit [50, 200) on Lap 1 is [21.667, 26.667).
    const model::RangeGroup& notes = (*groups)[4];
    const model::Range* fast = rangeNamed(notes, sec(20), "Fast bit");
    REQUIRE(fast != nullptr);
    CHECK(std::abs(fast->start - 21'666'667) <= 1);
    CHECK(std::abs(fast->end - 26'666'667) <= 1);
    CHECK((*groups)[5].ranges.size() == notes.ranges.size());

    // No index for a lap: no sections for it. No laps group: nothing at all. Cancelled.
    auto none = rule.sections(tree, track300(), [](const model::Range&) { return LapIndex{}; }, {});
    REQUIRE(none);
    CHECK((*none)[1].ranges.empty());
    CHECK(rule.sections(model::RangeGroupTree{}, track300(), lookup, {})->empty());
    CancelSource cancel;
    cancel.cancel();
    CHECK(rule.sections(tree, track300(), lookup, cancel.token()).error().code == ErrorCode::Cancelled);

    // A map without sections still yields the always-present Default group (empty).
    TrackMap bare = track300();
    bare.sections.clear();
    auto empty = rule.sections(tree, bare, lookup, {});
    REQUIRE(empty);
    REQUIRE(empty->size() == 2);
    CHECK((*empty)[1].path == "Outings:Laps:Track Sections:Default");
    CHECK((*empty)[1].ranges.empty());
}

TEST_CASE("track sections rule: RuleRunner's phase 2 builds and versions the groups (D-140)", "[unit]") {
    model::DataSource src = outing();
    const RuleRunner circuit = RuleRunner::circuitDefault();
    std::vector<std::shared_ptr<const IRangeRule>> rules(circuit.rules().begin(), circuit.rules().end());
    rules.push_back(std::make_shared<const TrackSectionsRule>());
    const RuleRunner runner(rules);
    runner.run(src);
    model::DataLayer& layer = src.layers[0];
    CHECK(layer.ranges.find("Outings:Laps:Track Sections") == nullptr); // phase 1 builds no sections
    const std::uint64_t lapsVersion = layer.ranges.rangeVersion("Outings:Laps");

    auto map = std::make_shared<const TrackMap>(track300());
    auto first = runner.runPhase2(src, "Normal", std::nullopt, map, lookup, {});
    REQUIRE(first);
    CHECK(first->rangeVersion("Outings:Laps:Track Sections:Default") == 1);
    CHECK(first->rangeVersion("Outings:Laps:Track Sections:Default:Corners") == 1);
    CHECK(first->rangeVersion("Outings:Laps") == lapsVersion);
    layer.ranges = *first; // published

    // The same map again: unchanged versions. A moved section: only its set's groups bump.
    auto same = runner.runPhase2(src, "Normal", std::nullopt, map, lookup, {});
    REQUIRE(same);
    CHECK(*same == *first);
    TrackMap moved = track300();
    moved.sections[3].endM = 250.0;
    auto bumped =
        runner.runPhase2(src, "Normal", std::nullopt, std::make_shared<const TrackMap>(moved), lookup, {});
    REQUIRE(bumped);
    CHECK(bumped->rangeVersion("Outings:Laps:Track Sections:Driver Notes") == 2);
    CHECK(bumped->rangeVersion("Outings:Laps:Track Sections:Driver Notes:Fast") == 2);
    CHECK(bumped->rangeVersion("Outings:Laps:Track Sections:Default") == 1);
    CHECK(bumped->rangeVersion("Outings:Laps") == lapsVersion);

    // Phase 1 again (a detail edit): the laps are unchanged, so the sections stay with their versions.
    runner.run(src);
    CHECK(layer.ranges.rangeVersion("Outings:Laps:Track Sections:Default") == 1);
    REQUIRE(layer.ranges.find("Outings:Laps:Track Sections:Driver Notes:Fast") != nullptr);
    // A beacon edit changes the laps: the stale sections go until phase 2 lands again.
    layer.markers.push_back(beacon(sec(35), "B35"));
    model::sortMarkers(layer.markers);
    runner.run(src);
    CHECK(layer.ranges.find("Outings:Laps:Track Sections") == nullptr);

    // No map: the groups are removed.
    layer.ranges = *first;
    auto removed = runner.runPhase2(src, "Normal", std::nullopt, nullptr, lookup, {});
    REQUIRE(removed);
    CHECK(removed->find("Outings:Laps:Track Sections") == nullptr);
    CHECK(runner.runPhase2(src, "Burst9", std::nullopt, map, lookup, {}).error().code == ErrorCode::NotFound);
}
