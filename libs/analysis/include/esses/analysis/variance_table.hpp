// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// VarianceTable (D-023; 04 section 8.8; ARCHITECTURE_OUTLINE section 4 esses_analysis; AC-M3-05): the
// variance panel of time_distance, sampled per device-pixel column from two laps' DistanceIndex tables,
// O(columns), no channel resampling. For an overlay k against Main:
//   distance domain  dt(d) = tAtD_k(d) - tAtD_M(d)   seconds (TimeAtDistance)
//   time domain      dd(t) = dAtT_k(t) - dAtT_M(t)   metres  (DistanceAtTime)
// x is measured from the Main lap start (metres, or seconds); the alignment offsets of both slots are applied
// before the lookups (slot s reads its lap at x - offset_s, as DomainMapper::xOf adds the offset), and each
// lap's stretch (DistanceIndex::stretch(), D-016) scales its distances as DistanceChain does: tAtD_s(d) =
// timeAt(d / stretch_s), dAtT_s(t) = distAt(t) * stretch_s. Variance is lap against lap: a column is NaN
// (left blank, 04 section 8.8 item 2) where x - mainOffset lies outside the Main lap [0, extent) or
// x - overlayOffset outside the overlay's lap [0, extent] (extent: the stretched length lengthM() *
// stretch(), or the lap duration in seconds). F3 with no overlay builds no table (AC-M3-05: 0 ms; the
// time_distance builder calls build() once per visible overlay and never without one). The single-point
// lookups serve the variance_time()/variance_dist() maths functions (D-023, through EvalContext) and tests;
// they equal a one-column build at x with zero offsets.
// AC-M3-05: equals a brute-force reference within +-1 ms at every column for 3 overlay pairs
// (tests/variance_table_tests.cpp).
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace esses::analysis {

enum class VarianceMode : std::uint8_t {
    TimeAtDistance, // distance domain: dt(d) in seconds
    DistanceAtTime  // time domain: dd(t) in metres
};

struct VarianceRequest {
    VarianceMode mode = VarianceMode::TimeAtDistance;
    double x0 = 0.0; // the sampled span [x0, x1] from the Main lap start (metres or seconds)
    double x1 = 0.0; //
    int columns = 0; // device-pixel columns (>= 1); column k samples x0 + (x1 - x0) * (k + 0.5) / columns
    double mainOffset = 0.0;    // alignment offset of Main (offsetM or offsetUs / 1e6), applied before lookup
    double overlayOffset = 0.0; // alignment offset of the overlay
};

class VarianceTable {
public:
    VarianceTable() = default; // empty: size() == 0

    // Invalid for columns < 1, x1 < x0 or a non-finite span or offset; an empty index on either side (no
    // distance covered, or an empty lap) gives Invalid with context.detail "variance needs distance data"
    // (the panel's banner, 04 section 8.8 item 3).
    static Result<VarianceTable> build(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex,
                                       const VarianceRequest& request);

    VarianceMode mode() const noexcept { return mode_; }
    std::size_t size() const noexcept { return values_.size(); }
    std::span<const double> values() const noexcept { return values_; } // NaN = blank column
    double xAt(std::size_t column) const noexcept;                      // the sampled x of a column
    // [min, max] over the finite values (autoscale); nullopt when there is none.
    std::optional<std::pair<double, double>> valueRange() const noexcept;

private:
    VarianceMode mode_ = VarianceMode::TimeAtDistance;
    double x0_ = 0.0;
    double x1_ = 0.0;
    std::vector<double> values_;
};

// Single-point lookups: seconds at a (stretched) Main lap distance, metres at seconds since the Main lap
// start; NaN outside either lap or when either index is empty.
double varianceTimeAt(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex, double distanceM);
double varianceDistanceAt(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex, double timeS);

} // namespace esses::analysis
