// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Polyline helpers of the track work (track_path.hpp).

#include "track_path.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace esses::analysis::trackpath {

namespace {

constexpr double kPi = std::numbers::pi;

double wrapPi(double a) noexcept {
    while (a > kPi) {
        a -= 2.0 * kPi;
    }
    while (a <= -kPi) {
        a += 2.0 * kPi;
    }
    return a;
}

Failure cancelled() {
    return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
}

// The local linear (kernel-weighted least-squares line) estimate at arc position u with a Gaussian kernel:
// unlike a kernel average it has no bias at the ends of an open path or where the observations thin out;
// a kernel average where the line is undetermined; nullopt when no observation lies within three sigma.
std::optional<Xy> kernelAt(const std::vector<Xy>& pts, const std::vector<double>& s, double period, double u,
                           double sigma) {
    const double reach = 3.0 * sigma;
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    double x0 = 0.0;
    double x1 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    const std::array<double, 3> shifts{0.0, -period, period};
    const std::size_t nShifts = period > 0.0 ? 3 : 1;
    for (std::size_t k = 0; k < nShifts; ++k) {
        const double shift = shifts[k];
        // Observations with s_i + shift in [u - reach, u + reach].
        const auto first = std::lower_bound(s.begin(), s.end(), u - reach - shift);
        for (auto it = first; it != s.end() && *it + shift <= u + reach; ++it) {
            const double d = *it + shift - u;
            const double w = std::exp(-d * d * inv2s2);
            const auto i = static_cast<std::size_t>(it - s.begin());
            s0 += w;
            s1 += w * d;
            s2 += w * d * d;
            x0 += w * pts[i].x;
            x1 += w * d * pts[i].x;
            y0 += w * pts[i].y;
            y1 += w * d * pts[i].y;
        }
    }
    if (s0 <= 1e-12) {
        return std::nullopt;
    }
    const double det = s0 * s2 - s1 * s1;
    if (det <= 1e-9 * s0 * s0 * sigma * sigma) {
        return Xy{x0 / s0, y0 / s0};
    }
    return Xy{(s2 * x0 - s1 * x1) / det, (s2 * y0 - s1 * y1) / det};
}

// Linear interpolation of the observations at arc position u (the fallback where the kernel finds nothing).
Xy linearAt(const std::vector<Xy>& pts, const std::vector<double>& s, double period, double u) {
    const std::size_t n = pts.size();
    if (n == 1) {
        return pts[0];
    }
    const auto hi = std::upper_bound(s.begin(), s.end(), u);
    if (hi == s.begin()) {
        if (period > 0.0) {
            const double span = s.front() + period - s.back();
            const double t = span > 0.0 ? (u + period - s.back()) / span : 0.0;
            return Xy{pts.back().x + t * (pts.front().x - pts.back().x),
                      pts.back().y + t * (pts.front().y - pts.back().y)};
        }
        return pts.front();
    }
    if (hi == s.end()) {
        if (period > 0.0) {
            const double span = s.front() + period - s.back();
            const double t = span > 0.0 ? (u - s.back()) / span : 0.0;
            return Xy{pts.back().x + t * (pts.front().x - pts.back().x),
                      pts.back().y + t * (pts.front().y - pts.back().y)};
        }
        return pts.back();
    }
    const auto j = static_cast<std::size_t>(hi - s.begin());
    const double span = s[j] - s[j - 1];
    const double t = span > 0.0 ? (u - s[j - 1]) / span : 0.0;
    return Xy{pts[j - 1].x + t * (pts[j].x - pts[j - 1].x), pts[j - 1].y + t * (pts[j].y - pts[j - 1].y)};
}

// The maximum of |v| over [i - w, i + w] for every i (cyclic or clamped), w in samples.
std::vector<double> windowMaxAbs(const std::vector<double>& v, std::size_t w, bool cyclic) {
    const std::size_t n = v.size();
    std::vector<double> out(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double m = 0.0;
        for (std::size_t k = 0; k <= 2 * w; ++k) {
            const auto off = static_cast<long long>(k) - static_cast<long long>(w);
            long long j = static_cast<long long>(i) + off;
            if (cyclic) {
                j = ((j % static_cast<long long>(n)) + static_cast<long long>(n)) % static_cast<long long>(n);
            } else if (j < 0 || j >= static_cast<long long>(n)) {
                continue;
            }
            m = std::max(m, std::abs(v[static_cast<std::size_t>(j)]));
        }
        out[i] = m;
    }
    return out;
}

} // namespace

