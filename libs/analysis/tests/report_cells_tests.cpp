// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportEngine::cells (04 section 12 items 2-6, AC-M3-03): rows by ordered headers, untrusted exclusion,
// overlays, Δ vs Overlay 0, display units, zoom clip, Pending / Missing / Error cells, min/max marks and the
// StatCache, against hand-computed tables.
//
// The fixture outing (40 s, 10 Hz "Speed" in m/s, displayed in km/h = 3.6 x):
//   Out Lap [0, 10 s)  untrusted   50 m/s throughout
//   Lap 1   [10, 20 s)             samples 100..199 = 0, 1, ..., 99
//   Lap 2   [20, 30 s)             samples 200..299 = 0, 2, ..., 198 with 200..202 invalid
//   In Lap  [30, 40 s) untrusted   30 m/s throughout
// Sections: T1 = the first 4 s and S1 = the rest of each flying lap; the out lap has only S1 (backwards
// origin), the in lap only T1.
//   Lap 1: avg 49.5, min 0, max 99, start 0, end 99, stdDev sqrt((100^2 - 1) / 12) = sqrt(833.25)
//   Lap 2: valid 6, 8, ..., 198 (97 samples): avg 102, min 6, max 198, start 6 (probe: 0.3 s in), end 198,
//          stdDev 2 sqrt((97^2 - 1) / 12) = 2 x 28 = 56
//   Lap 1 T1 [10, 14 s) = 0..39: avg 19.5; Lap 1 S1 [14, 20 s) = 40..99: avg 69.5
//   Lap 2 T1 [20, 24 s) = 6..78 step 2: avg 42; Lap 2 S1 [24, 30 s) = 80..198 step 2: avg 139
#include "report_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/store/memory_provider.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

constexpr double kKmh = 3.6;

std::vector<RangeSpec> fixtureLaps() {
    return {{"Out Lap", 0, 10, false},
            {"Lap 1", 10, 20, true},
            {"Lap 2", 20, 30, true},
            {"In Lap", 30, 40, false}};
}

std::vector<RangeSpec> fixtureSections() {
    return {{"S1", 6, 10, true},  {"T1", 10, 14, true}, {"S1", 14, 20, true},
            {"T1", 20, 24, true}, {"S1", 24, 30, true}, {"T1", 30, 34, true}};
}

store::UniformSeries speedSeries(TestCache& tc, double offset = 0.0) {
    std::vector<double> v(400);
    std::vector<bool> valid(400, true);
    for (std::size_t i = 0; i < 400; ++i) {
        if (i < 100) {
            v[i] = 50.0;
        } else if (i < 200) {
            v[i] = static_cast<double>(i - 100);
        } else if (i < 300) {
            v[i] = 2.0 * static_cast<double>(i - 200);
        } else {
            v[i] = 30.0;
        }
        v[i] += offset;
    }
    valid[200] = false;
    valid[201] = false;
    valid[202] = false;
    return tc.uniform(Rate::fromHz(10), v, valid);
}

struct Fixture {
    TestCache tc;
    std::shared_ptr<const model::DataSource> main;
    std::shared_ptr<const model::DataSource> overlay;
    ChannelTable channels;
    StatCache cache;

    Fixture() {
        main = reportSource("main", 40.0, fixtureLaps(), fixtureSections(),
                            {detailText("Log Date", "10/10/2026"), detailText("Log Time", "14:00:00"),
                             detailText("Driver", "Alice")});
        // The overlay: the same laps 10 m/s faster, logged the day before.
        overlay = reportSource("overlay", 40.0, fixtureLaps(), fixtureSections(),
                               {detailText("Log Date", "09/10/2026"), detailText("Log Time", "09:30:00"),
                                detailText("Driver", "Bob")});
        channels.add(main, "Speed", speedSeries(tc), units::Converter{kKmh, 0.0}, "km/h", 1);
        channels.add(overlay, "Speed", speedSeries(tc, 10.0), units::Converter{kKmh, 0.0}, "km/h", 1);
    }

    ReportInputs inputs(std::vector<ReportSource> sources) const {
        return ReportInputs{std::move(sources), channels.lookup()};
    }
};

