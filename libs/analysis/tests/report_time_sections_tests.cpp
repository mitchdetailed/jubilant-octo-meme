// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M3 item 29 (D-210 (f)): TimeReportEngine fed from TrackSectionsRule output rather than hand-written section
// ranges, so the origin rule of 04 section 13 item 5 and the in-lap rule of D-203 (e) are checked where they
// matter, in the eclectic row. Every expected time below is computed by hand from constant lap speeds on a
// 300 m track (Straight 1 [0, 100 m), Turn 1 [100, 150 m), Straight 2 [150, 300 m)):
//   Out Lap [0, 10 s)      20 m/s, 200 m, measured back from its end beacon (track s = lap s + 100 m):
//                          Straight 1 omitted, Turn 1 [0, 2.5 s) 2.5 s, Straight 2 [2.5, 10 s) 7.5 s
//   Lap 1   [10, 20 s)     30 m/s, 300 m: Straight 1 3.333 s, Turn 1 1.667 s, Straight 2 5 s
//   Lap 2   [20, 30 s)     the same
//   In Lap  [30, 33.6 s)   40 m/s, 144 m into the lap, ending in the pits: Straight 1 [30, 32.5 s) 2.5 s;
//                          Turn 1 would end past the last sample ([32.5, 33.6 s), a short 1.1 s), so it is
//                          no section (D-203 (e)), nor is Straight 2
//   eclectic (untrusted laps included): Straight 1 2.5 s (the in lap's complete section counts), Turn 1
//                          1.667 s (not the in lap's cut-off 1.1 s, nor the out lap's 2.5 s), Straight 2 5 s
//                          (not the out lap's 7.5 s); total 9.167 s
#include "report_test_support.hpp"

#include <esses/analysis/time_report_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;
using model::MarkerClass;

namespace {

TrackSection section(std::string id, std::string name, double startM, double endM, std::string category) {
    TrackSection s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.startM = startM;
    s.endM = endM;
    s.category = std::move(category);
    return s;
}

TrackMap track300() {
    TrackMap m;
    m.lengthM = 300.0;
    m.centre = {{0, 0, 0}, {300, 0, 300}};
    m.sections = {section("s1", "Straight 1", 0.0, 100.0, "straight"),
                  section("t1", "Turn 1", 100.0, 150.0, "corner"),
                  section("s2", "Straight 2", 150.0, 300.0, "straight")};
    return m;
}

// A lap at a constant speed: Corr Dist every 0.1 s from the lap start.
std::shared_ptr<const DistanceIndex> constantSpeed(const model::Range& lap, double speedMps) {
    std::vector<Micros> t;
    std::vector<double> d;
    for (Micros u = lap.start; u < lap.end; u += 100'000) {
        t.push_back(u);
        d.push_back(speedMps * static_cast<double>(u - lap.start) * 1e-6);
    }
    t.push_back(lap.end);
    d.push_back(speedMps * static_cast<double>(lap.end - lap.start) * 1e-6);
    DistanceIndexKey key;
    key.start = lap.start;
    key.end = lap.end;
    key.startMarker = lap.startMarker;
    auto idx = DistanceIndex::fromSamples(t, d, key);
    REQUIRE(idx);
    return std::make_shared<const DistanceIndex>(std::move(idx).value());
}

// The lap indexes of the outing above, by lap start.
LapIndex lookup(const model::Range& lap) {
    LapIndex out;
    out.startsAtBeacon = lap.lapClass != model::LapClass::Out && lap.lapClass != model::LapClass::Partial;
    out.endsAtBeacon = lap.lapClass != model::LapClass::In && lap.lapClass != model::LapClass::Partial;
    if (lap.start == sec(0)) {
        out.index = constantSpeed(lap, 20.0);
    } else if (lap.start == sec(30)) {
        out.index = constantSpeed(lap, 40.0);
    } else {
        out.index = constantSpeed(lap, 30.0);
    }
    return out;
}

// SOL 0, BCN 10 s, 20 s, 30 s, EOL 33.6 s: phase 1 of the circuit rules, then phase 2 with the
// track-sections rule over track300().
std::shared_ptr<const model::DataSource> sectionedOuting() {
    std::vector<model::Marker> m = {intrinsic(MarkerClass::SOL, 0), intrinsic(MarkerClass::EOL, sec(33.6))};
    for (const double b : {10.0, 20.0, 30.0}) {
        m.push_back(beacon(sec(b), "B" + std::to_string(static_cast<int>(b))));
    }
    model::DataLayer layer = layerWith(sec(33.6), std::move(m));
    layer.name = "Normal";
    model::DataSource src = sourceWith(std::move(layer), {detailText("Log Date", "10/10/2026")});
    src.id = hash128(std::string_view("report-time-sections"));
    const RuleRunner circuit = RuleRunner::circuitDefault();
    std::vector<std::shared_ptr<const IRangeRule>> rules(circuit.rules().begin(), circuit.rules().end());
    rules.push_back(std::make_shared<const TrackSectionsRule>());
    const RuleRunner runner(rules);
    runner.run(src);
    auto phase2 = runner.runPhase2(src, "Normal", std::nullopt, std::make_shared<const TrackMap>(track300()),
                                   lookup, {});
    REQUIRE(phase2);
    src.layers[0].ranges = std::move(phase2).value();
    return std::make_shared<const model::DataSource>(std::move(src));
}

// Every lap of the outing as a row, in time order.
ReportInputs everyLap(const std::shared_ptr<const model::DataSource>& src) {
    ReportInputs in;
    const model::RangeGroup* laps = src->layers.front().ranges.find(kLaps);
    REQUIRE(laps != nullptr);
    for (const model::Range& lap : laps->ranges) {
        ReportSource s;
        s.source = src;
        s.range.src = src->id;
        s.range.layer = "Normal";
        s.range.groupPath = std::string(kLaps);
        s.range.range = lap.id;
        s.range.name = lap.name;
        in.sources.push_back(std::move(s));
    }
    return in;
}

bool near(std::optional<Micros> got, double seconds) {
    return got.has_value() && std::llabs(*got - sec(seconds)) <= 2;
}

} // namespace