std::optional<double> lengthUnitToMetres(std::string_view unit) noexcept {
    if (unit == "m") {
        return 1.0;
    }
    if (unit == "km") {
        return 1000.0;
    }
    if (unit == "cm") {
        return 0.01;
    }
    if (unit == "mm") {
        return 0.001;
    }
    if (unit == "mi") {
        return 1609.344;
    }
    if (unit == "ft") {
        return 0.3048;
    }
    if (unit == "yd") {
        return 0.9144;
    }
    if (unit == "in") {
        return 0.0254;
    }
    return std::nullopt;
}

std::vector<double> cumulative(const std::vector<Xy>& pts, bool closed) {
    std::vector<double> out;
    out.reserve(pts.size() + 1);
    double acc = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        }
        out.push_back(acc);
    }
    if (closed && !pts.empty()) {
        acc += std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y);
        out.push_back(acc);
    }
    return out;
}

std::vector<TrackPoint> resample(const std::vector<Xy>& pts, bool closed, double step) {
    std::vector<TrackPoint> out;
    if (pts.empty()) {
        return out;
    }
    std::vector<Xy> path = pts;
    if (closed) {
        path.push_back(pts.front());
    }
    const std::vector<double> s = cumulative(path, false);
    const double length = s.back();
    if (length <= 0.0 || step <= 0.0) {
        out.push_back(TrackPoint{pts.front().x, pts.front().y, 0.0});
        return out;
    }
    const auto count = static_cast<std::size_t>(std::floor(length / step + 1e-9));
    out.reserve(count + 2);
    std::size_t j = 1;
    for (std::size_t k = 0; k <= count; ++k) {
        const double u = std::min(length, static_cast<double>(k) * step);
        while (j + 1 < s.size() && s[j] < u) {
            ++j;
        }
        const double span = s[j] - s[j - 1];
        const double t = span > 0.0 ? std::clamp((u - s[j - 1]) / span, 0.0, 1.0) : 0.0;
        out.push_back(TrackPoint{path[j - 1].x + t * (path[j].x - path[j - 1].x),
                                 path[j - 1].y + t * (path[j].y - path[j - 1].y), u});
    }
    if (length - out.back().dist > 1e-9) {
        out.push_back(TrackPoint{path.back().x, path.back().y, length});
    } else {
        out.back() = TrackPoint{path.back().x, path.back().y, length};
    }
    return out;
}

