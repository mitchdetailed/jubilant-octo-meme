// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackGenerator (06 section 13.5, AC-M3-04). Owner: the track work (src/track/).
//
// GPS: the lap's valid fixes (the bounds interpolated at the lap start and end) projected to ENU metres about
// the origin; a gap of more than 1.5 sample periods is bridged by a cubic Hermite curve whose end tangents
// come from quadratic fits over the 1.5 s on each side ("W-TRACK-DROPOUT" from 2.5 periods and 0.5 s); the
// loop is closed by joining its end to its start (the closure error is the gap between the two interpolated
// bound points); the fixes are smoothed by an adaptive local-linear Gaussian kernel along the path (sigma =
// smoothingM / 2 in the corners, up to eight times that on the straights, so that the kernel's offset towards
// the inside of a curve stays below 0.5 m) and resampled every stepM metres. Speed+LatG: dead reckoning on
// the faster signal's samples (heading rate = latAccel / speed, positive to the left, held at 0 below 2 m/s;
// trapezoidal heading and midpoint-heading positions). Loop closure removes a constant heading-rate bias so
// the lap turns by a whole number of revolutions, then distributes the remaining position gap along the
// distance travelled; the closure error reported is the raw gap before both.

#include "track_path.hpp"

#include <esses/analysis/track_generator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace esses::analysis {