ReportSpec speedSpec(std::vector<ReportStat> stats, std::vector<std::string> headers = {"Outings:Laps"}) {
    ReportSpec spec;
    spec.scope = ReportScope::SelectedOutings;
    spec.headers = std::move(headers);
    spec.channels = {ReportChannel{"Speed", std::move(stats)}};
    return spec;
}

ReportTable run(const ReportSpec& spec, const ReportInputs& inputs, StatCache& cache) {
    Result<ReportTable> t = ReportEngine::cells(spec, inputs, cache, {});
    REQUIRE(t.ok());
    return std::move(t).value();
}

std::vector<std::vector<std::string>> paths(const ReportTable& t) {
    std::vector<std::vector<std::string>> out;
    out.reserve(t.rows.size());
    for (const ReportHeading& h : t.rows) {
        out.push_back(h.path);
    }
    return out;
}

} // namespace

TEST_CASE("report_cells: laps of an outing with every statistic in display units", "[unit]") {
    Fixture f;
    const ReportSpec spec =
        speedSpec({ReportStat::Avg, ReportStat::Min, ReportStat::Max, ReportStat::AbsMax, ReportStat::Start,
                   ReportStat::End, ReportStat::Change, ReportStat::StdDev, ReportStat::Delta});
    const ReportTable t = run(spec, f.inputs({sourceRange(f.main, kOutings, "test.ld")}), f.cache);
    // The untrusted out and in laps are excluded.
    REQUIRE(paths(t) == std::vector<std::vector<std::string>>{{"Lap 1"}, {"Lap 2"}});
    REQUIRE(t.columns.size() == 9);
    CHECK(t.columns[0].unit == "km/h");
    CHECK(t.columns[0].dps == 1);
    CHECK(t.columns[7].stat == ReportStat::StdDev);
    CHECK_FALSE(t.transposed);
    const double lap1[] = {49.5, 0, 99, 99, 0, 99, 99, std::sqrt(833.25), 99};
    const double lap2[] = {102, 6, 198, 198, 6, 198, 192, 56, 192};
    for (std::size_t c = 0; c < 9; ++c) {
        INFO("column " << c);
        CHECK(t.at(0, c).state == ReportCellState::Ok);
        CHECK(t.at(0, c).value == Catch::Approx(lap1[c] * kKmh).epsilon(1e-12).margin(1e-12));
        CHECK(t.at(1, c).value == Catch::Approx(lap2[c] * kKmh).epsilon(1e-12).margin(1e-12));
    }
    // Min/max marks per column: Lap 1 has the smaller values everywhere.
    CHECK(t.at(0, 0).isMin);
    CHECK(t.at(1, 0).isMax);
    CHECK_FALSE(t.at(0, 0).isMax);
    CHECK(t.pending() == 0);
    CHECK(t.rows[0].trusted);
    CHECK(t.rows[0].source == 0);
}

TEST_CASE("report_cells: untrusted laps only with includeUntrusted, marked", "[unit]") {
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg});
    spec.options.includeUntrusted = true;
    const ReportTable t = run(spec, f.inputs({sourceRange(f.main, kOutings, "test.ld")}), f.cache);
    REQUIRE(paths(t) == std::vector<std::vector<std::string>>{{"Out Lap"}, {"Lap 1"}, {"Lap 2"}, {"In Lap"}});
    CHECK_FALSE(t.rows[0].trusted);
    CHECK(t.rows[1].trusted);
    CHECK_FALSE(t.rows[3].trusted);
    CHECK(t.at(0, 0).value == Catch::Approx(50.0 * kKmh));
    CHECK(t.at(3, 0).value == Catch::Approx(30.0 * kKmh));
    // A selected untrusted lap is excluded as well unless included.
    spec.options.includeUntrusted = false;
    spec.scope = ReportScope::SelectedRanges;
    spec.headers.clear();
    const ReportTable selected =
        run(spec, f.inputs({sourceRange(f.main, kLaps, "Out Lap"), sourceRange(f.main, kLaps, "Lap 2")}),
            f.cache);
    REQUIRE(paths(selected) == std::vector<std::vector<std::string>>{{"Lap 2"}});
    CHECK(selected.at(0, 0).value == Catch::Approx(102.0 * kKmh));
}