TEST_CASE("report_time: TrackSectionsRule output reaches the eclectic without the in lap's cut-off section",
          "[unit]") {
    const auto src = sectionedOuting();
    const model::RangeGroup* laps = src->layers.front().ranges.find(kLaps);
    REQUIRE(laps != nullptr);
    REQUIRE(laps->ranges.size() == 4);
    CHECK_FALSE(laps->ranges.front().trusted); // the out and in laps are untrusted by default
    CHECK_FALSE(laps->ranges.back().trusted);

    TimeReportSpec spec;
    spec.includeUntrusted = true; // the bogus times would only reach the summaries this way
    const Result<TimeReportTable> built = TimeReportEngine::build(spec, everyLap(src), {});
    REQUIRE(built);
    const TimeReportTable& t = *built;
    CHECK(t.sections == std::vector<std::string>{"Straight 1", "Turn 1", "Straight 2"});
    REQUIRE(t.rows.size() == 4);
    REQUIRE(t.rows[0].cells.size() == 4);
    // The out lap: no Straight 1 (it starts before the lap), Turn 1 and Straight 2 measured back.
    CHECK_FALSE(t.rows[0].cells[0].time.has_value());
    CHECK(near(t.rows[0].cells[1].time, 2.5));
    CHECK(near(t.rows[0].cells[2].time, 7.5));
    CHECK(near(t.rows[0].cells[3].time, 10.0));
    // A flying lap.
    CHECK(near(t.rows[1].cells[0].time, 100.0 / 30.0));
    CHECK(near(t.rows[1].cells[1].time, 50.0 / 30.0));
    CHECK(near(t.rows[1].cells[2].time, 5.0));
    // The in lap: its complete Straight 1 only; Turn 1 is missing, not a short 1.1 s.
    const TimeReportRow& in = t.rows[3];
    CHECK(near(in.cells[0].time, 2.5));
    CHECK_FALSE(in.cells[1].time.has_value());
    CHECK_FALSE(in.cells[2].time.has_value());
    CHECK(near(in.cells[3].time, 3.6));

    REQUIRE(t.eclectic.has_value());
    if (t.eclectic) {
        const TimeReportRow& best = *t.eclectic;
        REQUIRE(best.cells.size() == 4);
        CHECK(near(best.cells[0].time, 2.5));         // the in lap's Straight 1
        CHECK(near(best.cells[1].time, 50.0 / 30.0)); // a flying lap's Turn 1
        CHECK(near(best.cells[2].time, 5.0));         // a flying lap's Straight 2
        CHECK(near(best.cells[3].time, 2.5 + 50.0 / 30.0 + 5.0));
    }
    // The fastest marks follow: Straight 1 is the in lap's, Turn 1 the flying laps' (not the out lap's).
    CHECK(in.cells[0].fastest);
    CHECK(t.rows[1].cells[1].fastest);
    CHECK_FALSE(t.rows[0].cells[1].fastest);

    // Untrusted laps excluded: the flying laps alone, so the eclectic Straight 1 is theirs.
    spec.includeUntrusted = false;
    const Result<TimeReportTable> trusted = TimeReportEngine::build(spec, everyLap(src), {});
    REQUIRE(trusted);
    REQUIRE(trusted->rows.size() == 2);
    const std::optional<TimeReportRow>& eclectic = trusted->eclectic;
    REQUIRE(eclectic.has_value());
    if (eclectic) {
        REQUIRE(eclectic->cells.size() == 4);
        CHECK(near(eclectic->cells[0].time, 100.0 / 30.0));
        CHECK(near(eclectic->cells[3].time, 10.0));
    }
}