namespace {

using trackpath::Xy;

constexpr std::size_t kMinSamples = 10;
constexpr double kMinSpeedMps = 2.0; // below it the heading is held
constexpr double kMaxBiasM = 0.5;    // the smoother's bound on the offset towards a curve's inside
constexpr double kFitWindowS = 1.5;  // quadratic fits beside a gap
constexpr std::size_t kFitMaxPoints = 15;
constexpr double kPi = std::numbers::pi;

// WGS-84 (NIMA TR8350.2).
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;
constexpr double kWgs84E2 = kWgs84F * (2.0 - kWgs84F);

struct Ecef {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Ecef toEcef(double latRad, double lonRad, double h) noexcept {
    const double sp = std::sin(latRad);
    const double n = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sp * sp);
    return Ecef{(n + h) * std::cos(latRad) * std::cos(lonRad), (n + h) * std::cos(latRad) * std::sin(lonRad),
                (n * (1.0 - kWgs84E2) + h) * sp};
}

Failure cancelledFailure() {
    return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
}

Failure invalidArgument(std::string message) {
    return Failure{Error{ErrorCode::Invalid, std::move(message), {}}};
}

double periodSeconds(Rate r) noexcept {
    return static_cast<double>(r.den) / static_cast<double>(r.num);
}

bool usable(const UniformSignal& s, std::size_t i) noexcept {
    return i < s.values.size() && s.validAt(i) && std::isfinite(s.values[i]);
}

// The signal at time t: the sample itself on a sample time, else linear between two usable neighbours.
std::optional<double> valueAt(const UniformSignal& s, Micros t) {
    const std::size_t n = s.size();
    if (n == 0 || !s.rate.valid() || t < s.t0) {
        return std::nullopt;
    }
    const auto i = static_cast<std::size_t>(std::max<SampleIdx>(0, s.rate.indexAt(t, s.t0)));
    if (i >= n) {
        return std::nullopt;
    }
    const Micros ti = s.rate.timeAt(static_cast<SampleIdx>(i), s.t0);
    if (ti == t) {
        return usable(s, i) ? std::optional<double>(s.values[i]) : std::nullopt;
    }
    if (i + 1 >= n || !usable(s, i) || !usable(s, i + 1)) {
        return std::nullopt;
    }
    const Micros tj = s.rate.timeAt(static_cast<SampleIdx>(i + 1), s.t0);
    const double u = static_cast<double>(t - ti) / static_cast<double>(tj - ti);
    return s.values[i] + u * (s.values[i + 1] - s.values[i]);
}

// The sample index range of `s` covering [from, to] plus one sample on each side.
std::pair<std::size_t, std::size_t> sampleRange(const UniformSignal& s, Micros from, Micros to) {
    const std::size_t n = s.size();
    if (n == 0) {
        return {0, 0};
    }
    std::size_t lo = 0;
    if (from > s.t0) {
        const auto i = static_cast<std::size_t>(std::max<SampleIdx>(0, s.rate.indexAt(from, s.t0)));
        lo = i > 0 ? i - 1 : 0;
    }
    std::size_t hi = 0;
    if (to >= s.t0) {
        hi = static_cast<std::size_t>(std::max<SampleIdx>(0, s.rate.indexAt(to, s.t0))) + 2;
    }
    return {std::min(lo, n), std::min(hi, n)};
}

struct Fix {
    Micros t = 0;
    Xy p;
    bool observed = true; // false: interpolated at a lap bound or bridged across a gap
};

// Position and velocity (m/s) at time te from a least-squares quadratic over `pts` (times relative to te).
struct Edge {
    Xy p;
    Xy v;
};

std::optional<Edge> quadraticEdge(const std::vector<Fix>& fixes, Micros te) {
    const std::size_t n = fixes.size();
    if (n == 0) {
        return std::nullopt;
    }
    if (n < 3) {
        if (n == 2) {
            const double dt = static_cast<double>(fixes[1].t - fixes[0].t) * 1e-6;
            if (dt > 0.0) {
                const Fix& near = std::abs(fixes[0].t - te) < std::abs(fixes[1].t - te) ? fixes[0] : fixes[1];
                return Edge{near.p,
                            Xy{(fixes[1].p.x - fixes[0].p.x) / dt, (fixes[1].p.y - fixes[0].p.y) / dt}};
            }
        }
        return Edge{fixes.front().p, Xy{}};
    }
    // Normal equations of [1 tau tau^2].
    std::array<double, 5> m{}; // sums of tau^k, k = 0..4
    std::array<double, 3> bx{};
    std::array<double, 3> by{};
    for (const Fix& f : fixes) {
        const double tau = static_cast<double>(f.t - te) * 1e-6;
        double pk = 1.0;
        for (std::size_t k = 0; k < 5; ++k) {
            m[k] += pk;
            if (k < 3) {
                bx[k] += pk * f.p.x;
                by[k] += pk * f.p.y;
            }
            pk *= tau;
        }
    }
    const std::array<std::array<double, 3>, 3> a{
        {{m[0], m[1], m[2]}, {m[1], m[2], m[3]}, {m[2], m[3], m[4]}}};
    const auto det3 = [](const std::array<std::array<double, 3>, 3>& q) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) -
               q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
               q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    const double d = det3(a);
    if (std::abs(d) < 1e-18) {
        return Edge{fixes.back().p, Xy{}};
    }
    const auto solve = [&](const std::array<double, 3>& b) {
        std::array<double, 2> c{}; // c0 (position), c1 (velocity) by Cramer's rule
        for (std::size_t col = 0; col < 2; ++col) {
            auto q = a;
            for (std::size_t row = 0; row < 3; ++row) {
                q[row][col] = b[row];
            }
            c[col] = det3(q) / d;
        }
        return c;
    };
    const auto cx = solve(bx);
    const auto cy = solve(by);
    return Edge{Xy{cx[0], cy[0]}, Xy{cx[1], cy[1]}};
}

struct LineResult {
    std::vector<TrackPoint> line;
    double closureErrorM = 0.0;
    double rmsM = 0.0;
    std::size_t validSamples = 0;
    std::vector<std::string> diagnostics;
};

// The geodetic fixes of a GPS lap inside [start, end] plus one on each side, projected about `origin`.
std::vector<Fix> projectedFixes(const GpsLap& lap, const TrackOrigin& origin) {
    std::vector<Fix> out;
    const UniformSignal& lat = lap.latDeg;
    const UniformSignal& lon = lap.lonDeg;
    const bool aligned = lat.rate == lon.rate && lat.t0 == lon.t0;
    const auto [lo, hi] = sampleRange(lat, lap.lap.start, lap.lap.end);
    out.reserve(hi - lo);
    for (std::size_t i = lo; i < hi; ++i) {
        if (!usable(lat, i)) {
            continue;
        }
        const Micros t = lat.rate.timeAt(static_cast<SampleIdx>(i), lat.t0);
        std::optional<double> lonValue;
        if (aligned) {
            if (usable(lon, i)) {
                lonValue = lon.values[i];
            }
        } else {
            lonValue = valueAt(lon, t);
        }
        if (!lonValue) {
            continue;
        }
        const double la = lat.values[i];
        const double lo2 = *lonValue;
        if (std::abs(la) > 90.0 || std::abs(lo2) > 180.0 || (la == 0.0 && lo2 == 0.0)) {
            continue; // out of range, or the (0, 0) of no fix
        }
        const TrackPoint e = TrackGenerator::toEnu(origin, la, lo2);
        out.push_back(Fix{t, Xy{e.x, e.y}, true});
    }
    return out;
}