Result<std::vector<Xy>> smoothAdaptive(const std::vector<Xy>& pts, const std::vector<double>& s,
                                       double period, double step, double sigmaMin, double maxBiasM,
                                       const CancelToken& cancel) {
    std::vector<Xy> out;
    if (pts.empty() || pts.size() != s.size() || step <= 0.0) {
        return out;
    }
    const bool cyclic = period > 0.0;
    const double first = s.front();
    const double span = cyclic ? period : s.back() - first;
    const auto count = static_cast<std::size_t>(std::floor(span / step + 1e-9)) + (cyclic ? 0U : 1U);
    std::vector<double> u(std::max<std::size_t>(count, 1));
    for (std::size_t k = 0; k < u.size(); ++k) {
        u[k] = first + std::min(span, static_cast<double>(k) * step);
    }
    if (sigmaMin <= 0.0) {
        out.reserve(u.size());
        for (const double v : u) {
            out.push_back(linearAt(pts, s, period, v));
        }
        return out;
    }

    // The pilot: twice the minimum kernel everywhere, then its curvature over chords of +-4 sigmaMin (the
    // pilot only chooses the kernel, so it may be smoother than the result).
    std::vector<Xy> pilot;
    pilot.reserve(u.size());
    for (std::size_t k = 0; k < u.size(); ++k) {
        if ((k & 1023U) == 0 && cancel.cancelled()) {
            return cancelled();
        }
        pilot.push_back(
            kernelAt(pts, s, period, u[k], 2.0 * sigmaMin).value_or(linearAt(pts, s, period, u[k])));
    }
    const double halfChord = std::max(4.0 * sigmaMin, 5.0);
    const std::vector<double> kappa =
        chordCurvature(chordHeadings(pilot, step, halfChord, cyclic), step, halfChord, cyclic);

    // The widest candidate whose bias bound holds over +-2 sigma.
    static constexpr std::array<double, 7> kFactors{8.0, 6.0, 4.0, 3.0, 2.0, 1.5, 1.0};
    std::vector<double> sigma(u.size(), sigmaMin);
    std::vector<bool> chosen(u.size(), false);
    for (const double f : kFactors) {
        const double sg = f * sigmaMin;
        const auto w = static_cast<std::size_t>(std::ceil(2.0 * sg / step));
        const std::vector<double> kmax = windowMaxAbs(kappa, w, cyclic);
        for (std::size_t k = 0; k < u.size(); ++k) {
            if (!chosen[k] && (f == 1.0 || 0.5 * sg * sg * kmax[k] <= maxBiasM)) {
                sigma[k] = sg;
                chosen[k] = true;
            }
        }
        if (cancel.cancelled()) {
            return cancelled();
        }
    }
    out.reserve(u.size());
    for (std::size_t k = 0; k < u.size(); ++k) {
        if ((k & 1023U) == 0 && cancel.cancelled()) {
            return cancelled();
        }
        out.push_back(kernelAt(pts, s, period, u[k], sigma[k]).value_or(pilot[k]));
    }
    return out;
}

std::vector<double> chordHeadings(const std::vector<Xy>& pts, double step, double halfChordM, bool closed) {
    const std::size_t n = pts.size();
    std::vector<double> out(n, 0.0);
    if (n < 2) {
        return out;
    }
    const auto h = static_cast<long long>(std::max(1.0, std::round(halfChordM / step)));
    const auto nn = static_cast<long long>(n);
    double prev = 0.0;
    for (long long i = 0; i < nn; ++i) {
        long long a = i - h;
        long long b = i + h;
        if (closed) {
            a = ((a % nn) + nn) % nn;
            b = ((b % nn) + nn) % nn;
        } else {
            a = std::max(0LL, a);
            b = std::min(nn - 1, b);
        }
        const Xy pa = pts[static_cast<std::size_t>(a)];
        const Xy pb = pts[static_cast<std::size_t>(b)];
        double theta = std::atan2(pb.y - pa.y, pb.x - pa.x);
        if (i > 0) {
            theta = prev + wrapPi(theta - prev);
        }
        out[static_cast<std::size_t>(i)] = theta;
        prev = theta;
    }
    return out;
}

std::vector<double> chordCurvature(const std::vector<double>& headings, double step, double halfChordM,
                                   bool closed) {
    const std::size_t n = headings.size();
    std::vector<double> out(n, 0.0);
    if (n < 3) {
        return out;
    }
    const auto h = static_cast<long long>(std::max(1.0, std::round(halfChordM / step)));
    const auto nn = static_cast<long long>(n);
    for (long long i = 0; i < nn; ++i) {
        long long a = i - h;
        long long b = i + h;
        if (closed) {
            a = ((a % nn) + nn) % nn;
            b = ((b % nn) + nn) % nn;
        } else {
            a = std::max(0LL, a);
            b = std::min(nn - 1, b);
        }
        const double spanSamples = closed ? static_cast<double>(2 * h) : static_cast<double>(b - a);
        if (spanSamples <= 0.0) {
            continue;
        }
        const double d =
            wrapPi(headings[static_cast<std::size_t>(b)] - headings[static_cast<std::size_t>(a)]);
        out[static_cast<std::size_t>(i)] = d / (spanSamples * step);
    }
    return out;
}

