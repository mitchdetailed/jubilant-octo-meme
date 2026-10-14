// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReportEngine (04 section 13, AC-M3-03): section times, the eclectic row, the rolling minimum and the %
// bands against a hand-computed table. The fixture outing (seconds):
//   Out Lap [0, 53)    untrusted   S2 31,   S3 22             (S1 omitted: backwards origin)
//   Lap 1   [53, 136)              S1 30,   S2 32,   S3 21    lap 83
//   Lap 2   [136, 219)             S1 29.5, S2 33,   S3 20.5  lap 83
//   Lap 3   [219, 303)             S1 31,   S2 30.5, S3 22.5  lap 84
//   In Lap  [303, 340) untrusted   S1 28                      (the lap ends in the pits)
// Trusted laps only:
//   fastest     S1 29.5 (Lap 2), S2 30.5 (Lap 3), S3 20.5 (Lap 2), lap 83 (Laps 1 and 2)
//   eclectic    29.5, 30.5, 20.5, total 80.5
//   rolling     the windows of three over 30 32 21 | 29.5 33 20.5 | 31 30.5 22.5 sum to 83, 82.5, 83.5, 83,
//               84.5, 82, 84: the minimum 82 starts at index 5 (Lap 2 S3 20.5, Lap 3 S1 31, Lap 3 S2 30.5),
//               shown at the section positions as 31, 30.5, 20.5
//   total       90.5, 95.5, 64, 250
//   bands 1 %, 3 %: S1 limits 29.795 / 30.385, S2 30.805 / 31.415, S3 20.705 / 21.115, lap 83.83 / 85.49
#include "report_test_support.hpp"

#include <esses/analysis/time_report_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

using OptT = std::optional<Micros>;

std::vector<RangeSpec> timeLaps() {
    return {{"Out Lap", 0, 53, false},
            {"Lap 1", 53, 136, true},
            {"Lap 2", 136, 219, true},
            {"Lap 3", 219, 303, true},
            {"In Lap", 303, 340, false}};
}

std::vector<RangeSpec> timeSections() {
    return {{"S2", 0, 31, true},        {"S3", 31, 53, true},     {"S1", 53, 83, true},
            {"S2", 83, 115, true},      {"S3", 115, 136, true},   {"S1", 136, 165.5, true},
            {"S2", 165.5, 198.5, true}, {"S3", 198.5, 219, true}, {"S1", 219, 250, true},
            {"S2", 250, 280.5, true},   {"S3", 280.5, 303, true}, {"S1", 303, 331, true}};
}

std::shared_ptr<const model::DataSource> timeSource(std::string_view tag, std::string date = "10/10/2026") {
    return reportSource(tag, 340.0, timeLaps(), timeSections(),
                        {detailText("Log Date", std::move(date)), detailText("Log Time", "10:00:00")});
}

ReportInputs lapsOf(const std::shared_ptr<const model::DataSource>& src,
                    const std::vector<std::string>& names) {
    ReportInputs in;
    for (const std::string& n : names) {
        in.sources.push_back(sourceRange(src, kLaps, n));
    }
    return in;
}

TimeReportSpec bandedSpec() {
    TimeReportSpec spec;
    spec.bands = {TimeReportBand{1.0, 0x30C030FF, true}, TimeReportBand{3.0, 0xC0C030FF, true}};
    return spec;
}

TimeReportTable build(const TimeReportSpec& spec, const ReportInputs& inputs) {
    Result<TimeReportTable> t = TimeReportEngine::build(spec, inputs, {});
    REQUIRE(t.ok());
    return std::move(t).value();
}

std::vector<OptT> times(const TimeReportRow& row) {
    std::vector<OptT> out;
    out.reserve(row.cells.size());
    for (const TimeReportCell& c : row.cells) {
        out.push_back(c.time);
    }
    return out;
}

std::vector<int> bands(const TimeReportRow& row) {
    std::vector<int> out;
    out.reserve(row.cells.size());
    for (const TimeReportCell& c : row.cells) {
        out.push_back(c.band);
    }
    return out;
}

std::vector<OptT> secs(std::initializer_list<double> values) {
    std::vector<OptT> out;
    out.reserve(values.size());
    for (const double v : values) {
        out.push_back(v < 0 ? OptT{} : OptT{sec(v)});
    }
    return out;
}

} // namespace

