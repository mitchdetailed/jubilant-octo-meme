// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// VarianceTable (D-023, 04 section 8.8, AC-M3-05): one lookup per column in each lap's DistanceIndex.

#include <esses/analysis/variance_table.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace esses::analysis {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double stretchOf(const DistanceIndex& idx) noexcept {
    const double s = idx.stretch();
    return std::isfinite(s) && s > 0.0 ? s : 1.0;
}

// The lap's extent in the domain: stretched metres, or seconds.
double extentOf(const DistanceIndex& idx, VarianceMode mode) noexcept {
    return mode == VarianceMode::TimeAtDistance ? idx.lengthM() * stretchOf(idx)
                                                : static_cast<double>(idx.lapEnd() - idx.lapStart()) / 1e6;
}

bool hasDistance(const DistanceIndex& idx) noexcept {
    return idx.lengthM() > 0.0 && idx.lapEnd() > idx.lapStart();
}

// The lap's value at a lap-relative x: the first time (s) a stretched distance is reached, or the stretched
// distance (m) at a time.
double lookup(const DistanceIndex& idx, VarianceMode mode, double x) noexcept {
    if (mode == VarianceMode::TimeAtDistance) {
        return idx.timeAt(x / stretchOf(idx));
    }
    return idx.distAt(idx.lapStart() + static_cast<Micros>(std::llround(x * 1e6))) * stretchOf(idx);
}

// overlay - main at x (from the Main lap start, offsets applied): NaN where x lies outside the Main lap
// [0, extent) or the overlay's lap [0, extent].
double varianceAt(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex, VarianceMode mode,
                  double x, double mainOffset, double overlayOffset) noexcept {
    const double xm = x - mainOffset;
    const double xo = x - overlayOffset;
    if (!(xm >= 0.0 && xm < extentOf(mainIndex, mode)) ||
        !(xo >= 0.0 && xo <= extentOf(overlayIndex, mode))) {
        return kNaN;
    }
    return lookup(overlayIndex, mode, xo) - lookup(mainIndex, mode, xm);
}

} // namespace

Result<VarianceTable> VarianceTable::build(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex,
                                           const VarianceRequest& request) {
    if (request.columns < 1 || !std::isfinite(request.x0) || !std::isfinite(request.x1) ||
        request.x1 < request.x0 || !std::isfinite(request.mainOffset) ||
        !std::isfinite(request.overlayOffset)) {
        return Failure{
            Error{ErrorCode::Invalid, "variance needs at least one column over a finite span", {}}};
    }
    if (!hasDistance(mainIndex) || !hasDistance(overlayIndex)) {
        Error e{ErrorCode::Invalid, "variance needs distance data on both laps", {}};
        e.context.detail = "variance needs distance data";
        return Failure{std::move(e)};
    }
    VarianceTable table;
    table.mode_ = request.mode;
    table.x0_ = request.x0;
    table.x1_ = request.x1;
    table.values_.resize(static_cast<std::size_t>(request.columns));
    for (std::size_t k = 0; k < table.values_.size(); ++k) {
        table.values_[k] = varianceAt(mainIndex, overlayIndex, request.mode, table.xAt(k), request.mainOffset,
                                      request.overlayOffset);
    }
    return table;
}

double VarianceTable::xAt(std::size_t column) const noexcept {
    if (values_.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return x0_ + (x1_ - x0_) * (static_cast<double>(column) + 0.5) / static_cast<double>(values_.size());
}

std::optional<std::pair<double, double>> VarianceTable::valueRange() const noexcept {
    std::optional<std::pair<double, double>> range;
    for (const double v : values_) {
        if (!std::isfinite(v)) {
            continue;
        }
        if (!range) {
            range.emplace(v, v);
        } else {
            range->first = std::min(range->first, v);
            range->second = std::max(range->second, v);
        }
    }
    return range;
}

double varianceTimeAt(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex, double distanceM) {
    if (!hasDistance(mainIndex) || !hasDistance(overlayIndex) || !std::isfinite(distanceM)) {
        return kNaN;
    }
    return varianceAt(mainIndex, overlayIndex, VarianceMode::TimeAtDistance, distanceM, 0.0, 0.0);
}

double varianceDistanceAt(const DistanceIndex& mainIndex, const DistanceIndex& overlayIndex, double timeS) {
    if (!hasDistance(mainIndex) || !hasDistance(overlayIndex) || !std::isfinite(timeS)) {
        return kNaN;
    }
    return varianceAt(mainIndex, overlayIndex, VarianceMode::DistanceAtTime, timeS, 0.0, 0.0);
}

} // namespace esses::analysis