// The first usable fix inside the lap window (the default origin).
std::optional<TrackOrigin> firstFix(const GpsLap& lap) {
    const UniformSignal& lat = lap.latDeg;
    const auto [lo, hi] = sampleRange(lat, lap.lap.start, lap.lap.end);
    for (std::size_t i = lo; i < hi; ++i) {
        const Micros t = lat.rate.timeAt(static_cast<SampleIdx>(i), lat.t0);
        if (t < lap.lap.start || t >= lap.lap.end || !usable(lat, i)) {
            continue;
        }
        const std::optional<double> lon = valueAt(lap.lonDeg, t);
        if (lon && !(lat.values[i] == 0.0 && *lon == 0.0) && std::abs(lat.values[i]) <= 90.0 &&
            std::abs(*lon) <= 180.0) {
            return TrackOrigin{lat.values[i], *lon, 0.0};
        }
    }
    return std::nullopt;
}

// The point at time t interpolated between the fixes around it when they are at most `maxGapUs` apart.
std::optional<Xy> interpolateAt(const std::vector<Fix>& fixes, Micros t, Micros maxGapUs) {
    const auto hi =
        std::lower_bound(fixes.begin(), fixes.end(), t, [](const Fix& f, Micros v) { return f.t < v; });
    if (hi != fixes.end() && hi->t == t) {
        return hi->p;
    }
    if (hi == fixes.begin() || hi == fixes.end()) {
        return std::nullopt;
    }
    const Fix& a = *(hi - 1);
    const Fix& b = *hi;
    if (b.t - a.t > maxGapUs) {
        return std::nullopt;
    }
    const double u = static_cast<double>(t - a.t) / static_cast<double>(b.t - a.t);
    return Xy{a.p.x + u * (b.p.x - a.p.x), a.p.y + u * (b.p.y - a.p.y)};
}

// The fixes beside a gap for the quadratic fits (observed fixes within kFitWindowS, at most kFitMaxPoints).
std::vector<Fix> fitSide(const std::vector<Fix>& path, std::size_t edge, bool before) {
    std::vector<Fix> out;
    const auto window = static_cast<Micros>(kFitWindowS * 1e6);
    if (before) {
        for (std::size_t k = edge + 1; k-- > 0 && out.size() < kFitMaxPoints;) {
            if (path[edge].t - path[k].t > window) {
                break;
            }
            if (path[k].observed) {
                out.push_back(path[k]);
            }
        }
        std::reverse(out.begin(), out.end());
    } else {
        for (std::size_t k = edge; k < path.size() && out.size() < kFitMaxPoints; ++k) {
            if (path[k].t - path[edge].t > window) {
                break;
            }
            if (path[k].observed) {
                out.push_back(path[k]);
            }
        }
    }
    return out;
}

