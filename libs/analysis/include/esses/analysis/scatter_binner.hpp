// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterBinner (04 section 10.1 items 1, 2, 5, 9; ARCHITECTURE_OUTLINE section 4 esses_analysis): the point
// sets of a scatter plot. gather() evaluates X, every Y, the optional colour channel Z and the optional
// gating channel over the spans on a union grid at the highest rate among them (the sample times of the
// first channel with that rate, X first), each channel read with its own Interp under store::Resampler's
// rules (invalid outside the channel's first..last sample); Y trace k is read at t + yDelayUs[k] (positive
// = Y later). A point is dropped when X or that Y is invalid, or when gating is <= 0 or invalid; an invalid
// Z keeps the point with z = NaN. Every span is read (a scatter has no untrusted exclusion; the caller passes
// the spans to show). intervalAverage() is the mean Y per X bin (bin k holds k * binWidth <= x < (k + 1) *
// binWidth; the geometry draws a stepped polyline), density() the 2D histogram used above the density
// threshold (default 256 x 256 bins). Worker only (Priority::Visible); pins at most two chunks per column at
// a time.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace esses::analysis {

struct ScatterChannel {
    store::Series series;
    units::Converter toDisplay;                   // base -> the axis' display unit
    store::Interp interp = store::Interp::Linear; // the channel's own policy
};

struct ScatterInput {
    ScatterChannel x;
    std::vector<ScatterChannel> y;
    std::vector<Micros> yDelayUs;         // one per y (missing = 0)
    std::optional<ScatterChannel> colour; // Z
    std::optional<store::Series> gating;
    std::vector<SampleSpan> spans;
    store::Interp gatingInterp = store::Interp::Previous; // as the histogram's gating (04 section 11.1)
};

struct ScatterSeries { // one Y trace of one slot, display units
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z; // empty without a colour channel
    std::vector<Micros> t; // the X sample time of each point (click -> cursor)
};

struct IntervalAverage {
    std::vector<double> binLo; // left edge of each non-empty bin
    std::vector<double> mean;  // mean Y in that bin
};

struct DensityGrid {
    int nx = 0;
    int ny = 0;
    double x0 = 0.0;
    double x1 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    std::vector<std::uint32_t> counts; // ny rows of nx, row 0 at y0
};

class ScatterBinner {
public:
    static constexpr std::uint64_t kDensityThreshold = 1000000; // visible points (04 section 10.1 item 9)

    // One ScatterSeries per y channel (none for no y). Invalid when x is not a uniform series with a rate;
    // Cancelled; the Column::require errors.
    static Result<std::vector<ScatterSeries>> gather(const ScatterInput& input, Priority prio,
                                                     const CancelToken& cancel);
    // Non-empty bins in increasing order; pairs with a non-finite value are skipped. Invalid for binWidth <=
    // 0 (or not finite) or mismatched spans.
    static Result<IntervalAverage> intervalAverage(std::span<const double> x, std::span<const double> y,
                                                   double binWidth);
    // Counts of the points in nx x ny equal cells over [x0, x1] x [y0, y1] (the upper edges belong to the
    // last column and row; points outside are not counted). An empty grid (nx == ny == 0) for nx or ny < 1 or
    // an empty or non-finite extent.
    static DensityGrid density(const ScatterSeries& series, int nx, int ny, double x0, double x1, double y0,
                               double y1);
};

} // namespace esses::analysis
