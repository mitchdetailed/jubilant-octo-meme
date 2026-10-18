// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-05, the core part (the Lap Editor half is tests/ui/lap_editor): a beacon inserted at the cursor
// (SHIFT+F7, a user marker in the sidecar) and a MarkerEdit offset rebuild the laps of a 60-lap outing
// through DataSource::withSidecar and the circuit rules in under 5 ms; trust flags and alignment offsets
// keyed (groupPath, startMarker) stay on their laps when a beacon is inserted before them; "restore logged"
// deletes the edit, it is never stored (D-065). The 5 ms budget is asserted on the best of several runs in
// optimised builds (as the units.json load budget is) and reported in debug and sanitizer builds.

#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::MarkerClass;

namespace {

constexpr int kLaps = 60;      // flying laps between 61 beacons
constexpr double kLapS = 92.0; // lap time
constexpr double kFirstBeacon = 40.0;
constexpr int kChannels = 200; // a realistic channel count: the snapshot copies every descriptor

double beaconTime(int k) {
    return kFirstBeacon + kLapS * k;
}

model::DataSource outing() {
    const double end = beaconTime(kLaps) + 50.0;
    std::vector<model::Marker> m = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::EOL, sec(end))};
    for (int k = 0; k <= kLaps; ++k) {
        m.push_back(beacon(sec(beaconTime(k)), "B" + std::to_string(k)));
    }
    model::DataLayer layer = layerWith(sec(end), std::move(m));
    for (int c = 0; c < kChannels; ++c) {
        model::Channel ch;
        ch.desc.name = "Channel " + std::to_string(c);
        ch.desc.loggedName = ch.desc.name;
        ch.desc.rate = Rate::fromHz(100);
        layer.addChannel(std::move(ch));
    }
    return sourceWith(std::move(layer));
}

// By value: a reference bound to the result of a call that took a temporary name trips GCC 13's
// -Wdangling-reference.
model::Marker marker(const model::DataLayer& layer, std::string_view name) {
    const auto it = std::find_if(layer.markers.begin(), layer.markers.end(),
                                 [name](const model::Marker& mk) { return mk.name == name; });
    REQUIRE(it != layer.markers.end());
    return *it;
}

const model::Range* lapStartingAt(const model::DataLayer& layer, const model::MarkerId& start) {
    const model::RangeGroup* laps = lapsOf(layer);
    REQUIRE(laps != nullptr);
    for (const model::Range& r : laps->ranges) {
        if (r.startMarker == start) {
            return &r;
        }
    }
    return nullptr;
}

std::size_t flyingLaps(const model::DataLayer& layer) {
    const model::RangeGroup* laps = lapsOf(layer);
    REQUIRE(laps != nullptr);
    return static_cast<std::size_t>(
        std::count_if(laps->ranges.begin(), laps->ranges.end(),
                      [](const model::Range& r) { return r.lapClass == model::LapClass::Flying; }));
}