TEST_CASE("report_time: section times, eclectic, rolling minimum, total and bands", "[unit]") {
    const auto src = timeSource("time");
    const TimeReportTable t =
        build(bandedSpec(), lapsOf(src, {"Out Lap", "Lap 1", "Lap 2", "Lap 3", "In Lap"}));
    CHECK(t.sections == std::vector<std::string>{"S1", "S2", "S3"});
    REQUIRE(t.rows.size() == 3); // the untrusted out and in laps are not rows
    CHECK(t.rows[0].name == "Lap 1");
    CHECK(t.rows[2].name == "Lap 3");
    CHECK(t.rows[1].source == 2);
    CHECK(times(t.rows[0]) == secs({30, 32, 21, 83}));
    CHECK(times(t.rows[1]) == secs({29.5, 33, 20.5, 83}));
    CHECK(times(t.rows[2]) == secs({31, 30.5, 22.5, 84}));
    CHECK(bands(t.rows[0]) == std::vector<int>{1, -1, 1, 0});
    CHECK(bands(t.rows[1]) == std::vector<int>{0, -1, 0, 0});
    CHECK(bands(t.rows[2]) == std::vector<int>{-1, 0, -1, 1});
    CHECK(t.rows[1].cells[0].fastest);
    CHECK(t.rows[2].cells[1].fastest);
    CHECK(t.rows[0].cells[3].fastest); // both 83 s laps are the fastest
    CHECK(t.rows[1].cells[3].fastest);
    CHECK_FALSE(t.rows[0].cells[0].fastest);
    REQUIRE(t.eclectic.has_value());
    REQUIRE(t.rollingMin.has_value());
    REQUIRE(t.total.has_value());
    if (t.eclectic && t.rollingMin && t.total) {
        CHECK(t.eclectic->name == "Eclectic");
        CHECK(times(*t.eclectic) == secs({29.5, 30.5, 20.5, 80.5}));
        CHECK(times(*t.rollingMin) == secs({31, 30.5, 20.5, 82}));
        CHECK(times(*t.total) == secs({90.5, 95.5, 64, 250}));
    }
}

TEST_CASE("report_time: included untrusted laps are rows and feed the summaries", "[unit]") {
    const auto src = timeSource("time");
    TimeReportSpec spec = bandedSpec();
    spec.includeUntrusted = true;
    const TimeReportTable t = build(spec, lapsOf(src, {"Out Lap", "Lap 1", "Lap 2", "Lap 3", "In Lap"}));
    CHECK(t.sections == std::vector<std::string>{"S1", "S2", "S3"});
    REQUIRE(t.rows.size() == 5);
    CHECK_FALSE(t.rows[0].trusted);
    CHECK(times(t.rows[0]) == secs({-1, 31, 22, 53}));
    CHECK(times(t.rows[4]) == secs({28, -1, -1, 37}));
    CHECK(t.rows[0].cells[0].band == -1);
    const std::optional<TimeReportRow>& eclectic = t.eclectic;
    const std::optional<TimeReportRow>& rolling = t.rollingMin;
    REQUIRE(eclectic.has_value());
    REQUIRE(rolling.has_value());
    if (eclectic && rolling) {
        // S1's minimum is now the in lap's 28 s: 28 + 30.5 + 20.5 = 79.
        CHECK(times(*eclectic) == secs({28, 30.5, 20.5, 79}));
        // Lap 3 S2 30.5 + Lap 3 S3 22.5 + In Lap S1 28 = 81 (index 10), at positions S1, S2, S3.
        CHECK(times(*rolling) == secs({28, 30.5, 22.5, 81}));
    }
}

TEST_CASE("report_time: the rolling minimum never joins laps that were not consecutive", "[unit]") {
    const auto src = timeSource("time");
    // Laps 1 and 3 only: across the gap 21 + 31 + 30.5 = 82.5 would win; within the laps Lap 1's 83 does.
    const TimeReportTable t = build(TimeReportSpec{}, lapsOf(src, {"Lap 1", "Lap 3"}));
    REQUIRE(t.rollingMin.has_value());
    if (t.rollingMin) {
        CHECK(times(*t.rollingMin) == secs({30, 32, 21, 83}));
    }
}