// One GPS lap -> a smoothed, resampled line in the frame of `origin`.
Result<LineResult> gpsLine(const GpsLap& lap, const TrackOrigin& origin, const TrackGeneratorOptions& options,
                           const CancelToken& cancel) {
    const UniformSignal& lat = lap.latDeg;
    if (!lat.rate.valid() || !lap.lonDeg.rate.valid()) {
        return invalidArgument("a GPS signal without a sample rate");
    }
    if (lap.lap.end <= lap.lap.start) {
        return invalidArgument("the lap window is empty");
    }
    const double period = periodSeconds(lat.rate);
    const auto periodUs = static_cast<Micros>(std::llround(period * 1e6));
    const std::vector<Fix> fixes = projectedFixes(lap, origin);
    LineResult result;
    for (const Fix& f : fixes) {
        result.validSamples += (f.t >= lap.lap.start && f.t < lap.lap.end) ? 1U : 0U;
    }
    if (result.validSamples < kMinSamples) {
        Error e{ErrorCode::NotFound,
                "the lap holds " + std::to_string(result.validSamples) + " valid GPS fixes (at least " +
                    std::to_string(kMinSamples) + " are needed)",
                {}};
        return Failure{std::move(e)};
    }
    if (cancel.cancelled()) {
        return cancelledFailure();
    }

    // The path from the lap start to the lap end.
    const auto boundGap = static_cast<Micros>(std::llround(2.5 * period * 1e6));
    std::vector<Fix> path;
    path.reserve(fixes.size() + 2);
    const auto insideBegin =
        std::find_if(fixes.begin(), fixes.end(), [&](const Fix& f) { return f.t > lap.lap.start; });
    const auto insideEnd =
        std::find_if(fixes.begin(), fixes.end(), [&](const Fix& f) { return f.t >= lap.lap.end; });
    if (const std::optional<Xy> s = interpolateAt(fixes, lap.lap.start, boundGap)) {
        path.push_back(Fix{lap.lap.start, *s, false});
    }
    for (auto it = insideBegin; it != insideEnd; ++it) {
        path.push_back(*it);
    }
    if (const std::optional<Xy> e = interpolateAt(fixes, lap.lap.end, boundGap)) {
        path.push_back(Fix{lap.lap.end, *e, false});
    }
    if (path.size() < kMinSamples) {
        return Failure{Error{ErrorCode::NotFound, "the lap holds too few valid GPS fixes", {}}};
    }
    const double dropoutS = std::max(2.5 * period, 0.5);
    if (static_cast<double>(path.front().t - lap.lap.start) * 1e-6 > dropoutS ||
        static_cast<double>(lap.lap.end - path.back().t) * 1e-6 > dropoutS) {
        result.diagnostics.emplace_back("W-TRACK-DROPOUT"); // no fix near a lap bound
    }

    // Bridge the gaps.
    std::vector<Fix> bridged;
    bridged.reserve(path.size() + 64);
    for (std::size_t i = 0; i < path.size(); ++i) {
        bridged.push_back(path[i]);
        if (i + 1 == path.size()) {
            break;
        }
        const Fix& a = path[i];
        const Fix& b = path[i + 1];
        const double gapS = static_cast<double>(b.t - a.t) * 1e-6;
        if (gapS <= 1.5 * period) {
            continue;
        }
        if (gapS > dropoutS) {
            result.diagnostics.emplace_back("W-TRACK-DROPOUT");
        }
        const std::optional<Edge> ea = quadraticEdge(fitSide(path, i, true), a.t);
        const std::optional<Edge> eb = quadraticEdge(fitSide(path, i + 1, false), b.t);
        const Xy va = ea ? ea->v : Xy{};
        const Xy vb = eb ? eb->v : Xy{};
        for (Micros t = a.t + periodUs; t < b.t - periodUs / 2; t += periodUs) {
            const double u = static_cast<double>(t - a.t) / static_cast<double>(b.t - a.t);
            const double u2 = u * u;
            const double u3 = u2 * u;
            const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
            const double h10 = u3 - 2.0 * u2 + u;
            const double h01 = -2.0 * u3 + 3.0 * u2;
            const double h11 = u3 - u2;
            bridged.push_back(Fix{t,
                                  Xy{h00 * a.p.x + h10 * gapS * va.x + h01 * b.p.x + h11 * gapS * vb.x,
                                     h00 * a.p.y + h10 * gapS * va.y + h01 * b.p.y + h11 * gapS * vb.y},
                                  false});
        }
    }
    const Xy start = bridged.front().p;
    const Xy end = bridged.back().p;
    result.closureErrorM = std::hypot(end.x - start.x, end.y - start.y);
    if (options.closeLoop && bridged.size() > 2 && !bridged.back().observed &&
        bridged.back().t == lap.lap.end) {
        bridged.pop_back(); // the point at the lap end is the start again; a last real fix is kept
    }

    // The arc parameter from a [1 2 1] pre-smoothed copy (noise would inflate raw chord lengths).
    const std::size_t n = bridged.size();
    std::vector<Xy> pts(n);
    std::vector<Xy> pre(n);
    for (std::size_t i = 0; i < n; ++i) {
        pts[i] = bridged[i].p;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const bool cyc = options.closeLoop;
        const std::size_t ia = i > 0 ? i - 1 : (cyc ? n - 1 : 0);
        const std::size_t ib = i + 1 < n ? i + 1 : (cyc ? 0 : n - 1);
        pre[i] = Xy{0.25 * pts[ia].x + 0.5 * pts[i].x + 0.25 * pts[ib].x,
                    0.25 * pts[ia].y + 0.5 * pts[i].y + 0.25 * pts[ib].y};
    }
    std::vector<double> s = trackpath::cumulative(pre, options.closeLoop);
    double periodM = 0.0;
    if (options.closeLoop) {
        periodM = s.back();
        s.pop_back();
    }
    const double inner = std::min(1.0, options.stepM);
    auto smooth =
        trackpath::smoothAdaptive(pts, s, periodM, inner, 0.5 * options.smoothingM, kMaxBiasM, cancel);
    if (!smooth) {
        return Failure{smooth.error()};
    }
    result.line = trackpath::resample(*smooth, options.closeLoop, options.stepM);

    // The RMS distance of the observed fixes from the line.
    std::vector<Xy> linePts;
    std::vector<double> lineDist;
    for (const TrackPoint& p : result.line) {
        linePts.push_back(Xy{p.x, p.y});
        lineDist.push_back(p.dist);
    }
    const trackpath::NearestSegment nearest(std::move(linePts), std::move(lineDist), false);
    double sum = 0.0;
    std::size_t count = 0;
    for (const Fix& f : bridged) {
        if (!f.observed) {
            continue;
        }
        if (const auto hit = nearest.nearest(f.p)) {
            sum += hit->distanceM * hit->distanceM;
            ++count;
        }
    }
    result.rmsM = count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
    return result;
}