// The best wall time of `runs` rebuilds of `src` with `doc`, in milliseconds, and the last snapshot.
double bestRebuildMs(const model::DataSource& src, const model::SidecarDocument& doc,
                     const RuleRunner& runner, std::shared_ptr<const model::DataSource>& out, int runs = 15) {
    double best = 1e9;
    for (int i = 0; i < runs; ++i) {
        model::SidecarApplyReport report;
        const auto t0 = std::chrono::steady_clock::now();
        out = src.withSidecar(doc, &report, runner.rebuildFunction());
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void checkBudget(double bestMs, const char* what) {
#if defined(NDEBUG) && !defined(ESSES_FORCE_ASSERTS) && !defined(__SANITIZE_ADDRESS__)
    INFO(what << ": best rebuild " << bestMs << " ms");
    CHECK(bestMs < 5.0);
#else
    WARN(what << ": best rebuild " << bestMs
              << " ms (debug build; the 5 ms budget is asserted in optimised builds)");
#endif
}

} // namespace

TEST_CASE("marker_edit: a beacon inserted before a lap keeps its trust flag and offsets (AC-M2-05)",
          "[unit]") {
    const RuleRunner runner = RuleRunner::circuitDefault();
    model::DataSource src = outing();
    runner.run(src);
    REQUIRE(flyingLaps(src.layers[0]) == kLaps);

    // Lap 30 starts at B29: untrusted, with an alignment offset in both domains.
    const model::MarkerId lap30Start = marker(src.layers[0], "B29").id;
    const model::Range* lap30 = lapStartingAt(src.layers[0], lap30Start);
    REQUIRE(lap30 != nullptr);
    CHECK(lap30->number == 30);
    model::SidecarDocument doc = src.sidecar;
    model::RangeOverride ov;
    ov.groupPath = "Outings:Laps";
    ov.startMarker = lap30Start;
    ov.name = lap30->name;
    ov.trusted = false;
    ov.offsetUs = 125'000;
    ov.offsetM = 4.5;
    doc.setRangeOverride("Normal", ov);
    std::shared_ptr<const model::DataSource> before;
    (void)bestRebuildMs(src, doc, runner, before, 1);
    REQUIRE(lapStartingAt(before->layers[0], lap30Start)->trusted == false);

    // SHIFT+F7 inside lap 10: a user beacon splits it; every later lap is renumbered.
    model::Marker inserted;
    inserted.cls = MarkerClass::BCN;
    inserted.group = "Beacons";
    inserted.t = sec(beaconTime(9) + kLapS / 2.0);
    inserted.name = "Manual.1";
    const model::MarkerId manual = doc.addUserMarker("Normal", inserted);
    std::shared_ptr<const model::DataSource> after;
    const double insertMs = bestRebuildMs(src, doc, runner, after);
    const model::DataLayer& layer = after->layers[0];
    CHECK(flyingLaps(layer) == kLaps + 1);
    CHECK(lapStartingAt(layer, manual) != nullptr);
    const model::Range* moved = lapStartingAt(layer, lap30Start);
    REQUIRE(moved != nullptr);
    CHECK(moved->number == 31);  // renumbered ...
    CHECK_FALSE(moved->trusted); // ... but the override follows (groupPath, startMarker)
    CHECK(moved->offsetUs == 125'000);
    CHECK(moved->offsetM == 4.5);
    std::size_t untrustedFlying = 0;
    for (const model::Range& r : lapsOf(layer)->ranges) {
        untrustedFlying += r.lapClass == model::LapClass::Flying && !r.trusted ? 1 : 0;
    }
    CHECK(untrustedFlying == 1); // no other flying lap picked the override up (out and in laps never trusted)
    checkBudget(insertMs, "beacon insert");
}

TEST_CASE("marker_edit: an offset edit rebuilds the laps and restore logged deletes it (AC-M2-05)",
          "[unit]") {
    const RuleRunner runner = RuleRunner::circuitDefault();
    model::DataSource src = outing();
    runner.run(src);
    const model::Marker b20 = marker(src.layers[0], "B20");
    const model::Range* lap20 = lapStartingAt(src.layers[0], marker(src.layers[0], "B19").id);
    REQUIRE(lap20 != nullptr);
    const Micros lap20End = lap20->end;
    REQUIRE(lap20End == b20.t);

    model::SidecarDocument doc = src.sidecar;
    REQUIRE(
        doc.applyEdit("Normal", model::MarkerEdit{b20.id, model::MarkerEditOp::Offset, -1'500'000, 0.0, "m"})
            .ok());
    std::shared_ptr<const model::DataSource> edited;
    const double offsetMs = bestRebuildMs(src, doc, runner, edited);
    const model::Range* shorter = lapStartingAt(edited->layers[0], marker(src.layers[0], "B19").id);
    REQUIRE(shorter != nullptr);
    CHECK(shorter->end == lap20End - 1'500'000);
    const model::Range* longer = lapStartingAt(edited->layers[0], b20.id);
    REQUIRE(longer != nullptr);
    CHECK(longer->start == lap20End - 1'500'000);
    CHECK(longer->id == lapStartingAt(src.layers[0], b20.id)->id); // identity by start marker (D-014)
    CHECK(edited->layers[0].ranges.rangeVersion("Outings:Laps") >
          src.layers[0].ranges.rangeVersion("Outings:Laps"));
    checkBudget(offsetMs, "offset edit");

    // Restore is a command: the offset edit is deleted from the document, nothing is recorded (D-065).
    REQUIRE(
        doc.applyEdit("Normal", model::MarkerEdit{b20.id, model::MarkerEditOp::Restore, 0, 0.0, "m"}).ok());
    CHECK(doc.layer("Normal").markerEdits.empty());
    const auto restored = src.withSidecar(doc, nullptr, runner.rebuildFunction());
    CHECK(lapStartingAt(restored->layers[0], marker(src.layers[0], "B19").id)->end == lap20End);
    // "Restore logged laps" (06 section 13.3 item 6) removes every edit and user marker of the layer at once.
    REQUIRE(
        doc.applyEdit("Normal", model::MarkerEdit{b20.id, model::MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    model::Marker user;
    user.cls = MarkerClass::BCN;
    user.group = "Beacons";
    user.t = sec(beaconTime(3) + 10.0);
    doc.addUserMarker("Normal", user);
    CHECK(doc.restoreLogged("Normal"));
    CHECK(doc.layer("Normal").markerEdits.empty());
    CHECK(doc.layer("Normal").markers.empty());
    const auto logged = src.withSidecar(doc, nullptr, runner.rebuildFunction());
    CHECK(flyingLaps(logged->layers[0]) == kLaps);
}
