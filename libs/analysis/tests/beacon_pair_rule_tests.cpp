// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BeaconPairRule (02 section 4.4 rules 9-13, 02 section 4.5; D-014, D-075, D-132; AC-M1-07): phase-1 laps,
// classes, numbering, trust and ids from markers; phase-2 stint and pitStop annotations from the speed, the
// lap counter, RESET markers and logged-data holes. Expected values are reasoned from the rules, case by
// case.
#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::LapClass;
using model::Marker;
using model::MarkerClass;
using model::Range;

namespace {

const BeaconPairRule& circuit() {
    static const BeaconPairRule rule(BeaconPairSettings::circuitDefault());
    return rule;
}

// The laps the circuit rule produces for `layer`; empty when it produces no group.
std::vector<Range> lapsFor(const model::DataLayer& layer, const std::vector<model::Detail>& details = {}) {
    auto tree = circuit().run(layer, RuleContext{details});
    REQUIRE(tree);
    if (tree->roots.empty()) {
        return {};
    }
    REQUIRE(tree->roots.size() == 1);
    CHECK(tree->roots[0].path == "Outings:Laps");
    CHECK(tree->roots[0].name == "Laps");
    return tree->roots[0].ranges;
}

struct Expect {
    LapClass cls;
    int number;
    const char* name;
    const char* abbrev;
    bool trusted;
    double start;
    double end;
};

void checkLaps(const std::vector<Range>& laps, const std::vector<Expect>& expected) {
    REQUIRE(laps.size() == expected.size());
    for (std::size_t i = 0; i < laps.size(); ++i) {
        INFO("lap " << i << ": " << laps[i].name);
        CHECK(laps[i].kind == model::RangeKind::Lap);
        CHECK(laps[i].lapClass == expected[i].cls);
        CHECK(laps[i].number == expected[i].number);
        CHECK(laps[i].name == expected[i].name);
        CHECK(laps[i].abbrev == expected[i].abbrev);
        CHECK(laps[i].trusted == expected[i].trusted);
        CHECK(laps[i].start == sec(expected[i].start));
        CHECK(laps[i].end == sec(expected[i].end));
        CHECK(laps[i].enabled);
        CHECK(laps[i].stint == 0); // phase 1 never annotates
        CHECK_FALSE(laps[i].pitStop);
        CHECK(laps[i].id == model::rangeId("Outings:Laps", laps[i].startMarker));
    }
}

// SOL at 0, the beacons, EOL at `end` seconds.
model::DataLayer circuitLayer(std::vector<double> beacons, double end, std::vector<Marker> extra = {}) {
    std::vector<Marker> m = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::EOL, sec(end))};
    for (std::size_t i = 0; i < beacons.size(); ++i) {
        m.push_back(beacon(sec(beacons[i]), "B" + std::to_string(i + 1)));
    }
    for (Marker& x : extra) {
        m.push_back(std::move(x));
    }
    return layerWith(sec(end), std::move(m));
}

const Marker& markerNamed(const model::DataLayer& layer, const std::string& name) {
    for (const Marker& m : layer.markers) {
        if (m.name == name) {
            return m;
        }
    }
    FAIL("no marker " << name);
    return layer.markers.front();
}

// Phase 1 then phase 2 over a layer with the given speed (and the layer's channels).
std::vector<Range> annotated(const model::DataLayer& layer, const std::optional<store::Series>& speed,
                             const BeaconPairRule& rule = circuit()) {
    auto tree = rule.run(layer, RuleContext{});
    REQUIRE(tree);
    REQUIRE(tree->roots.size() == 1);
    model::RangeGroup laps = tree->roots[0];
    const model::RangeGroup before = laps;
    REQUIRE(rule.annotate(layer, speed, laps, {}));
    // Rule 12: annotations never change ids, classes, numbers, names, bounds or trust.
    REQUIRE(laps.ranges.size() == before.ranges.size());
    for (std::size_t i = 0; i < laps.ranges.size(); ++i) {
        Range stripped = laps.ranges[i];
        stripped.stint = 0;
        stripped.pitStop = false;
        CHECK(stripped == before.ranges[i]);
    }
    return laps.ranges;
}