TrackDirection directionOf(const std::vector<TrackPoint>& line) {
    std::vector<Xy> pts;
    pts.reserve(line.size());
    for (const TrackPoint& p : line) {
        pts.push_back(Xy{p.x, p.y});
    }
    return trackpath::signedArea(pts) >= 0.0 ? TrackDirection::CounterClockwise : TrackDirection::Clockwise;
}

Result<void> checkOptions(const TrackGeneratorOptions& options) {
    if (!std::isfinite(options.stepM) || options.stepM <= 0.0) {
        return invalidArgument("the resampling step is a positive distance");
    }
    if (!std::isfinite(options.smoothingM) || options.smoothingM < 0.0) {
        return invalidArgument("the smoothing window is a non-negative distance");
    }
    return ok();
}

} // namespace

Result<GeneratedTrack> TrackGenerator::fromSpeedLatG(const LapWindow& lap, const UniformSignal& speedMps,
                                                     const UniformSignal& latAccelMps2,
                                                     const TrackGeneratorOptions& options,
                                                     const CancelToken& cancel) {
    if (auto c = checkOptions(options); !c) {
        return Failure{c.error()};
    }
    if (!speedMps.rate.valid() || !latAccelMps2.rate.valid()) {
        return invalidArgument("a speed or lateral acceleration signal without a sample rate");
    }
    if (lap.end <= lap.start) {
        return invalidArgument("the lap window is empty");
    }
    // The grid: the lap bounds and the faster signal's samples between them.
    const UniformSignal& grid = speedMps.rate >= latAccelMps2.rate ? speedMps : latAccelMps2;
    std::vector<Micros> times;
    times.push_back(lap.start);
    {
        const auto [lo, hi] = sampleRange(grid, lap.start, lap.end);
        for (std::size_t i = lo; i < hi; ++i) {
            const Micros t = grid.rate.timeAt(static_cast<SampleIdx>(i), grid.t0);
            if (t > lap.start && t < lap.end) {
                times.push_back(t);
            }
        }
    }
    times.push_back(lap.end);
    const std::size_t n = times.size();
    std::vector<double> v(n, std::numeric_limits<double>::quiet_NaN());
    std::vector<double> a(n, std::numeric_limits<double>::quiet_NaN());
    std::size_t valid = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::optional<double> vs = valueAt(speedMps, times[k]);
        const std::optional<double> as = valueAt(latAccelMps2, times[k]);
        if (vs && as) {
            v[k] = *vs;
            a[k] = options.invertLatAccel ? -*as : *as;
            valid += (times[k] < lap.end) ? 1U : 0U;
        }
    }
    if (valid < kMinSamples) {
        return Failure{Error{ErrorCode::NotFound,
                             "the lap holds " + std::to_string(valid) +
                                 " samples with both speed and lateral acceleration (at least " +
                                 std::to_string(kMinSamples) + " are needed)",
                             {}}};
    }
    // Invalid samples hold the previous usable pair (the first usable one before any).
    std::size_t firstUsable = 0;
    while (firstUsable < n && !std::isfinite(v[firstUsable])) {
        ++firstUsable;
    }
    for (std::size_t k = 0; k < n; ++k) {
        if (!std::isfinite(v[k])) {
            const std::size_t src = k < firstUsable ? firstUsable : k - 1;
            v[k] = v[src];
            a[k] = a[src];
        }
    }
    if (cancel.cancelled()) {
        return cancelledFailure();
    }

    // Dead reckoning.
    std::vector<double> theta(n, 0.0);
    std::vector<double> dist(n, 0.0);
    const auto omega = [&](std::size_t k) { return v[k] > kMinSpeedMps ? a[k] / v[k] : 0.0; };
    for (std::size_t k = 0; k + 1 < n; ++k) {
        const double dt = static_cast<double>(times[k + 1] - times[k]) * 1e-6;
        theta[k + 1] = theta[k] + 0.5 * (omega(k) + omega(k + 1)) * dt;
        dist[k + 1] = dist[k] + 0.5 * (std::max(v[k], 0.0) + std::max(v[k + 1], 0.0)) * dt;
    }
    const auto integrate = [&](const std::vector<double>& heading) {
        std::vector<Xy> p(n);
        for (std::size_t k = 0; k + 1 < n; ++k) {
            const double ds = dist[k + 1] - dist[k];
            const double hm = 0.5 * (heading[k] + heading[k + 1]);
            p[k + 1] = Xy{p[k].x + ds * std::cos(hm), p[k].y + ds * std::sin(hm)};
        }
        return p;
    };
    std::vector<Xy> pts = integrate(theta);
    GeneratedTrack out;
    out.report.validSamples = valid;
    out.report.closureErrorM = std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y);
    const double length = dist.back();
    if (length <= 0.0) {
        return Failure{Error{ErrorCode::NotFound, "the car does not move during the lap", {}}};
    }
    if (options.closeLoop) {
        const double turns = std::round(theta.back() / (2.0 * kPi));
        if (turns != 0.0) {
            const double excess = theta.back() - turns * 2.0 * kPi;
            const auto span = static_cast<double>(times.back() - times.front());
            for (std::size_t k = 0; k < n; ++k) {
                theta[k] -= excess * static_cast<double>(times[k] - times.front()) / span;
            }
            pts = integrate(theta);
        } else {
            out.report.diagnostics.emplace_back("W-TRACK-NOT-A-LOOP");
        }
        const Xy gap{pts.back().x - pts.front().x, pts.back().y - pts.front().y};
        for (std::size_t k = 0; k < n; ++k) {
            const double f = dist[k] / length;
            pts[k].x -= f * gap.x;
            pts[k].y -= f * gap.y;
        }
        pts.pop_back(); // the end joins the start
        dist.pop_back();
    }
    if (cancel.cancelled()) {
        return cancelledFailure();
    }
    const double inner = std::min(1.0, options.stepM);
    auto smooth = trackpath::smoothAdaptive(pts, dist, options.closeLoop ? length : 0.0, inner,
                                            0.5 * options.smoothingM, kMaxBiasM, cancel);
    if (!smooth) {
        return Failure{smooth.error()};
    }
    TrackMap& map = out.map;
    map.centre = trackpath::resample(*smooth, options.closeLoop, options.stepM);
    map.closed = options.closeLoop;
    map.lengthM = map.centre.empty() ? 0.0 : map.centre.back().dist;
    map.direction = directionOf(map.centre);
    map.origin = options.origin.value_or(TrackOrigin{});
    map.generatedFrom.method = TrackMethod::SpeedLatG;
    return out;
}