TEST_CASE("report_cells: headers divide in any order", "[unit]") {
    Fixture f;
    const auto outing = f.inputs({sourceRange(f.main, kOutings, "test.ld")});
    const ReportTable lapsThenSections =
        run(speedSpec({ReportStat::Avg}, {std::string(kLaps), std::string(kSections)}), outing, f.cache);
    REQUIRE(paths(lapsThenSections) ==
            std::vector<std::vector<std::string>>{
                {"Lap 1", "T1"}, {"Lap 1", "S1"}, {"Lap 2", "T1"}, {"Lap 2", "S1"}});
    CHECK(lapsThenSections.at(0, 0).value == Catch::Approx(19.5 * kKmh));
    CHECK(lapsThenSections.at(1, 0).value == Catch::Approx(69.5 * kKmh));
    CHECK(lapsThenSections.at(2, 0).value == Catch::Approx(42.0 * kKmh));
    CHECK(lapsThenSections.at(3, 0).value == Catch::Approx(139.0 * kKmh));

    // Sections first: the corners of every lap, then the straights (the out lap's S1 is untrusted: out).
    const ReportTable sectionsThenLaps =
        run(speedSpec({ReportStat::Avg}, {std::string(kSections), std::string(kLaps)}), outing, f.cache);
    REQUIRE(paths(sectionsThenLaps) ==
            std::vector<std::vector<std::string>>{
                {"T1", "Lap 1"}, {"T1", "Lap 2"}, {"S1", "Lap 1"}, {"S1", "Lap 2"}});
    CHECK(sectionsThenLaps.at(1, 0).value == Catch::Approx(42.0 * kKmh));

    // An outer level that does not divide: the outing, and the Driver detail (no such range group).
    const ReportTable labelled = run(
        speedSpec({ReportStat::Max}, {"Driver", std::string(kOutings), std::string(kLaps)}), outing, f.cache);
    REQUIRE(paths(labelled) == std::vector<std::vector<std::string>>{{"Alice", "test.ld", "Lap 1"},
                                                                     {"Alice", "test.ld", "Lap 2"}});
    // Without headers a row is the source range itself.
    const ReportTable whole = run(speedSpec({ReportStat::Min}, {}), outing, f.cache);
    REQUIRE(paths(whole) == std::vector<std::vector<std::string>>{{"test.ld"}});
    CHECK(whole.at(0, 0).value == 0.0);
}

