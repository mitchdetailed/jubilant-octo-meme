// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackGenerator (ARCHITECTURE_OUTLINE section 4 esses_analysis; 06 section 13.5; D-027; AC-M3-04): builds a
// TrackMap from one lap.
//   fromSpeedLatG  dead reckoning: heading rate = latAccel / speed, integrated over the lap and closed into a
//                  loop (the closure error before the correction is reported, never gated)
//   fromGps        latitude/longitude projected to ENU metres about the lap's first valid fix (or `origin`),
//                  resampled along the path, smoothed, closed; optional inner/outer laps give the inner and
//                  outer lines. The inputs are the esses.gps_corrections chain head as MathsService resolves
//                  it (03 section 9.2): (0, 0) fixes are already invalid
// Inputs are whole-lap signals in base units (CorrectedSpeed's UniformSignal: m/s, m/s^2, degrees) read by
// the caller on a worker. AC-M3-04: (a) GPS with sigma 1.5 m noise and a 2 s dropout lies within 2 m RMS of
// the true centreline; (b) Speed+LatG after a best-fit rigid alignment lies within 15 m RMS of the GPS map
// and every default-section apex within 20 m of lap distance (thresholds provisional until the first real
// fixture). The methods (track_generator.cpp): GPS fixes are projected, a gap is bridged by a Hermite curve
// ("W-TRACK-DROPOUT" for gaps from 2.5 sample periods and 0.5 s), the loop is closed by joining its end to
// its start, and an adaptive Gaussian kernel along the path smooths it (sigma smoothingM / 2 in the corners,
// up to eight times that on the straights); Speed+LatG removes the heading excess over a whole number of
// revolutions (zero for a figure eight) as a constant heading-rate bias and spreads the remaining gap along
// the distance; "W-TRACK-NOT-A-LOOP" when the corrected ends are still more than a quarter of the length
// apart (D-195 (b)). Lateral acceleration is
// positive to the left (ISO 8855); invertLatAccel reads loggers that log it positive to the right. A map from
// Speed+LatG has an arbitrary rotation (the lap starts heading east) and the origin of options.origin (else
// 0, 0). Owner: src/track/ (the track work).
#pragma once

#include <esses/analysis/corrected_speed.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>

#include <optional>
#include <string>
#include <vector>

namespace esses::analysis {

struct LapWindow { // [start, end) in layer time
    Micros start = 0;
    Micros end = 0;
};

struct GpsLap {
    LapWindow lap;
    UniformSignal latDeg;
    UniformSignal lonDeg;
};

struct TrackGeneratorOptions {
    double stepM = 1.0;       // centreline resampling step
    double smoothingM = 10.0; // path smoothing window (06 section 13.5 item 1 "smoothing option"); 0 = off
    bool closeLoop = true;    // distribute the closure error along the lap
    std::optional<TrackOrigin> origin; // GPS: ENU origin; default the lap's first valid fix
    std::optional<GpsLap> innerLap;    // GPS only
    std::optional<GpsLap> outerLap;    // GPS only
    bool invertLatAccel = false;       // Speed+LatG: the channel is positive to the right
};

struct TrackGenerationReport {
    double closureErrorM = 0.0;   // the gap between the end and the start before loop closure
    double rmsM = 0.0;            // GPS: RMS distance of the fixes from the smoothed centreline
    std::size_t validSamples = 0; // samples used
    // "W-TRACK-DROPOUT" (one per GPS gap, or no fix near a lap bound), "W-TRACK-EDGE-SKIPPED" (an inner or
    // outer lap without enough fixes), "W-TRACK-NOT-A-LOOP" (Speed+LatG turning less than half a revolution)
    std::vector<std::string> diagnostics;
};

struct GeneratedTrack {
    TrackMap map; // name/venue empty (the caller sets them); sections empty (SectionDetector::defaults)
    TrackGenerationReport report;
};

class TrackGenerator {
public:
    // NotFound when the lap holds too few valid samples (10), Invalid for a signal without a rate, an empty
    // lap window or options out of range (stepM <= 0, smoothingM < 0), Cancelled.
    static Result<GeneratedTrack> fromSpeedLatG(const LapWindow& lap, const UniformSignal& speedMps,
                                                const UniformSignal& latAccelMps2,
                                                const TrackGeneratorOptions& options,
                                                const CancelToken& cancel);
    static Result<GeneratedTrack> fromGps(const GpsLap& lap, const TrackGeneratorOptions& options,
                                          const CancelToken& cancel);
    // Local tangent-plane (ENU) projection about `origin` (metres east, north; dist 0).
    static TrackPoint toEnu(const TrackOrigin& origin, double latDeg, double lonDeg) noexcept;
};

} // namespace esses::analysis