Result<GeneratedTrack> TrackGenerator::fromGps(const GpsLap& lap, const TrackGeneratorOptions& options,
                                               const CancelToken& cancel) {
    if (auto c = checkOptions(options); !c) {
        return Failure{c.error()};
    }
    if (!lap.latDeg.rate.valid() || !lap.lonDeg.rate.valid()) {
        return invalidArgument("a GPS signal without a sample rate");
    }
    std::optional<TrackOrigin> origin = options.origin;
    if (!origin) {
        origin = firstFix(lap);
    }
    if (!origin) {
        return Failure{Error{ErrorCode::NotFound, "the lap holds no valid GPS fix", {}}};
    }
    auto centre = gpsLine(lap, *origin, options, cancel);
    if (!centre) {
        return Failure{centre.error()};
    }
    GeneratedTrack out;
    TrackMap& map = out.map;
    map.centre = std::move(centre->line);
    map.closed = options.closeLoop;
    map.lengthM = map.centre.empty() ? 0.0 : map.centre.back().dist;
    map.direction = directionOf(map.centre);
    map.origin = *origin;
    map.generatedFrom.method = TrackMethod::Gps;
    out.report.closureErrorM = centre->closureErrorM;
    out.report.rmsM = centre->rmsM;
    out.report.validSamples = centre->validSamples;
    out.report.diagnostics = std::move(centre->diagnostics);

    // The inner and outer lines, with dist = the centreline distance of their nearest centreline point.
    std::vector<Xy> centrePts;
    std::vector<double> centreDist;
    for (const TrackPoint& p : map.centre) {
        centrePts.push_back(Xy{p.x, p.y});
        centreDist.push_back(p.dist);
    }
    const trackpath::NearestSegment onCentre(std::move(centrePts), std::move(centreDist), false);
    const auto edge = [&](const std::optional<GpsLap>& edgeLap,
                          std::vector<TrackPoint>& line) -> Result<void> {
        if (!edgeLap) {
            return ok();
        }
        auto built = gpsLine(*edgeLap, *origin, options, cancel);
        if (!built) {
            if (built.error().code == ErrorCode::Cancelled) {
                return Failure{built.error()};
            }
            out.report.diagnostics.emplace_back("W-TRACK-EDGE-SKIPPED");
            return ok();
        }
        for (std::string& d : built->diagnostics) {
            out.report.diagnostics.push_back(std::move(d));
        }
        line = std::move(built->line);
        for (TrackPoint& p : line) {
            if (const auto hit = onCentre.nearest(Xy{p.x, p.y})) {
                p.dist = map.closed && hit->alongM >= map.lengthM ? 0.0 : hit->alongM;
            }
        }
        return ok();
    };
    if (auto r = edge(options.innerLap, map.inner); !r) {
        return Failure{r.error()};
    }
    if (auto r = edge(options.outerLap, map.outer); !r) {
        return Failure{r.error()};
    }
    return out;
}

TrackPoint TrackGenerator::toEnu(const TrackOrigin& origin, double latDeg, double lonDeg) noexcept {
    constexpr double kDeg = kPi / 180.0;
    const double lat0 = origin.lat * kDeg;
    const double lon0 = origin.lon * kDeg;
    const Ecef o = toEcef(lat0, lon0, origin.alt);
    const Ecef p = toEcef(latDeg * kDeg, lonDeg * kDeg, origin.alt);
    const double dx = p.x - o.x;
    const double dy = p.y - o.y;
    const double dz = p.z - o.z;
    const double sl = std::sin(lon0);
    const double cl = std::cos(lon0);
    const double sp = std::sin(lat0);
    const double cp = std::cos(lat0);
    return TrackPoint{-sl * dx + cl * dy, -sp * cl * dx - sp * sl * dy + cp * dz, 0.0};
}

} // namespace esses::analysis