TEST_CASE("report_cells: overlays merge under one heading in date order; delta versus Overlay 0", "[unit]") {
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg, ReportStat::Max});
    spec.deltaVsOverlay0 = true;
    const auto inputs = f.inputs({sourceRange(f.main, kOutings, "test.ld"),
                                  sourceRange(f.overlay, kOutings, "test.ld", ReportSlotRole::Overlay0)});
    const ReportTable t = run(spec, inputs, f.cache);
    // The overlay was logged first, so its Lap 1 opens the heading; each Lap n row of Main follows its
    // overlay row.
    REQUIRE(paths(t) == std::vector<std::vector<std::string>>{{"Lap 1"}, {"Lap 1"}, {"Lap 2"}, {"Lap 2"}});
    CHECK(t.rows[0].role == ReportSlotRole::Overlay0);
    CHECK(t.rows[0].source == 1);
    CHECK(t.rows[1].role == ReportSlotRole::Main);
    REQUIRE(t.columns.size() == 4);
    CHECK_FALSE(t.columns[0].deltaVsOverlay0);
    CHECK(t.columns[1].deltaVsOverlay0);
    CHECK(t.columns[1].stat == ReportStat::Avg);
    CHECK(t.columns[3].stat == ReportStat::Max);
    // Overlay rows: blank deltas. Main rows: Main - Overlay 0 = -10 m/s.
    CHECK(t.at(0, 1).state == ReportCellState::Blank);
    CHECK(t.at(1, 0).value == Catch::Approx(49.5 * kKmh));
    CHECK(t.at(1, 1).value == Catch::Approx(-10.0 * kKmh));
    CHECK(t.at(1, 3).value == Catch::Approx(-10.0 * kKmh));
    CHECK(t.at(3, 1).value == Catch::Approx(-10.0 * kKmh));

    // excludeOverlays hides the Overlay 0 rows but keeps the delta.
    spec.options.excludeOverlays = true;
    const ReportTable hidden = run(spec, inputs, f.cache);
    REQUIRE(paths(hidden) == std::vector<std::vector<std::string>>{{"Lap 1"}, {"Lap 2"}});
    CHECK(hidden.at(0, 1).value == Catch::Approx(-10.0 * kKmh));

    // A numbered overlay is never the delta reference (no D-017 fallback): blank deltas.
    spec.options.excludeOverlays = false;
    const ReportTable numbered =
        run(spec,
            f.inputs({sourceRange(f.main, kOutings, "test.ld"),
                      sourceRange(f.overlay, kOutings, "test.ld", ReportSlotRole::Overlay)}),
            f.cache);
    REQUIRE(numbered.rows.size() == 4);
    for (std::size_t r = 0; r < 4; ++r) {
        CHECK(numbered.at(r, 1).state == ReportCellState::Blank);
    }
    // Overlays excluded without a delta: Main only.
    spec.deltaVsOverlay0 = false;
    spec.options.excludeOverlays = true;
    CHECK(run(spec, inputs, f.cache).rows.size() == 2);
}

TEST_CASE("report_cells: zoomLinked clips every range to the zoom span", "[unit]") {
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg, ReportStat::Start, ReportStat::End}, {});
    spec.options.zoomLinked = true;
    ReportSource lap1 = sourceRange(f.main, kLaps, "Lap 1");
    lap1.clip = std::make_pair(sec(12.0), sec(15.0)); // samples 120..149 = 20..49
    const ReportTable t = run(spec, f.inputs({lap1}), f.cache);
    REQUIRE(t.rows.size() == 1);
    CHECK(t.at(0, 0).value == Catch::Approx(34.5 * kKmh));
    CHECK(t.at(0, 1).value == Catch::Approx(20.0 * kKmh));
    CHECK(t.at(0, 2).value == Catch::Approx(49.0 * kKmh));
    // Not zoom-linked: the clip is ignored.
    spec.options.zoomLinked = false;
    CHECK(run(spec, f.inputs({lap1}), f.cache).at(0, 0).value == Catch::Approx(49.5 * kKmh));
}

TEST_CASE("report_cells: Pending, Missing and Error channels; an affine display unit", "[unit]") {
    Fixture f;
    // A temperature in deg C shown in deg F: values convert with the offset, differences with the scale only.
    std::vector<double> temp(400, 20.0);
    for (std::size_t i = 100; i < 200; ++i) {
        temp[i] = i < 150 ? 10.0 : 30.0; // Lap 1: avg 20, stdDev 10, delta 20, change 20
    }
    f.channels.add(f.main, "Temp", f.tc.uniform(Rate::fromHz(10), temp), units::Converter{1.8, 32.0}, "degF");
    f.channels.addStatus(f.main, "Busy", model::ChannelStatus::Pending);
    f.channels.addStatus(f.main, "Broken", model::ChannelStatus::Error);
    ReportSpec spec;
    spec.headers = {};
    spec.channels = {
        ReportChannel{"Temp", {ReportStat::Avg, ReportStat::StdDev, ReportStat::Delta, ReportStat::Change}},
        ReportChannel{"Busy", {ReportStat::Avg}}, ReportChannel{"Ghost", {ReportStat::Max}},
        ReportChannel{"Broken", {ReportStat::Min}}, ReportChannel{"Speed", {}}};
    const ReportTable t = run(spec, f.inputs({sourceRange(f.main, kLaps, "Lap 1")}), f.cache);
    REQUIRE(t.columns.size() == 7);
    CHECK(t.at(0, 0).value == Catch::Approx(20.0 * 1.8 + 32.0));
    CHECK(t.at(0, 1).value == Catch::Approx(10.0 * 1.8));
    CHECK(t.at(0, 2).value == Catch::Approx(20.0 * 1.8));
    CHECK(t.at(0, 3).value == Catch::Approx(20.0 * 1.8));
    CHECK(t.columns[0].unit == "degF");
    CHECK(t.at(0, 4).state == ReportCellState::Pending);
    CHECK(t.at(0, 5).state == ReportCellState::Missing);
    CHECK(t.at(0, 6).state == ReportCellState::Error);
    CHECK(t.pending() == 1);
}

