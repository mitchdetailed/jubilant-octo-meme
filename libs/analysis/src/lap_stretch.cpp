// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretch (D-016, D-140): the factor rule, its application to a lap's DistanceIndex (the stretch is part
// of the index key, set before the index is published) and the per-lap report.

#include <esses/analysis/lap_stretch.hpp>

#include <cmath>

namespace esses::analysis {

namespace {

bool within(double lengthM, double trackLengthM, double tolerance) noexcept {
    return lengthM > 0.0 && trackLengthM > 0.0 && tolerance >= 0.0 && std::isfinite(lengthM) &&
           std::isfinite(trackLengthM) && std::abs(lengthM - trackLengthM) / trackLengthM <= tolerance;
}

} // namespace

double LapStretch::factor(double lengthM, double trackLengthM, double tolerance) noexcept {
    if (!within(lengthM, trackLengthM, tolerance)) {
        return 1.0;
    }
    return trackLengthM / lengthM;
}

LapStretchRow LapStretch::row(const DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                              const LapStretchSettings& settings) {
    LapStretchRow out;
    out.lap = lap;
    out.lengthM = index.lengthM();
    out.trackLengthM = trackLengthM > 0.0 && std::isfinite(trackLengthM) ? trackLengthM : 0.0;
    out.withinTolerance = within(out.lengthM, out.trackLengthM, settings.tolerance);
    out.candidate = out.withinTolerance ? out.trackLengthM / out.lengthM : 1.0;
    out.applied = settings.enabled && out.withinTolerance;
    out.stretch = out.applied ? out.candidate : 1.0;
    return out;
}

LapStretchRow LapStretch::apply(DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                                const LapStretchSettings& settings) {
    LapStretchRow out = row(index, lap, trackLengthM, settings);
    index.setStretch(out.stretch);
    return out;
}

LapStretchRow LapStretch::apply(DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                                double tolerance) {
    return apply(index, lap, trackLengthM, LapStretchSettings{true, tolerance});
}

std::vector<LapStretchRow>
LapStretch::report(std::span<const std::pair<model::RangeId, const DistanceIndex*>> laps, double trackLengthM,
                   const LapStretchSettings& settings) {
    std::vector<LapStretchRow> rows;
    rows.reserve(laps.size());
    const DistanceIndex none;
    for (const auto& [lap, index] : laps) {
        rows.push_back(row(index != nullptr ? *index : none, lap, trackLengthM, settings));
    }
    return rows;
}

} // namespace esses::analysis
