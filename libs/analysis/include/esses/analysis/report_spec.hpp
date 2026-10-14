// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportSpec (04 section 12 item 2; ARCHITECTURE_OUTLINE section 4 esses_analysis; D-027): what a
// channel_report computes. The JSON form is the body's "report" member (04 section 12 item 8:
// {scope, headers[], channels[{name, stats[]}], options{...}}); `layout` and `deltaVsOverlay0` are body
// members the engine needs too, set by the caller from the body. Built by value from the ComponentSpec and
// the current selection and handed to workers (01 section 5.3 rule 2).
//   scope    selectedRanges | selectedOutings | allOutings
//   headers  ordered range-group paths that divide the data, outermost first ("Outings",
//            "Outings:Laps:Track Sections:Default", "Outings:Laps"), or detail groups ("Events", "Driver",
//            ...)
//   stats    avg, min, max, absMax, start, end, change, stdDev, delta (delta = max - min)
//   options  excludeOverlays, includeUntrusted, interpolateBoundaries, probeSeconds (5), zoomLinked
// fromJson reads the members it knows and ignores others (the body schema is closed); toJson writes every
// member, so toJson -> fromJson is exact.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

enum class ReportScope : std::uint8_t { SelectedRanges, SelectedOutings, AllOutings };
enum class ReportStat : std::uint8_t { Avg, Min, Max, AbsMax, Start, End, Change, StdDev, Delta };
enum class ReportLayout : std::uint8_t { ChannelsInRows, ChannelsInColumns };

// "avg", "min", "max", "absMax", "start", "end", "change", "stdDev", "delta".
std::string_view reportStatName(ReportStat stat) noexcept;
std::optional<ReportStat> parseReportStat(std::string_view name) noexcept;
// "selectedRanges", "selectedOutings", "allOutings".
std::string_view reportScopeName(ReportScope scope) noexcept;
std::optional<ReportScope> parseReportScope(std::string_view name) noexcept;

struct ReportChannel {
    std::string name;
    std::vector<ReportStat> stats;
    friend bool operator==(const ReportChannel&, const ReportChannel&) = default;
};

struct ReportOptions {
    bool excludeOverlays = false;
    bool includeUntrusted = false;
    bool interpolateBoundaries = false;
    double probeSeconds = 5.0; // start/end probe inward for a valid sample
    bool zoomLinked = false;   // clip every range to the worksheet zoom
    friend bool operator==(const ReportOptions&, const ReportOptions&) = default;
};

struct ReportSpec {
    ReportScope scope = ReportScope::SelectedRanges;
    std::vector<std::string> headers{"Outings:Laps"};
    std::vector<ReportChannel> channels;
    ReportOptions options;
    ReportLayout layout = ReportLayout::ChannelsInRows; // body "layout"
    bool deltaVsOverlay0 = false;                       // body "deltaVsOverlay0"

    // The body's "report" object. Invalid with context.detail (a JSON pointer) on a wrong type or an unknown
    // statistic or scope; absent members keep the defaults above.
    static Result<ReportSpec> fromJson(const JsonDoc& report);
    // The "report" object (canonical; layout and deltaVsOverlay0 are not part of it).
    JsonDoc toJson() const;

    friend bool operator==(const ReportSpec&, const ReportSpec&) = default;
};

} // namespace esses::analysis