TEST_CASE("report_cells: the StatCache serves a second run and keys the options", "[unit]") {
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg, ReportStat::Start});
    const auto inputs = f.inputs({sourceRange(f.main, kOutings, "test.ld")});
    const ReportTable first = run(spec, inputs, f.cache);
    CHECK(f.cache.size() == 4); // 2 laps x 2 statistics
    const ReportTable second = run(spec, inputs, f.cache);
    CHECK(f.cache.size() == 4);
    REQUIRE(second.cells.size() == first.cells.size());
    for (std::size_t i = 0; i < first.cells.size(); ++i) {
        CHECK(second.cells[i] == first.cells[i]);
    }
    // The probe changes start/end/change only: two new cells.
    spec.options.probeSeconds = 0.1;
    const ReportTable narrow = run(spec, inputs, f.cache);
    CHECK(f.cache.size() == 6);
    CHECK(narrow.at(1, 1).state == ReportCellState::Blank); // Lap 2's first valid sample is 0.3 s in
    CHECK(narrow.at(1, 0).value == first.at(1, 0).value);
    // A channel without a descriptor is computed but never cached.
    ReportChannelData bare;
    bare.series = speedSeries(f.tc);
    ReportInputs noDescriptor = inputs;
    noDescriptor.channels = [&bare](const ReportSource&, std::string_view) { return bare; };
    StatCache other;
    const ReportTable uncached = run(spec, noDescriptor, other);
    CHECK(other.size() == 0);
    CHECK(uncached.at(0, 0).value == 49.5);
}

TEST_CASE("report_cells: errors, cancellation and the layout flag", "[unit]") {
    Fixture f;
    ReportSpec empty;
    const Result<ReportTable> invalid = ReportEngine::cells(empty, f.inputs({}), f.cache, {});
    REQUIRE_FALSE(invalid.ok());
    CHECK(invalid.error().code == ErrorCode::Invalid);

    ReportSpec spec = speedSpec({ReportStat::Avg});
    CancelSource cancel;
    cancel.cancel();
    const Result<ReportTable> cancelled = ReportEngine::cells(
        spec, f.inputs({sourceRange(f.main, kOutings, "test.ld")}), f.cache, cancel.token());
    REQUIRE_FALSE(cancelled.ok());
    CHECK(cancelled.error().code == ErrorCode::Cancelled);

    spec.layout = ReportLayout::ChannelsInColumns;
    CHECK(run(spec, f.inputs({sourceRange(f.main, kOutings, "test.ld")}), f.cache).transposed);
    // A source whose range does not resolve gives no rows; no sources, no rows.
    ReportSource stale = sourceRange(f.main, kLaps, "Lap 1");
    stale.range.range = model::RangeId{};
    stale.range.name = "Lap 9";
    CHECK(run(spec, f.inputs({stale}), f.cache).rows.empty());
    const ReportTable none = run(spec, f.inputs({}), f.cache);
    CHECK(none.rows.empty());
    CHECK(none.columns.size() == 1);
}

