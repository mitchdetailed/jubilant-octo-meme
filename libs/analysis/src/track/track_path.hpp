// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private polyline helpers of the track work (src/track/): arc length, resampling, the adaptive Gaussian path
// smoother of TrackGenerator, headings and curvature for SectionDetector, nearest-segment queries and the
// length-unit table of track-1 documents. Everything works in ENU metres.
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esses::analysis::trackpath {

struct Xy {
    double x = 0.0;
    double y = 0.0;
};

// Metres per unit for the length units a track-1 document may name (no unit registry in the analysis
// layer); nullopt for any other spelling.
std::optional<double> lengthUnitToMetres(std::string_view unit) noexcept;

// Cumulative chord lengths: out[0] = 0, out[i] = out[i-1] + |p[i] - p[i-1]|. When `closed`, one more entry
// holds the length of the closed loop (back to p[0]).
std::vector<double> cumulative(const std::vector<Xy>& pts, bool closed);

// The polyline resampled every `step` metres along its arc length (linear interpolation), with dist set. A
// closed loop ends with the start point again at dist = length; an open path ends with its last point.
std::vector<TrackPoint> resample(const std::vector<Xy>& pts, bool closed, double step);

// The adaptive Gaussian smoother of TrackGenerator. `pts` are observations at arc positions `s` (ascending,
// from 0; `period` > 0 makes them cyclic with that period). The result samples the smoothed curve every
// `step` metres of `s` (local linear Gaussian kernel). The kernel's sigma starts from `sigmaMin` and widens
// up to 8 * sigmaMin where the curvature of a pilot (kernel 2 * sigmaMin, chords of +-4 sigmaMin) allows it
// (sigma^2 * |kappa| / 2 <= maxBiasM within +-2 sigma), so straights are smoothed hard and corners keep their
// radius. sigmaMin <= 0 returns the observations resampled without smoothing. Cancelled.
Result<std::vector<Xy>> smoothAdaptive(const std::vector<Xy>& pts, const std::vector<double>& s,
                                       double period, double step, double sigmaMin, double maxBiasM,
                                       const CancelToken& cancel);

// Headings (radians, unwrapped) of a uniformly spaced polyline (spacing `step`) from the chords
// p[i + h] - p[i - h], h = max(1, round(halfChordM / step)) samples; cyclic when `closed` (the closing point
// at dist = length, if present, is ignored by the caller).
std::vector<double> chordHeadings(const std::vector<Xy>& pts, double step, double halfChordM, bool closed);
// Curvature (1/m, positive turning left) from such headings: (theta[i + h] - theta[i - h]) / (2 h step).
std::vector<double> chordCurvature(const std::vector<double>& headings, double step, double halfChordM,
                                   bool closed);

// Nearest points on a polyline (segments p[i] -> p[i+1], plus the closing one when `closed`) through a
// uniform grid of cells.
class NearestSegment {
public:
    NearestSegment(std::vector<Xy> pts, std::vector<double> dist, bool closed, double cellM = 25.0);
    struct Hit {
        double distanceM = 0.0; // from the query point
        double alongM = 0.0;    // the `dist` interpolated at the foot point
        Xy foot;
    };
    // nullopt for an empty polyline.
    std::optional<Hit> nearest(Xy q) const;

private:
    struct Key {
        long long cx = 0;
        long long cy = 0;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return static_cast<std::size_t>(k.cx * 73856093LL) ^ static_cast<std::size_t>(k.cy * 19349663LL);
        }
    };
    Key keyOf(Xy p) const noexcept;
    std::vector<Xy> pts_;
    std::vector<double> dist_;
    bool closed_ = false;
    double cell_ = 25.0;
    std::size_t segments_ = 0;
    std::unordered_map<Key, std::vector<std::size_t>, KeyHash> grid_;
    long long ringLimit_ = 0;
};

// The signed area of a closed polygon (shoelace): positive when counter-clockwise.
double signedArea(const std::vector<Xy>& pts) noexcept;

} // namespace esses::analysis::trackpath