NearestSegment::NearestSegment(std::vector<Xy> pts, std::vector<double> dist, bool closed, double cellM)
    : pts_(std::move(pts)), dist_(std::move(dist)), closed_(closed), cell_(cellM > 0.0 ? cellM : 25.0) {
    const std::size_t n = pts_.size();
    if (dist_.size() != n) {
        dist_ = cumulative(pts_, false);
    }
    segments_ = n < 2 ? 0 : (closed_ ? n : n - 1);
    double minX = std::numeric_limits<double>::infinity();
    double minY = minX;
    double maxX = -minX;
    double maxY = -minX;
    for (std::size_t i = 0; i < segments_; ++i) {
        const Xy a = pts_[i];
        const Xy b = pts_[(i + 1) % n];
        const Key ka = keyOf(Xy{std::min(a.x, b.x), std::min(a.y, b.y)});
        const Key kb = keyOf(Xy{std::max(a.x, b.x), std::max(a.y, b.y)});
        for (long long cx = ka.cx; cx <= kb.cx; ++cx) {
            for (long long cy = ka.cy; cy <= kb.cy; ++cy) {
                grid_[Key{cx, cy}].push_back(i);
            }
        }
        minX = std::min({minX, a.x, b.x});
        minY = std::min({minY, a.y, b.y});
        maxX = std::max({maxX, a.x, b.x});
        maxY = std::max({maxY, a.y, b.y});
    }
    if (segments_ > 0) {
        const double extent = std::max(maxX - minX, maxY - minY);
        ringLimit_ = static_cast<long long>(std::ceil(extent / cell_)) + 2;
    }
}

NearestSegment::Key NearestSegment::keyOf(Xy p) const noexcept {
    return Key{static_cast<long long>(std::floor(p.x / cell_)),
               static_cast<long long>(std::floor(p.y / cell_))};
}

std::optional<NearestSegment::Hit> NearestSegment::nearest(Xy q) const {
    const std::size_t n = pts_.size();
    if (n == 0) {
        return std::nullopt;
    }
    if (segments_ == 0) {
        return Hit{std::hypot(q.x - pts_[0].x, q.y - pts_[0].y), dist_.empty() ? 0.0 : dist_[0], pts_[0]};
    }
    std::optional<Hit> best;
    const auto consider = [&](std::size_t i) {
        const Xy a = pts_[i];
        const std::size_t jb = (i + 1) % n;
        const Xy b = pts_[jb];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double len2 = dx * dx + dy * dy;
        const double t =
            len2 > 0.0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
        const Xy foot{a.x + t * dx, a.y + t * dy};
        const double d = std::hypot(q.x - foot.x, q.y - foot.y);
        if (!best || d < best->distanceM) {
            const double da = dist_[i];
            double db = jb == 0 ? da + std::sqrt(len2) : dist_[jb];
            if (db < da) {
                db = da + std::sqrt(len2);
            }
            best = Hit{d, da + t * (db - da), foot};
        }
    };
    const Key c = keyOf(q);
    // Every cell lies within the grid's extent (ringLimit_) of any one cell, so the ring distance to one cell
    // plus the extent bounds the search.
    long long far = 0;
    if (const auto any = grid_.begin(); any != grid_.end()) {
        far = std::max(std::abs(any->first.cx - c.cx), std::abs(any->first.cy - c.cy));
    }
    const long long limit = far + ringLimit_ + 1;
    for (long long r = 0; r <= limit; ++r) {
        for (long long cx = c.cx - r; cx <= c.cx + r; ++cx) {
            for (long long cy = c.cy - r; cy <= c.cy + r; ++cy) {
                if (std::max(std::abs(cx - c.cx), std::abs(cy - c.cy)) != r) {
                    continue;
                }
                const auto it = grid_.find(Key{cx, cy});
                if (it == grid_.end()) {
                    continue;
                }
                for (const std::size_t i : it->second) {
                    consider(i);
                }
            }
        }
        if (best && best->distanceM <= static_cast<double>(r) * cell_) {
            break;
        }
    }
    if (!best) {
        for (std::size_t i = 0; i < segments_; ++i) {
            consider(i);
        }
    }
    return best;
}

double signedArea(const std::vector<Xy>& pts) noexcept {
    const std::size_t n = pts.size();
    double a = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Xy p = pts[i];
        const Xy q = pts[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

} // namespace esses::analysis::trackpath