TEST_CASE("report_cells: chunks prefetched on a pool give the same table", "[unit]") {
    // 8 laps of 30 s at 1 kHz (240,000 samples, four chunks) with an invalid run; the columns of one copy
    // schedule their chunk tasks on a pool, so the scans meet chunks prefetched, being built or still queued.
    const std::size_t n = 240'000;
    std::vector<double> v(n);
    std::vector<bool> valid(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = std::sin(static_cast<double>(i) * 1e-3) * 50.0 + static_cast<double>(i % 7);
        valid[i] = i % 30'000 >= 20;
    }
    std::vector<RangeSpec> laps;
    laps.reserve(8);
    for (int l = 0; l < 8; ++l) {
        laps.push_back(RangeSpec{"Lap " + std::to_string(l + 1), 30.0 * l, 30.0 * (l + 1), true});
    }
    const auto src = reportSource("pool", 240.0, laps, {});
    const auto provider = store::InMemoryProvider::fromF64(v, valid, "test:pool");
    store::ChunkCache serialCache;
    store::ChunkCache pooledCache;
    TaskPool pool(3); // after the caches: joined before they go
    store::ColumnOptions serialOptions;
    serialOptions.cache = &serialCache;
    store::ColumnOptions pooledOptions;
    pooledOptions.cache = &pooledCache;
    pooledOptions.pool = &pool;
    ReportSpec spec = speedSpec({ReportStat::Avg, ReportStat::Min, ReportStat::Max, ReportStat::Start,
                                 ReportStat::End, ReportStat::StdDev});
    std::vector<ReportTable> tables;
    tables.reserve(2);
    for (const store::ColumnOptions* options : {&serialOptions, &pooledOptions}) {
        ChannelTable channels;
        channels.add(src, "Speed",
                     store::UniformSeries{Rate::fromHz(1000), 0, store::Column(provider, *options)});
        StatCache cache;
        tables.push_back(
            run(spec, ReportInputs{{sourceRange(src, kOutings, "test.ld")}, channels.lookup()}, cache));
    }
    REQUIRE(tables[0].rows.size() == 8);
    REQUIRE(tables[1].cells.size() == tables[0].cells.size());
    for (std::size_t i = 0; i < tables[0].cells.size(); ++i) {
        CHECK(tables[1].cells[i] == tables[0].cells[i]);
    }
    // The first 20 samples of each lap are invalid: Lap 2 starts with sample 30,020.
    CHECK(tables[0].at(1, 3).value ==
          Catch::Approx(std::sin(30'020.0 * 1e-3) * 50.0 + static_cast<double>(30'020 % 7)));
    pool.join();
}

TEST_CASE("report_cells: delta pairs Main with Overlay 0 below the compared range (D-197 (c))", "[unit]") {
    // Main is Lap 2 of the main log, Overlay 0 Lap 1 of the overlay (10 m/s faster than the main's Lap 1):
    //   Lap 2 avg 102, T1 42, S1 139; overlay Lap 1 avg 59.5, T1 29.5, S1 79.5 (all m/s).
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg}, {std::string(kLaps)});
    spec.scope = ReportScope::SelectedRanges;
    spec.deltaVsOverlay0 = true;
    const auto inputs = f.inputs({sourceRange(f.main, kLaps, "Lap 2"),
                                  sourceRange(f.overlay, kLaps, "Lap 1", ReportSlotRole::Overlay0)});
    const auto mainRow = [](const ReportTable& t) {
        for (std::size_t r = 0; r < t.rows.size(); ++r) {
            if (t.rows[r].role == ReportSlotRole::Main) {
                return r;
            }
        }
        FAIL("no Main row");
        return std::size_t{0};
    };
    // One level, the lap itself: the lap names differ, the rows pair.
    const ReportTable laps = run(spec, inputs, f.cache);
    REQUIRE(laps.rows.size() == 2);
    CHECK(laps.at(mainRow(laps), 1).state == ReportCellState::Ok);
    CHECK(laps.at(mainRow(laps), 1).value == Catch::Approx((102.0 - 59.5) * kKmh));
    // The demo's headings: a detail (different drivers), then the lap.
    spec.headers = {"Driver", std::string(kLaps)};
    const ReportTable byDriver = run(spec, inputs, f.cache);
    REQUIRE(byDriver.rows.size() == 2);
    CHECK(byDriver.at(mainRow(byDriver), 1).value == Catch::Approx((102.0 - 59.5) * kKmh));
    // Sections below the lap pair by section name.
    spec.headers = {std::string(kLaps), std::string(kSections)};
    const ReportTable sections = run(spec, inputs, f.cache);
    REQUIRE(sections.rows.size() == 4);
    for (std::size_t r = 0; r < sections.rows.size(); ++r) {
        if (sections.rows[r].role != ReportSlotRole::Main) {
            continue;
        }
        const double want = sections.rows[r].path.back() == "T1" ? 42.0 - 29.5 : 139.0 - 79.5;
        INFO(sections.rows[r].path.back());
        CHECK(sections.at(r, 1).value == Catch::Approx(want * kKmh));
    }
}