// A 10 Hz speed of `seconds` length: 40 m/s, 0 m/s inside the [from, to) spans (seconds).
std::vector<double> speedWithStops(double seconds, const std::vector<std::pair<double, double>>& stops) {
    const auto at = [](double s) { return static_cast<std::size_t>(std::lround(s * 10.0)); };
    std::vector<double> v(at(seconds), 40.0);
    for (const auto& [a, b] : stops) {
        for (std::size_t i = at(a); i < at(b) && i < v.size(); ++i) {
            v[i] = 0.0;
        }
    }
    return v;
}

std::vector<int> stints(const std::vector<Range>& laps) {
    std::vector<int> out;
    out.reserve(laps.size());
    for (const Range& r : laps) {
        out.push_back(r.stint);
    }
    return out;
}

std::vector<bool> pitStops(const std::vector<Range>& laps) {
    std::vector<bool> out;
    out.reserve(laps.size());
    for (const Range& r : laps) {
        out.push_back(r.pitStop);
    }
    return out;
}

} // namespace

// ---- phase 1
// ------------------------------------------------------------------------------------------------

TEST_CASE("beacon-pair: out, flying and in laps between beacons", "[unit][AC-M1-07]") {
    const model::DataLayer layer = circuitLayer({10, 30, 55}, 70);
    const std::vector<Range> laps = lapsFor(layer);
    checkLaps(laps, {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                     {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                     {LapClass::Flying, 2, "Lap 2", "L2", true, 30, 55},
                     {LapClass::In, 3, "In Lap", "In", false, 55, 70}});
    CHECK(laps[0].startMarker == layer.markers.front().id); // SOL
    CHECK(laps[1].startMarker == markerNamed(layer, "B1").id);
    CHECK(laps[1].endMarker == markerNamed(layer, "B2").id);
    CHECK(laps[3].endMarker == layer.markers.back().id); // EOL

    auto tree = circuit().run(layer, RuleContext{});
    REQUIRE(tree);
    CHECK(tree->roots[0].supportsTrackGeneration);
    CHECK(tree->roots[0].supportsReporting);
    CHECK(tree->roots[0].version == 0);
}

TEST_CASE("beacon-pair: the Start Lap detail numbers the first flying lap", "[unit][AC-M1-07]") {
    const model::DataLayer layer = circuitLayer({10, 30, 55}, 70);
    checkLaps(lapsFor(layer, {textDetail("Start Lap", "7")}),
              {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
               {LapClass::Flying, 7, "Lap 7", "L7", true, 10, 30},
               {LapClass::Flying, 8, "Lap 8", "L8", true, 30, 55},
               {LapClass::In, 9, "In Lap", "In", false, 55, 70}});
    model::Detail quantity{"start lap", model::Quantityd{}, model::Provenance::User, std::nullopt};
    std::get<model::Quantityd>(quantity.value).v = 4.0;
    CHECK(lapsFor(layer, {quantity})[1].number == 4); // case-insensitive id, numeric value
    for (const char* text : {"abc", "2.5", "", "7 laps"}) {
        INFO(text);
        CHECK(lapsFor(layer, {textDetail("Start Lap", text)})[1].number == 1); // not an integer: default
    }
    CHECK(lapsFor(layer, {textDetail("Start Lap", " 12 ")})[1].number == 12);
}

TEST_CASE("beacon-pair: a RESET closes a partial lap that keeps the number without consuming it",
          "[unit][AC-M1-07]") {
    const model::DataLayer layer = circuitLayer({10, 30, 52, 71, 92}, 120, {reset(sec(45), "R1")});
    checkLaps(lapsFor(layer), {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                               {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                               {LapClass::Partial, 2, "Lap 2 (partial)", "P2", false, 30, 45},
                               {LapClass::Out, 0, "Out Lap", "Out", false, 45, 52},
                               {LapClass::Flying, 2, "Lap 2", "L2", true, 52, 71},
                               {LapClass::Flying, 3, "Lap 3", "L3", true, 71, 92},
                               {LapClass::In, 4, "In Lap", "In", false, 92, 120}});
    CHECK(lapsFor(layer)[3].startMarker == markerNamed(layer, "R1").id); // the next range starts at the RESET
}

TEST_CASE("beacon-pair: a RESET in the out lap and a RESET before EOL", "[unit][AC-M1-07]") {
    checkLaps(lapsFor(circuitLayer({10, 30}, 40, {reset(sec(5))})),
              {{LapClass::Partial, 1, "Lap 1 (partial)", "P1", false, 0, 5},
               {LapClass::Out, 0, "Out Lap", "Out", false, 5, 10},
               {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
               {LapClass::In, 2, "In Lap", "In", false, 30, 40}});
    // RESET -> EOL has no transition: the time after the RESET is no lap.
    checkLaps(lapsFor(circuitLayer({10}, 30, {reset(sec(20))})),
              {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
               {LapClass::Partial, 1, "Lap 1 (partial)", "P1", false, 10, 20}});
    // Two RESETs in a row: * -> RESET.
    checkLaps(lapsFor(circuitLayer({10, 40}, 50, {reset(sec(20), "R1"), reset(sec(25), "R2")})),
              {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
               {LapClass::Partial, 1, "Lap 1 (partial)", "P1", false, 10, 20},
               {LapClass::Partial, 1, "Lap 1 (partial)", "P1", false, 20, 25},
               {LapClass::Out, 0, "Out Lap", "Out", false, 25, 40},
               {LapClass::In, 1, "In Lap", "In", false, 40, 50}});
}

TEST_CASE("beacon-pair: ignored and split beacons, disabled markers and other classes create no lap",
          "[unit][AC-M1-07]") {
    Marker disabled = beacon(sec(20), "disabled");
    disabled.enabled = false;
    Marker stageInIntrinsic = model::makeLoggedMarker(MarkerClass::STGSTRT, sec(25), "", "Stage", "", 0, 0,
                                                      model::Provenance::Companion);
    Marker resetElsewhere = model::makeLoggedMarker(MarkerClass::RESET, sec(35), "Stages", "Reset", "", 0, 0,
                                                    model::Provenance::Companion);
    const model::DataLayer layer = circuitLayer(
        {10, 30, 50}, 60,
        {beacon(sec(15), "S1", MarkerClass::SPLTBCN), beacon(sec(31), "I1", MarkerClass::IGRDBCN),
         model::makeDeviceEventMarker(3, sec(40), 0), disabled, stageInIntrinsic, resetElsewhere});
    checkLaps(lapsFor(layer), {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                               {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                               {LapClass::Flying, 2, "Lap 2", "L2", true, 30, 50},
                               {LapClass::In, 3, "In Lap", "In", false, 50, 60}});
}

TEST_CASE("beacon-pair: a layer without beacons produces no laps group (D-104)", "[unit][AC-M1-07]") {
    CHECK(lapsFor(circuitLayer({}, 60)).empty());
    CHECK(
        lapsFor(circuitLayer({}, 60, {beacon(sec(10), "I1", MarkerClass::IGRDBCN), reset(sec(20))})).empty());
    auto tree = circuit().run(circuitLayer({}, 60), RuleContext{});
    REQUIRE(tree);
    CHECK(tree->roots.empty());
}

TEST_CASE("beacon-pair: numbering restarts in every outing", "[unit][AC-M1-07]") {
    std::vector<Marker> m = {intrinsic(MarkerClass::SOL, 0),
                             beacon(sec(10), "B1"),
                             beacon(sec(30), "B2"),
                             intrinsic(MarkerClass::EOL, sec(40)),
                             intrinsic(MarkerClass::SOL, sec(50), 0),
                             beacon(sec(60), "B3"),
                             beacon(sec(80), "B4"),
                             intrinsic(MarkerClass::EOL, sec(100))};
    const model::DataLayer layer = layerWith(sec(100), std::move(m));
    checkLaps(lapsFor(layer, {textDetail("Start Lap", "3")}),
              {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
               {LapClass::Flying, 3, "Lap 3", "L3", true, 10, 30},
               {LapClass::In, 4, "In Lap", "In", false, 30, 40},
               {LapClass::Out, 0, "Out Lap", "Out", false, 50, 60},
               {LapClass::Flying, 3, "Lap 3", "L3", true, 60, 80},
               {LapClass::In, 4, "In Lap", "In", false, 80, 100}});
}

TEST_CASE("beacon-pair: virtual SOL and EOL when the layer lacks them", "[unit][AC-M1-07]") {
    // No SOL: the out lap starts at 0 on the id an importer-synthesised SOL would have.
    const model::DataLayer noSol = layerWith(
        sec(40), {beacon(sec(10), "B1"), beacon(sec(30), "B2"), intrinsic(MarkerClass::EOL, sec(40))});
    const std::vector<Range> a = lapsFor(noSol);
    checkLaps(a, {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                  {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                  {LapClass::In, 2, "In Lap", "In", false, 30, 40}});
    CHECK(a[0].startMarker == model::markerIdLogged(MarkerClass::SOL, 0, "", 0));
    // No EOL: the in lap ends at the layer duration with a nil end marker.
    const model::DataLayer noEol =
        layerWith(sec(50), {intrinsic(MarkerClass::SOL, 0), beacon(sec(10), "B1"), beacon(sec(30), "B2")});
    const std::vector<Range> b = lapsFor(noEol);
    checkLaps(b, {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                  {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                  {LapClass::In, 2, "In Lap", "In", false, 30, 50}});
    CHECK(b[2].endMarker.isNil());
}

TEST_CASE("beacon-pair: a SOL while an outing is open ends that outing", "[unit][AC-M1-07]") {
    // No EOL between the outings (companion or user markers): the first outing still gets its in lap, ending
    // where the second outing starts, and the second outing numbers from 1 again.
    std::vector<Marker> m = {
        intrinsic(MarkerClass::SOL, 0),          beacon(sec(10), "B1"), beacon(sec(30), "B2"),
        intrinsic(MarkerClass::SOL, sec(50), 1), beacon(sec(60), "B3"), beacon(sec(80), "B4"),
        intrinsic(MarkerClass::EOL, sec(100))};
    const model::DataLayer layer = layerWith(sec(100), std::move(m));
    const std::vector<Range> laps = lapsFor(layer);
    checkLaps(laps, {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                     {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                     {LapClass::In, 2, "In Lap", "In", false, 30, 50},
                     {LapClass::Out, 0, "Out Lap", "Out", false, 50, 60},
                     {LapClass::Flying, 1, "Lap 1", "L1", true, 60, 80},
                     {LapClass::In, 2, "In Lap", "In", false, 80, 100}});
    CHECK(laps[2].endMarker.isNil()); // the implied end has no marker
}

TEST_CASE("beacon-pair: an outing that ends exactly where the next one starts", "[unit][AC-M1-07]") {
    // EOL and SOL at 50 s (model order: EOL first): outing 1 keeps its in lap, outing 2 its out lap.
    std::vector<Marker> m = {intrinsic(MarkerClass::SOL, 0),
                             beacon(sec(10), "B1"),
                             beacon(sec(30), "B2"),
                             intrinsic(MarkerClass::EOL, sec(50)),
                             intrinsic(MarkerClass::SOL, sec(50), 1),
                             beacon(sec(60), "B3"),
                             beacon(sec(80), "B4"),
                             intrinsic(MarkerClass::EOL, sec(100), 1)};
    const model::DataLayer layer = layerWith(sec(100), std::move(m));
    checkLaps(lapsFor(layer), {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                               {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                               {LapClass::In, 2, "In Lap", "In", false, 30, 50},
                               {LapClass::Out, 0, "Out Lap", "Out", false, 50, 60},
                               {LapClass::Flying, 1, "Lap 1", "L1", true, 60, 80},
                               {LapClass::In, 2, "In Lap", "In", false, 80, 100}});
}

TEST_CASE("beacon-pair: the virtual EOL is the end of the data, not a later marker of another kind",
          "[unit][AC-M1-07]") {
    // No EOL; a device event and a disabled beacon lie after the data end (50 s): the in lap ends at 50 s.
    Marker late =
        model::makeLoggedMarker(MarkerClass::DeviceEvent, sec(70), std::string(model::kGroupDeviceEvents),
                                "Logging off", "", 0, 0, model::Provenance::Derived);
    Marker off = beacon(sec(65), "B3");
    off.enabled = false;
    const model::DataLayer layer = layerWith(
        sec(50), {intrinsic(MarkerClass::SOL, 0), beacon(sec(10), "B1"), beacon(sec(30), "B2"), late, off});
    checkLaps(lapsFor(layer), {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                               {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                               {LapClass::In, 2, "In Lap", "In", false, 30, 50}});
}

TEST_CASE("beacon-pair: beacons at the same time produce no empty lap", "[unit][AC-M1-07]") {
    Marker twin = model::makeLoggedMarker(MarkerClass::BCN, sec(10), "Beacons", "B1b", "", 77, 1,
                                          model::Provenance::Companion);
    const model::DataLayer layer = circuitLayer({10, 30}, 40, {twin});
    const std::vector<Range> laps = lapsFor(layer);
    checkLaps(laps, {{LapClass::Out, 0, "Out Lap", "Out", false, 0, 10},
                     {LapClass::Flying, 1, "Lap 1", "L1", true, 10, 30},
                     {LapClass::In, 2, "In Lap", "In", false, 30, 40}});
}

TEST_CASE("beacon-pair: lap ids depend only on the start marker (rule 9)", "[unit][AC-M1-07]") {
    model::DataLayer layer = circuitLayer({10, 30, 55}, 70);
    const std::vector<Range> before = lapsFor(layer);
    // Offset B2 by +1 s (an Offset edit keeps the marker id) and insert a user beacon in lap 3.
    for (Marker& m : layer.markers) {
        if (m.name == "B2") {
            m.originalT = m.t;
            m.t += sec(1);
        }
    }
    layer.markers.push_back(model::makeLoggedMarker(MarkerClass::BCN, sec(60), "Beacons", "user", "", 0, 0,
                                                    model::Provenance::User));
    model::sortMarkers(layer.markers);
    const std::vector<Range> after = lapsFor(layer);
    REQUIRE(after.size() == before.size() + 1);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(after[i].id == before[i].id);
    }
    CHECK(after[2].start == sec(31));
    CHECK(after[3].lapClass == LapClass::Flying); // B3 -> user beacon
    CHECK(after[4].lapClass == LapClass::In);
}

TEST_CASE("beacon-pair: a custom document drives classes, templates and layers", "[unit]") {
    BeaconPairSettings s = BeaconPairSettings::circuitDefault();
    s.layers = {"Normal"};
    s.classes.at("partial").name = "Lap {n} reset";
    s.classes.at("partial").trusted = true;
    s.classes.at("in").numberIsLastPlusOne = false;
    s.classes.at("in").fixedNumber = 99;
    s.defaultStart = 0;
    const BeaconPairRule rule(s, "test.custom");
    CHECK(rule.id() == "test.custom");
    CHECK(rule.appliesTo("Normal"));
    CHECK_FALSE(rule.appliesTo("Fastest"));
    auto tree = rule.run(circuitLayer({10, 30}, 50, {reset(sec(40))}), RuleContext{});
    REQUIRE(tree);
    const std::vector<Range>& laps = tree->roots[0].ranges;
    REQUIRE(laps.size() == 3);
    CHECK(laps[1].number == 0); // numbering from 0
    CHECK(laps[2].name == "Lap 1 reset");
    CHECK(laps[2].trusted);
}

// ---- phase 2
// ------------------------------------------------------------------------------------------------

TEST_CASE("beacon-pair annotations: 15 s stationary sets only pitStop", "[unit][AC-M1-07]") {
    TestCache tc;
    // Laps: out [0,10), L1 [10,70), L2 [70,130), in [130,150). L2 stops 20 s (80 - 100 s).
    const model::DataLayer layer = circuitLayer({10, 70, 130}, 150);
    const auto speed = tc.uniform(Rate::fromHz(10), speedWithStops(150, {{80, 100}}));
    const std::vector<Range> laps = annotated(layer, speed);
    CHECK(pitStops(laps) == std::vector<bool>{false, false, true, false});
    CHECK(laps[2].lapClass == LapClass::Flying);
    CHECK(laps[2].number == 2);
    CHECK(laps[2].trusted);
    CHECK(stints(laps) == std::vector<int>{1, 1, 1, 2}); // the lap after the pit stop starts stint 2
}

TEST_CASE("beacon-pair annotations: the stationary threshold and duration are inclusive at 15 s",
          "[unit][AC-M1-07]") {
    TestCache tc;
    const model::DataLayer layer = circuitLayer({10, 70}, 90);
    // 149 samples below the threshold: 14.9 s, no pit stop.
    CHECK(pitStops(annotated(layer, tc.uniform(Rate::fromHz(10), speedWithStops(90, {{20, 34.9}})))) ==
          std::vector<bool>{false, false, false});
    // 150 samples: 15.0 s, a pit stop.
    CHECK(pitStops(annotated(layer, tc.uniform(Rate::fromHz(10), speedWithStops(90, {{20, 35}})))) ==
          std::vector<bool>{false, true, false});
    // 0.9 km/h is stationary, 1.0 km/h is not (stationaryBelow is strict).
    std::vector<double> slow = speedWithStops(90, {});
    for (std::size_t i = 200; i < 400; ++i) {
        slow[i] = 0.9 / 3.6;
    }
    CHECK(annotated(layer, tc.uniform(Rate::fromHz(10), slow))[1].pitStop);
    for (std::size_t i = 200; i < 400; ++i) {
        slow[i] = 1.0 / 3.6;
    }
    CHECK_FALSE(annotated(layer, tc.uniform(Rate::fromHz(10), slow))[1].pitStop);
}

TEST_CASE("beacon-pair annotations: invalid speed samples end a stationary run", "[unit][AC-M1-07]") {
    TestCache tc;
    const model::DataLayer layer = circuitLayer({10, 70}, 90);
    std::vector<double> v = speedWithStops(90, {{20, 40}});
    std::vector<bool> valid(v.size(), true);
    valid[300] = false; // 30 s: two runs of 10 s
    CHECK_FALSE(annotated(layer, tc.uniform(Rate::fromHz(10), v, valid))[1].pitStop);
    CHECK(annotated(layer, tc.uniform(Rate::fromHz(10), v))[1].pitStop);
}

TEST_CASE("beacon-pair annotations: stints never split on ordinary beacon intervals", "[unit][AC-M1-07]") {
    TestCache tc;
    const model::DataLayer layer = circuitLayer({10, 20, 30, 40, 50, 60, 70, 80}, 90);
    const std::vector<Range> laps = annotated(layer, tc.uniform(Rate::fromHz(10), speedWithStops(90, {})));
    for (const Range& r : laps) {
        CHECK(r.stint == 1);
        CHECK_FALSE(r.pitStop);
    }
}

TEST_CASE("beacon-pair annotations: a counter reset starts a stint at the next lap", "[unit][AC-M1-07]") {
    TestCache tc;
    // Laps: out [0,10), L1 [10,30), L2 [30,50), L3 [50,70), in [70,80). Counter 0,1,2,3 at the beacons, reset
    // to 0 at 41 s (inside L2): stint 2 from L3. A second decrease exactly at the L3->in beacon sample (70
    // s).
    std::vector<double> counter(800, 0.0);
    for (std::size_t i = 0; i < counter.size(); ++i) {
        const double t = static_cast<double>(i) / 10.0;
        counter[i] = t < 10 ? 0 : t < 30 ? 1 : t < 41 ? 2 : t < 50 ? 0 : t < 70 ? 1 : 0;
    }
    model::DataLayer layer = circuitLayer({10, 30, 50, 70}, 80);
    layer.addChannel(channel("Lap Number", tc.uniform(Rate::fromHz(10), counter)));
    const std::vector<Range> laps = annotated(layer, std::nullopt);
    CHECK(stints(laps) == std::vector<int>{1, 1, 1, 2, 3});
    CHECK(pitStops(laps) == std::vector<bool>{false, false, false, false, false});
}

TEST_CASE("beacon-pair annotations: a RESET marker starts a stint", "[unit][AC-M1-07]") {
    const model::DataLayer layer = circuitLayer({10, 30, 52, 71}, 90, {reset(sec(45))});
    CHECK(stints(annotated(layer, std::nullopt)) == std::vector<int>{1, 1, 1, 2, 2, 2});
}

TEST_CASE("beacon-pair annotations: a logged-data hole over dataGapUs starts a stint", "[unit][AC-M1-07]") {
    TestCache tc;
    // A timestamped channel sampled every second with a 20 s hole (31 s -> 51 s) inside L2 [30, 60), and a 9
    // s hole (65 s -> 74 s) inside L3 [60, 90).
    std::vector<std::int64_t> times;
    for (int s = 0; s <= 31; ++s) {
        times.push_back(sec(s));
    }
    for (int s = 51; s <= 65; ++s) {
        times.push_back(sec(s));
    }
    for (int s = 74; s < 100; ++s) {
        times.push_back(sec(s));
    }
    model::DataLayer layer = circuitLayer({10, 30, 60, 90}, 100);
    layer.addChannel(channel("Gps Time", tc.timestamped(times, std::vector<double>(times.size(), 1.0))));
    layer.addChannel(channel("Gps Time 2", std::get<store::TimestampedSeries>(layer.channels[0].series)));
    CHECK(stints(annotated(layer, std::nullopt)) == std::vector<int>{1, 1, 1, 2, 2});
}

TEST_CASE("beacon-pair annotations: a logger pause between outings over dataGapUs starts a stint",
          "[unit][AC-M1-07]") {
    const auto twoOutings = [](double pause) {
        const double s2 = 40 + pause;
        return layerWith(sec(s2 + 40),
                         {intrinsic(MarkerClass::SOL, 0), beacon(sec(10), "B1"), beacon(sec(30), "B2"),
                          intrinsic(MarkerClass::EOL, sec(40)), intrinsic(MarkerClass::SOL, sec(s2)),
                          beacon(sec(s2 + 10), "B3"), beacon(sec(s2 + 30), "B4"),
                          intrinsic(MarkerClass::EOL, sec(s2 + 40))});
    };
    CHECK(stints(annotated(twoOutings(20), std::nullopt)) == std::vector<int>{1, 1, 1, 2, 2, 2});
    CHECK(stints(annotated(twoOutings(5), std::nullopt)) == std::vector<int>{1, 1, 1, 1, 1, 1});
}

TEST_CASE(
    "beacon-pair annotations: a wait in the pit box before the car moves does not split the first stint",
    "[unit][AC-M1-07]") {
    TestCache tc;
    // Out [0,30) starts with 20 s stationary; L2 [50,90) has a 20 s pit stop.
    const model::DataLayer layer = circuitLayer({30, 50, 90}, 110);
    const auto speed = tc.uniform(Rate::fromHz(10), speedWithStops(110, {{0, 20}, {60, 80}}));
    const std::vector<Range> laps = annotated(layer, speed);
    CHECK(pitStops(laps) == std::vector<bool>{true, false, true, false});
    CHECK(stints(laps) == std::vector<int>{1, 1, 1, 2});
}

TEST_CASE("beacon-pair annotations: the speed channel fallback, no speed and disabled annotations",
          "[unit][AC-M1-07]") {
    TestCache tc;
    model::DataLayer layer = circuitLayer({10, 70}, 90);
    // Without a speed series and without a Corr Speed channel: no pit stop.
    CHECK(pitStops(annotated(layer, std::nullopt)) == std::vector<bool>{false, false, false});
    // A layer channel named after annotations.speedChannel is used when no series is passed.
    layer.addChannel(channel("Corr Speed", tc.uniform(Rate::fromHz(10), speedWithStops(90, {{20, 40}}))));
    CHECK(pitStops(annotated(layer, std::nullopt)) == std::vector<bool>{false, true, false});

    BeaconPairSettings off = BeaconPairSettings::circuitDefault();
    off.annotations.enabled = false;
    const BeaconPairRule rule(off);
    auto tree = rule.run(layer, RuleContext{});
    REQUIRE(tree);
    model::RangeGroup laps = tree->roots[0];
    laps.ranges[1].stint = 5;
    laps.ranges[1].pitStop = true;
    REQUIRE(rule.annotate(layer, std::nullopt, laps, {}));
    CHECK(laps.ranges[1].stint == 0);
    CHECK_FALSE(laps.ranges[1].pitStop);
}

TEST_CASE("beacon-pair annotations: cancellation", "[unit]") {
    TestCache tc;
    const model::DataLayer layer = circuitLayer({10, 70}, 90);
    auto tree = circuit().run(layer, RuleContext{});
    REQUIRE(tree);
    model::RangeGroup laps = tree->roots[0];
    CancelSource cancel;
    cancel.cancel();
    const auto r =
        circuit().annotate(layer, tc.uniform(Rate::fromHz(10), speedWithStops(90, {})), laps, cancel.token());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
}
