// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReportEngine (04 section 13; ARCHITECTURE_OUTLINE section 4 esses_analysis; D-027; AC-M3-03): the
// section-time table of a time_report.
//   rows       the lap ranges of ReportInputs::sources (the caller expands selected outings into their laps),
//              chronological (sources by the Log Date / Log Time details, then time); untrusted laps only
//              with includeUntrusted (then marked, and they feed the summaries like any row)
//   columns    the sections of rowGroup (default Outings:Laps:Track Sections:Default) that start inside a
//              lap, by name, in track order (the laps' section sequences merged); cell = section end - start;
//              the last column is the lap time; a missing section is empty ("-")
//   summaries  eclectic = the minimum per section and their total (the theoretical best; no total while a
//              section has no time); rollingMin = over the chronological concatenation of the section times
//              of each run of contiguous laps (a lap that ends where the next one starts; a window never
//              joins laps that were not driven one after the other), the window of sectionsPerLap consecutive
//              sections with the minimum sum (the earliest on a tie), shown at the section positions with
//              the sum in the lap-time column (an interpretation of the help, 04 section 20 item 9); total =
//              the sum per column of the times present
//   bands      a cell is tinted with band n when cell <= fastest(column) * (1 + pct_n / 100); bands apply in
//              list order (the first match wins); invisible bands are skipped (ALT+n); `fastest` marks the
//              column's minimum
// Out laps and partial laps get their sections from TrackSectionsRule's backwards origin, so they never feed
// bogus section times into the eclectic.
#pragma once

#include <esses/analysis/report_engine.hpp>
#include <esses/analysis/report_spec.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::analysis {

struct TimeReportBand {
    double pct = 1.0;
    std::uint32_t colour = 0x30C030FF; // 0xRRGGBBAA
    bool visible = true;
    friend bool operator==(const TimeReportBand&, const TimeReportBand&) = default;
};

struct TimeReportSummaries {
    bool eclectic = true;
    bool rollingMin = true;
    bool total = true;
    friend bool operator==(const TimeReportSummaries&, const TimeReportSummaries&) = default;
};

struct TimeReportSpec { // the time_report body minus timeFormat (04 section 13 item 7)
    ReportScope scope = ReportScope::SelectedOutings;
    std::string rowGroup = "Outings:Laps:Track Sections:Default";
    bool includeUntrusted = false; // options.includeUntrusted
    TimeReportSummaries summaries;
    std::vector<TimeReportBand> bands;

    // The time_report body. Invalid with context.detail on a wrong type; absent members keep the defaults.
    static Result<TimeReportSpec> fromJson(const JsonDoc& body);
    friend bool operator==(const TimeReportSpec&, const TimeReportSpec&) = default;
};

struct TimeReportCell {
    std::optional<Micros> time; // nullopt: missing section ("-")
    int band = -1;              // index into the spec's bands, -1 = none
    bool fastest = false;       // the fastest of its column
};

struct TimeReportRow {
    std::string name; // the lap's name ("Lap 3"); "Eclectic", "Rolling minimum", "Total" for summaries
    bool trusted = true;
    std::size_t source = 0;            // index into ReportInputs::sources (summary rows: 0)
    std::vector<TimeReportCell> cells; // one per section, then the lap time
};

struct TimeReportTable {
    std::vector<std::string> sections; // column titles in track order (the lap time column is extra)
    std::vector<TimeReportRow> rows;
    std::optional<TimeReportRow> eclectic;
    std::optional<TimeReportRow> rollingMin;
    std::optional<TimeReportRow> total;
};

struct RollingMinimum {
    std::optional<Micros> sum;  // nullopt: fewer than sectionsPerLap section times in scope
    std::size_t firstIndex = 0; // index of the window's first section time in the concatenation
    std::vector<std::optional<Micros>> bySection; // the window's times at their section positions
};

class TimeReportEngine {
public:
    // The rows are the laps in `inputs.sources` (each a lap range; the engine finds the rowGroup sections
    // under it in the source's range tree). Cancelled.
    static Result<TimeReportTable> build(const TimeReportSpec& spec, const ReportInputs& inputs,
                                         const CancelToken& cancel);
    // Minimum per column over the rows (missing cells ignored; columns = the longest row), then one more
    // element: the total of those minima, nullopt when a column has no time or there is no column.
    static std::vector<std::optional<Micros>>
    eclectic(const std::vector<std::vector<std::optional<Micros>>>& rows);
    // `chronological`: every lap's section times in order, laps concatenated (missing = nullopt, which breaks
    // a window); index j belongs to section j % sectionsPerLap. The earliest window wins a tie.
    static RollingMinimum rollingMinimum(std::span<const std::optional<Micros>> chronological,
                                         std::size_t sectionsPerLap);
    static int bandOf(Micros cell, Micros fastest, std::span<const TimeReportBand> bands) noexcept;
};

} // namespace esses::analysis