TEST_CASE("report_cells: untrusted laps are cut out of rows coarser than a lap (D-197 (b))", "[unit]") {
    // The whole outing as one row: without the out lap (50) and the in lap (30), the trusted samples are
    // Lap 1's 0..99 (sum 4950) and Lap 2's 97 valid samples (sum 9894).
    Fixture f;
    ReportSpec spec = speedSpec({ReportStat::Avg, ReportStat::Max, ReportStat::Start, ReportStat::End}, {});
    const auto outing = f.inputs({sourceRange(f.main, kOutings, "test.ld")});
    const ReportTable trusted = run(spec, outing, f.cache);
    REQUIRE(trusted.rows.size() == 1);
    CHECK(trusted.at(0, 0).value == Catch::Approx((4950.0 + 9894.0) / 197.0 * kKmh));
    CHECK(trusted.at(0, 1).value == Catch::Approx(198.0 * kKmh));
    CHECK(trusted.at(0, 2).value == Catch::Approx(0.0).margin(1e-12)); // the first trusted sample, 10 s
    CHECK(trusted.at(0, 3).value == Catch::Approx(198.0 * kKmh));      // the last trusted sample
    // A detail heading is as coarse: the same row under the driver's name.
    spec.headers = {"Driver"};
    const ReportTable labelled = run(spec, outing, f.cache);
    REQUIRE(paths(labelled) == std::vector<std::vector<std::string>>{{"Alice"}});
    CHECK(labelled.at(0, 0).value == Catch::Approx((4950.0 + 9894.0) / 197.0 * kKmh));
    // Included: every valid sample (397 of them).
    spec.options.includeUntrusted = true;
    const ReportTable all = run(spec, outing, f.cache);
    CHECK(all.at(0, 0).value == Catch::Approx((5000.0 + 4950.0 + 9894.0 + 3000.0) / 397.0 * kKmh));
    CHECK(all.at(0, 2).value == Catch::Approx(50.0 * kKmh));
    // Sections that lie wholly in an untrusted lap are no rows: the out lap's S1, the in lap's T1.
    spec.options.includeUntrusted = false;
    spec.headers = {std::string(kSections)};
    const ReportTable sections = run(spec, outing, f.cache);
    CHECK(sections.rows.size() == 4);
    for (std::size_t r = 0; r < sections.rows.size(); ++r) {
        CHECK(sections.at(r, 0).value != Catch::Approx(50.0 * kKmh));
        CHECK(sections.at(r, 0).value != Catch::Approx(30.0 * kKmh));
    }
}

TEST_CASE("report_cells: min, max, absMax and delta under an offset and a negative display scale", "[unit]") {
    // Lap 1 of "Temp" ramps from 253.15 to 283.15 (base K): -20 to +10 deg C.
    Fixture f;
    std::vector<double> temp(400, 273.15);
    for (std::size_t i = 100; i < 200; ++i) {
        temp[i] = 253.15 + 30.0 * static_cast<double>(i - 100) / 99.0;
    }
    f.channels.add(f.main, "Temp", f.tc.uniform(Rate::fromHz(10), temp), units::Converter{1.0, -273.15},
                   "degC");
    // A made-up unit with a negative scale: display = -base.
    f.channels.add(f.main, "Flip", f.tc.uniform(Rate::fromHz(10), temp), units::Converter{-1.0, 0.0}, "neg");
    ReportSpec spec;
    spec.headers = {};
    spec.channels = {
        ReportChannel{"Temp", {ReportStat::AbsMax}},
        ReportChannel{"Flip", {ReportStat::Min, ReportStat::Max, ReportStat::Delta, ReportStat::Change}}};
    const ReportTable t = run(spec, f.inputs({sourceRange(f.main, kLaps, "Lap 1")}), f.cache);
    REQUIRE(t.columns.size() == 5);                    // Temp's hidden extremes are not shown
    CHECK(t.at(0, 0).value == Catch::Approx(20.0));    // |-20 deg C|, not c(283.15) = 10
    CHECK(t.at(0, 1).value == Catch::Approx(-283.15)); // the display minimum is -max
    CHECK(t.at(0, 2).value == Catch::Approx(-253.15));
    CHECK(t.at(0, 3).value == Catch::Approx(30.0));  // a magnitude
    CHECK(t.at(0, 4).value == Catch::Approx(-30.0)); // a signed difference: -(end - start)
}

