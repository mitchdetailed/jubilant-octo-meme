// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RuleRunner (02 section 4.4 rule 13, D-140): phase 1 over every layer of a source with the sidecar's range
// overrides and per-path versions, phase 2 for one layer, the rebuild function DataSource::withSidecar takes.
#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::MarkerClass;

namespace {

model::DataLayer circuitLayer(std::string name, std::vector<double> beacons, double end) {
    std::vector<model::Marker> m = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::EOL, sec(end))};
    for (std::size_t i = 0; i < beacons.size(); ++i) {
        m.push_back(beacon(sec(beacons[i]), "B" + std::to_string(i + 1)));
    }
    model::DataLayer layer = layerWith(sec(end), std::move(m));
    layer.name = std::move(name);
    return layer;
}

model::MarkerId markerId(const model::DataLayer& layer, const std::string& name) {
    for (const model::Marker& m : layer.markers) {
        if (m.name == name) {
            return m.id;
        }
    }
    FAIL("no marker " << name);
    return {};
}

} // namespace

TEST_CASE("rule runner: the circuit rules build Outings and Outings:Laps per layer", "[unit][AC-M1-07]") {
    model::DataSource src = sourceWith(circuitLayer("Normal", {10, 30}, 50));
    src.layers.push_back(circuitLayer("Fastest", {10, 30}, 50));
    src.layers.push_back(circuitLayer("Burst0", {10, 30}, 50));
    const RuleRunner runner = RuleRunner::circuitDefault();
    REQUIRE(runner.rules().size() == 2);
    const model::SidecarApplyReport report = runner.run(src);
    CHECK(report.notes.empty());
    for (const model::DataLayer& layer : src.layers) {
        INFO(layer.name);
        const model::RangeGroup* outings = layer.ranges.find("Outings");
        REQUIRE(outings != nullptr);
        REQUIRE(outings->ranges.size() == 1);
        CHECK(outings->ranges[0].name == "test.ld");
        CHECK(outings->version == 1);
        const model::RangeGroup* laps = lapsOf(layer);
        if (layer.name == "Burst0") {
            CHECK(laps == nullptr); // the beacon-pair rule names Normal and Fastest only
            CHECK(outings->children.empty());
        } else {
            REQUIRE(laps != nullptr);
            CHECK(laps->ranges.size() == 3);
            CHECK(laps->version == 1);
            REQUIRE(outings->children.size() == 1);
        }
    }
    // A log without beacons keeps its whole outing only (D-104).
    model::DataSource plain = sourceWith(circuitLayer("Normal", {}, 50));
    runner.run(plain);
    REQUIRE(plain.layers[0].ranges.roots.size() == 1);
    CHECK(plain.layers[0].ranges.roots[0].children.empty());

    // No rules: empty trees.
    model::DataSource none = sourceWith(circuitLayer("Normal", {10}, 50));
    RuleRunner{}.run(none);
    CHECK(none.layers[0].ranges.roots.empty());
}