TEST_CASE("report_time: rows are chronological across sources; summaries can be off", "[unit]") {
    const auto later = timeSource("later", "11/10/2026");
    const auto earlier = timeSource("earlier", "2026-10-09");
    ReportInputs in;
    in.sources = {sourceRange(later, kLaps, "Lap 1"), sourceRange(earlier, kLaps, "Lap 2"),
                  sourceRange(earlier, kLaps, "Lap 1")};
    TimeReportSpec spec;
    spec.summaries = TimeReportSummaries{false, false, false};
    const TimeReportTable t = build(spec, in);
    REQUIRE(t.rows.size() == 3);
    CHECK(t.rows[0].source == 2);
    CHECK(t.rows[1].source == 1);
    CHECK(t.rows[2].source == 0);
    CHECK_FALSE(t.eclectic.has_value());
    CHECK_FALSE(t.rollingMin.has_value());
    CHECK_FALSE(t.total.has_value());
    // Another row group: the laps have no such sections, so only the lap-time column.
    spec.rowGroup = "Outings:Laps:Track Sections:Fast";
    const TimeReportTable none = build(spec, in);
    CHECK(none.sections.empty());
    CHECK(times(none.rows[0]) == secs({83}));
    // Cancelled.
    CancelSource cancel;
    cancel.cancel();
    const Result<TimeReportTable> c = TimeReportEngine::build(spec, in, cancel.token());
    REQUIRE_FALSE(c.ok());
    CHECK(c.error().code == ErrorCode::Cancelled);
}

TEST_CASE("report_time: eclectic, rollingMinimum and bandOf by hand", "[unit]") {
    // Column minima 5, 7, 8 -> total 20; a column without a time has no minimum and no total.
    CHECK(TimeReportEngine::eclectic({{5, 7, 9}, {6, std::nullopt, 8}}) == std::vector<OptT>{5, 7, 8, 20});
    CHECK(TimeReportEngine::eclectic({{5, std::nullopt}, {6, std::nullopt}}) ==
          std::vector<OptT>{5, std::nullopt, std::nullopt});
    CHECK(TimeReportEngine::eclectic({}) == std::vector<OptT>{std::nullopt});

    // 1 2 3 1 1 5 in windows of three: 6, 6, 5, 7 -> 5 from index 2 (3 at position 2, 1 at 0, 1 at 1).
    const std::vector<OptT> seq{1, 2, 3, 1, 1, 5};
    const RollingMinimum r = TimeReportEngine::rollingMinimum(seq, 3);
    CHECK(r.sum == OptT{5});
    CHECK(r.firstIndex == 2);
    CHECK(r.bySection == std::vector<OptT>{1, 1, 3});
    // Ties go to the earliest window; a missing time breaks every window over it.
    CHECK(TimeReportEngine::rollingMinimum(std::vector<OptT>{2, 2, 2, 2}, 2).firstIndex == 0);
    const RollingMinimum gap =
        TimeReportEngine::rollingMinimum(std::vector<OptT>{1, std::nullopt, 1, 1, 1}, 2);
    CHECK(gap.sum == OptT{2});
    CHECK(gap.firstIndex == 2);
    CHECK(gap.bySection == std::vector<OptT>{1, 1});
    CHECK_FALSE(TimeReportEngine::rollingMinimum(std::vector<OptT>{1, 2}, 3).sum.has_value());
    CHECK_FALSE(TimeReportEngine::rollingMinimum(std::vector<OptT>{1, 2}, 0).sum.has_value());

    // fastest 1,000,000 us: 1 % allows up to 1,010,000 us, 3 % up to 1,030,000 us.
    const std::vector<TimeReportBand> b{TimeReportBand{1.0, 0, true}, TimeReportBand{3.0, 0, true}};
    CHECK(TimeReportEngine::bandOf(1'000'000, 1'000'000, b) == 0);
    CHECK(TimeReportEngine::bandOf(1'010'000, 1'000'000, b) == 0);
    CHECK(TimeReportEngine::bandOf(1'010'001, 1'000'000, b) == 1);
    CHECK(TimeReportEngine::bandOf(1'030'001, 1'000'000, b) == -1);
    // List order wins, and an invisible band is skipped (ALT+n).
    const std::vector<TimeReportBand> reversed{TimeReportBand{3.0, 0, true}, TimeReportBand{1.0, 0, true}};
    CHECK(TimeReportEngine::bandOf(1'000'000, 1'000'000, reversed) == 0);
    const std::vector<TimeReportBand> hidden{TimeReportBand{1.0, 0, false}, TimeReportBand{3.0, 0, true}};
    CHECK(TimeReportEngine::bandOf(1'000'000, 1'000'000, hidden) == 1);
    CHECK(TimeReportEngine::bandOf(1'000'000, 1'000'000, {}) == -1);
}