TEST_CASE("report_cells: sources with different display units convert into the column's unit", "[unit]") {
    static const units::UnitRegistry registry = [] {
        auto loaded = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        REQUIRE(loaded.ok());
        return std::move(loaded).value();
    }();
    const std::optional<units::UnitId> mps = registry.find("m/s");
    const std::optional<units::UnitId> mph = registry.find("mph");
    REQUIRE(mps);
    REQUIRE(mph);
    Fixture f;
    // The overlay shows Speed in mph; the column takes Main's km/h.
    ChannelTable channels;
    channels.addWithUnit(f.main, "Speed", speedSeries(f.tc), *mps, units::Converter{kKmh, 0.0}, "km/h", 1);
    channels.addWithUnit(f.overlay, "Speed", speedSeries(f.tc, 10.0), *mps, registry.fromBase(*mph), "mph",
                         1);
    ReportSpec spec = speedSpec({ReportStat::Avg});
    spec.scope = ReportScope::SelectedRanges;
    spec.deltaVsOverlay0 = true;
    ReportInputs inputs{{sourceRange(f.main, kLaps, "Lap 1"),
                         sourceRange(f.overlay, kLaps, "Lap 1", ReportSlotRole::Overlay0)},
                        channels.lookup(),
                        &registry};
    const ReportTable t = run(spec, inputs, f.cache);
    REQUIRE(t.rows.size() == 2);
    CHECK(t.columns[0].unit == "km/h");
    for (std::size_t r = 0; r < 2; ++r) {
        const bool isMain = t.rows[r].role == ReportSlotRole::Main;
        INFO((isMain ? "Main" : "Overlay 0"));
        CHECK(t.at(r, 0).value == Catch::Approx((isMain ? 49.5 : 59.5) * kKmh).epsilon(1e-9));
        if (isMain) {
            CHECK(t.at(r, 1).value == Catch::Approx(-10.0 * kKmh).epsilon(1e-9));
        }
    }
    // Without a registry the overlay's cells cannot convert: Error, never mph under a km/h heading.
    inputs.units = nullptr;
    StatCache fresh;
    const ReportTable unconverted = run(spec, inputs, fresh);
    for (std::size_t r = 0; r < 2; ++r) {
        if (unconverted.rows[r].role == ReportSlotRole::Overlay0) {
            CHECK(unconverted.at(r, 0).state == ReportCellState::Error);
        }
    }
}

TEST_CASE("report_cells: the detail-group headings read their detail, with the profile's missing text",
          "[unit]") {
    Fixture f;
    auto withSession = std::make_shared<model::DataSource>(*f.main);
    withSession->details.push_back(detailText("Session", "Race 2"));
    const std::shared_ptr<const model::DataSource> shared = withSession;
    f.channels.add(shared, "Speed", speedSeries(f.tc), units::Converter{kKmh, 0.0}, "km/h", 1);
    const ReportTable t = run(speedSpec({ReportStat::Avg}, {"Sessions", "Events", std::string(kLaps)}),
                              f.inputs({sourceRange(shared, kOutings, "test.ld")}), f.cache);
    REQUIRE(paths(t) == std::vector<std::vector<std::string>>{{"Race 2", "(no event)", "Lap 1"},
                                                              {"Race 2", "(no event)", "Lap 2"}});
}