TEST_CASE("rule runner: range overrides apply by start marker and name", "[unit][AC-M1-07]") {
    model::DataSource src = sourceWith(circuitLayer("Normal", {10, 30, 55}, 70));
    const model::MarkerId b1 = markerId(src.layers[0], "B1");
    model::RangeOverride byId;
    byId.groupPath = "Outings:Laps";
    byId.startMarker = b1;
    byId.trusted = false;
    byId.offsetUs = 250'000;
    src.sidecar.setRangeOverride("Normal", byId);
    model::RangeOverride byName;
    byName.groupPath = "Outings:Laps";
    byName.name = "In Lap";
    byName.trusted = true;
    src.sidecar.setRangeOverride("Normal", byName);

    const model::SidecarApplyReport report = RuleRunner::circuitDefault().run(src);
    const model::RangeGroup* laps = lapsOf(src.layers[0]);
    REQUIRE(laps != nullptr);
    REQUIRE(laps->ranges.size() == 4);
    CHECK_FALSE(laps->ranges[1].trusted);
    CHECK(laps->ranges[1].offsetUs == 250'000);
    CHECK(laps->ranges[3].trusted); // the In Lap, matched by name
    CHECK(report.has(model::kNoteRangeOverrideByName));
    CHECK(laps->ranges[1].lapClass == model::LapClass::Flying); // overrides never change the class
}

TEST_CASE("rule runner: versions bump per changed group path, never for annotations (D-140)",
          "[unit][AC-M1-07]") {
    TestCache tc;
    model::DataSource src = sourceWith(circuitLayer("Normal", {10, 70, 130}, 150));
    const RuleRunner runner = RuleRunner::circuitDefault();
    runner.run(src);
    model::DataLayer& layer = src.layers[0];
    REQUIRE(lapsOf(layer) != nullptr);
    CHECK(layer.ranges.rangeVersion("Outings") == 1);
    CHECK(layer.ranges.rangeVersion("Outings:Laps") == 1);

    // Phase 2 annotates (L2 [70,130) holds a 20 s stop) and keeps the versions.
    std::vector<double> v(1500, 40.0);
    for (std::size_t i = 800; i < 1000; ++i) {
        v[i] = 0.0;
    }
    const std::optional<store::Series> speed = tc.uniform(Rate::fromHz(10), v);
    auto annotated = runner.runPhase2(src, "Normal", speed, {});
    REQUIRE(annotated);
    const model::RangeGroup* laps2 = annotated->find("Outings:Laps");
    REQUIRE(laps2 != nullptr);
    CHECK(laps2->ranges[2].pitStop);
    CHECK(laps2->ranges[3].stint == 2);
    CHECK(annotated->rangeVersion("Outings:Laps") == 1);
    for (std::size_t i = 0; i < laps2->ranges.size(); ++i) {
        model::Range a = laps2->ranges[i];
        a.stint = 0;
        a.pitStop = false;
        CHECK(a == lapsOf(layer)->ranges[i]); // ids, classes, numbers, names, trust unchanged
    }
    layer.ranges = std::move(annotated).value();

    // A phase-1 rerun with unchanged markers keeps the annotations and every version.
    runner.run(src);
    CHECK(layer.ranges.rangeVersion("Outings") == 1);
    CHECK(layer.ranges.rangeVersion("Outings:Laps") == 1);
    CHECK(lapsOf(layer)->ranges[2].pitStop);
    CHECK(lapsOf(layer)->ranges[3].stint == 2);

    // Moving B2 changes the laps (version 2) but not the outing; the moved laps lose their stale annotations.
    for (model::Marker& m : layer.markers) {
        if (m.name == "B2") {
            m.originalT = m.t;
            m.t += sec(2);
        }
    }
    model::sortMarkers(layer.markers);
    runner.run(src);
    CHECK(layer.ranges.rangeVersion("Outings") == 1);
    CHECK(layer.ranges.rangeVersion("Outings:Laps") == 2);
    CHECK(lapsOf(layer)->ranges[1].stint == 0); // L1 changed its end: annotations wait for phase 2
    CHECK_FALSE(lapsOf(layer)->ranges[2].pitStop);
    CHECK(lapsOf(layer)->ranges[0].stint == 1); // the out lap is unchanged
}

TEST_CASE("rule runner: the rebuild function serves DataSource::withSidecar", "[unit][AC-M1-07]") {
    model::DataSource src = sourceWith(circuitLayer("Normal", {10, 30, 55}, 70));
    const RuleRunner runner = RuleRunner::circuitDefault();
    runner.run(src);
    const model::MarkerId b2 = markerId(src.layers[0], "B2");

    model::SidecarDocument doc = src.sidecar;
    REQUIRE(doc.applyEdit("Normal", model::MarkerEdit{b2, model::MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    model::SidecarApplyReport report;
    const auto next = src.withSidecar(doc, &report, runner.rebuildFunction());
    REQUIRE(next != nullptr);
    const model::RangeGroup* laps = lapsOf(next->layers[0]);
    REQUIRE(laps != nullptr);
    REQUIRE(laps->ranges.size() == 3); // B2 ignored: out, L1 [10, 55), in
    CHECK(laps->ranges[1].end == sec(55));
    CHECK(laps->ranges[1].id == lapsOf(src.layers[0])->ranges[1].id);
    CHECK(next->layers[0].ranges.rangeVersion("Outings:Laps") == 2);
    CHECK(next->layers[0].ranges.rangeVersion("Outings") == 1);
}

TEST_CASE("rule runner: phase 2 errors", "[unit]") {
    model::DataSource src = sourceWith(circuitLayer("Normal", {10, 30}, 50));
    const RuleRunner runner = RuleRunner::circuitDefault();
    runner.run(src);
    auto missing = runner.runPhase2(src, "Burst9", std::nullopt, {});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    // A layer without a laps group: the tree comes back unchanged.
    model::DataSource plain = sourceWith(circuitLayer("Normal", {}, 50));
    runner.run(plain);
    auto same = runner.runPhase2(plain, "", std::nullopt, {});
    REQUIRE(same);
    CHECK(*same == plain.layers[0].ranges);
}
