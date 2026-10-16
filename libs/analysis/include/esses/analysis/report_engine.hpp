// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportEngine (04 section 12; 01 section 4.4; ARCHITECTURE_OUTLINE section 4 esses_analysis; AC-M3-03;
// D-197): the cells of a channel_report. ReportEngine::cells(spec, inputs, cache) on a worker
// (Priority::Background).
//
// Rows. Each source range in scope (untrusted ones only with options.includeUntrusted, overlay slots only
// without options.excludeOverlays) is divided by the spec's headers in order: a header names a range group of
// the source's layer, and every range of it (enabled; trusted unless includeUntrusted) that overlaps the row
// so far splits the row into the intersections, so headers may come in any order ("Corners" before "Laps"). A
// header that names no group of the layer does not divide and labels the row with the source detail of that
// name ("Venue", "Driver"; "" without one); without headers a row is the source range under its own name. The
// path holds one name per header. Rows are ordered by source (chronological by the Log Date / Log Time
// details) and time, then grouped so that equal names at each level stay together under their first
// occurrence (ranges of equal name across sources merge under one heading). zoomLinked clips every source
// range to its `clip`.
//
// Columns. One per (channel, stat) in spec order; with deltaVsOverlay0 a Main - Overlay 0 column follows each
// of them (paired by heading path; blank on rows other than Main and where no Overlay 0 row of that path
// exists: Overlay 0 only, never D-017's reference-overlay fallback, 04 section 12 item 5; excludeOverlays
// hides the Overlay 0 rows but still pairs them).
//
// Cells. statistic() over the row's [start, end); one scan of a (row, channel) serves all its statistics.
// Value statistics (avg, min, max, absMax, start, end) are converted with toDisplay, differences (change,
// delta, stdDev) with its scale only, so stdDev is computed in base units then converted (display-unit
// invariance). A Pending channel gives Pending cells, an Error channel Error cells, a channel without a
// series Missing cells, a range without a valid sample a Blank cell. isMin/isMax mark the cells equal to the
// column's minimum/maximum when the column has two Ok cells that differ. Cells already in the StatCache are
// reused (key: the channel id, the column's identity and version, the narrowest range of the row with its
// exact [start, end) as the clip, the statistic and, for start/end/change, the probe and boundary options),
// so the table fills incrementally (ReportModel re-runs on Data); the missing ones are scanned in order while
// the chunks of the next scans are prefetched on the columns' pool. layout ChannelsInColumns sets
// ReportTable::transposed; the table itself is always rows x (channel, stat) columns.
//
// statistic() over the valid samples with start <= t < end, in base units:
//   avg      the arithmetic mean of the samples
//   stdDev   the population standard deviation (divides by n)
//   min/max  the smallest/largest sample; `at` is the time of its first occurrence
//   absMax   the largest magnitude |v| (>= 0); `at` as for min/max
//   delta    max - min
//   start    the range's first sample when it is valid, else the first valid sample at most probeSeconds
//            after `start`; with interpolateBoundaries the value at exactly `start` by `interp`
//            (store::Resampler's policy; neighbours outside the range count) when that is valid, else as
//            without the option
//   end      the range's last sample when it is valid, else the last valid sample at most probeSeconds before
//            `end`; with interpolateBoundaries the value at exactly `end` by `interp` when valid
//   change   end - start
// A range without a valid sample (or without a probe hit) gives StatValue::valid == false.
// AC-M3-03: cells equal hand-computed fixtures for every statistic incl. the probe and untrusted exclusion;
// 50 laps x 30 channels x 8 stats cold <= 300 ms (bench/report_bench).
#pragma once

#include <esses/analysis/report_spec.hpp>
#include <esses/analysis/stat_cache.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::analysis {

enum class ReportSlotRole : std::uint8_t { Main, Overlay0, Overlay };

// One range in scope (a selected range, or an outing of a selected/open source).
struct ReportSource {
    ReportSlotRole role = ReportSlotRole::Main;
    std::shared_ptr<const model::DataSource> source;
    std::string layer = "Normal";
    model::RangeRef range;
    std::optional<std::pair<Micros, Micros>> clip; // zoomLinked: the zoom span in this range's layer time
};

// A channel as resolved for one source (logged through ChannelResolver, maths outputs through MathsService;
// esses_qtmodel resolves, analysis never links maths).
struct ReportChannelData {
    std::optional<store::Series> series; // nullopt: missing, or pending when status == Pending
    std::optional<model::ChannelDescriptor> descriptor;
    model::ChannelStatus status = model::ChannelStatus::Ok;
    units::Converter toDisplay; // base -> display unit
    std::string displayUnit;
    int dps = 2;
};
using ReportChannelLookup =
    std::function<ReportChannelData(const ReportSource& source, std::string_view name)>;

struct ReportInputs {
    std::vector<ReportSource> sources;
    ReportChannelLookup channels;
    // Converts the cells of a source whose display unit differs from its column's into the column's unit
    // (D-197 (g)); without a registry, or when the units do not convert, such a source's cells are Error.
    const units::UnitRegistry* units = nullptr;
};

enum class ReportCellState : std::uint8_t { Ok, Blank, Pending, Missing, Error };

struct ReportCell {
    double value = 0.0; // display unit
    ReportCellState state = ReportCellState::Blank;
    bool isMin = false; // the column's minimum / maximum (highlight, 04 section 12 item 2)
    bool isMax = false;
    friend bool operator==(const ReportCell&, const ReportCell&) = default;
};

struct ReportHeading {
    std::vector<std::string> path; // one name per header level (range names; section names for sections)
    ReportSlotRole role = ReportSlotRole::Main;
    bool trusted = true;    // untrusted rows are shown italic when included
    std::size_t source = 0; // index into ReportInputs::sources
};

struct ReportColumn {
    std::size_t channel = 0; // index into ReportSpec::channels
    ReportStat stat = ReportStat::Avg;
    bool deltaVsOverlay0 = false;
    std::string unit; // display unit
    int dps = 2;
};

struct ReportTable {
    std::vector<ReportHeading> rows;
    std::vector<ReportColumn> columns;
    std::vector<ReportCell> cells; // rows.size() * columns.size(), row-major
    bool transposed = false;       // ReportLayout::ChannelsInColumns

    const ReportCell& at(std::size_t row, std::size_t column) const {
        return cells[row * columns.size() + column];
    }
    std::size_t pending() const noexcept; // cells still Pending
};

class ReportEngine {
public:
    // Cancelled; Invalid for a spec without channels.
    static Result<ReportTable> cells(const ReportSpec& spec, const ReportInputs& inputs, StatCache& cache,
                                     const CancelToken& cancel);
    // One statistic of a series over [start, end) with the probe and boundary options (base units; see the
    // table above). `interp` is the channel's interpolation for interpolateBoundaries. A range without a
    // valid sample gives StatValue::valid == false. Cancelled, and the Column::require errors.
    static Result<StatValue> statistic(const store::Series& series, ReportStat stat, Micros start, Micros end,
                                       const ReportOptions& options, const CancelToken& cancel,
                                       store::Interp interp = store::Interp::Linear);
};

} // namespace esses::analysis
