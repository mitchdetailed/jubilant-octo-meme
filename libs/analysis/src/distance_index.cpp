// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "series_access.hpp"

#include <esses/analysis/distance_index.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace esses::analysis {

namespace {

Failure invalid(std::string message) {
    return Failure{Error{ErrorCode::Invalid, std::move(message), {}}};
}

// Distance at t by linear interpolation of the (strictly increasing) samples, held beyond both ends.
double interpolate(std::span<const Micros> t, std::span<const double> d, Micros at) noexcept {
    if (at <= t.front()) {
        return d.front();
    }
    if (at >= t.back()) {
        return d.back();
    }
    const auto it = std::upper_bound(t.begin(), t.end(), at);
    const auto hi = static_cast<std::size_t>(it - t.begin());
    const std::size_t lo = hi - 1;
    if (t[lo] == at) {
        return d[lo];
    }
    const double f = static_cast<double>(at - t[lo]) / static_cast<double>(t[hi] - t[lo]);
    return d[lo] + f * (d[hi] - d[lo]);
}

} // namespace

Result<DistanceIndex> DistanceIndex::fromPoints(std::vector<Micros> times, std::vector<double> dist,
                                                const DistanceIndexKey& key) {
    DistanceIndex idx;
    idx.key_ = key;
    const std::size_t n = times.size();
    // Evenly spaced inner samples allow an O(1) distAt.
    if (n >= 4) {
        const Micros step = times[2] - times[1];
        bool uniform = step > 0;
        for (std::size_t i = 2; uniform && i + 1 < n - 1; ++i) {
            uniform = times[i + 1] - times[i] == step;
        }
        idx.stepUs_ = uniform ? step : 0;
    }
    // tAtD: the first time each 0.25 m step is reached (running maximum), interpolated inside the segment.
    const auto secondsAt = [&](std::size_t i) { return static_cast<double>(times[i] - key.start) / 1e6; };
    // The rise segments (samples i-1 -> i that set a new running maximum) partition (0, length] into
    // consecutive distance ranges (previous maximum, dist[i]]: the first time any d is reached lies on the
    // segment whose range holds d. stepRise_[k] is the first segment reaching k * kStepM, so timeAt starts
    // its (short) forward search there.
    idx.tAtD_.push_back(0.0);
    idx.stepRise_.push_back(0);
    double runMax = dist[0];
    std::size_t maxAt = 0;
    std::size_t k = 1;
    for (std::size_t i = 1; i < n; ++i) {
        if (!(dist[i] > runMax)) {
            continue;
        }
        const double d0 = dist[i - 1];
        const double d1 = dist[i];
        const auto rise = static_cast<std::uint32_t>(idx.riseIdx_.size());
        idx.riseIdx_.push_back(static_cast<std::uint32_t>(i));
        while (static_cast<double>(k) * kStepM <= d1) {
            const double s = static_cast<double>(k) * kStepM;
            const double f = (s - d0) / (d1 - d0); // d0 <= runMax < s <= d1
            idx.tAtD_.push_back(secondsAt(i - 1) + f * (secondsAt(i) - secondsAt(i - 1)));
            idx.stepRise_.push_back(rise);
            ++k;
        }
        runMax = d1;
        maxAt = i;
    }
    idx.lengthM_ = std::max(0.0, runMax);
    idx.tLengthS_ = secondsAt(maxAt);
    idx.timeT_ = std::move(times);
    idx.dAtT_ = std::move(dist);
    return idx;
}

Result<DistanceIndex> DistanceIndex::fromSamples(std::span<const Micros> timesUs,
                                                 std::span<const double> distM, const DistanceIndexKey& key) {
    if (timesUs.empty() || timesUs.size() != distM.size()) {
        return invalid("DistanceIndex: times and distances must be non-empty and of equal length");
    }
    if (key.end <= key.start) {
        return invalid("DistanceIndex: empty lap [" + std::to_string(key.start) + ", " +
                       std::to_string(key.end) + ")");
    }
    for (std::size_t i = 0; i < timesUs.size(); ++i) {
        if (i > 0 && timesUs[i] <= timesUs[i - 1]) {
            return invalid("DistanceIndex: sample times must increase (index " + std::to_string(i) + ")");
        }
        if (!std::isfinite(distM[i])) {
            return invalid("DistanceIndex: non-finite distance at index " + std::to_string(i));
        }
    }
    const double d0 = interpolate(timesUs, distM, key.start);
    std::vector<Micros> times;
    std::vector<double> dist;
    const auto first = std::upper_bound(timesUs.begin(), timesUs.end(), key.start);
    const auto last = std::lower_bound(timesUs.begin(), timesUs.end(), key.end);
    const auto inner = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, last - first));
    times.reserve(inner + 2);
    dist.reserve(inner + 2);
    times.push_back(key.start);
    dist.push_back(0.0);
    for (auto it = first; it < last; ++it) {
        const auto i = static_cast<std::size_t>(it - timesUs.begin());
        times.push_back(timesUs[i]);
        dist.push_back(distM[i] - d0);
    }
    times.push_back(key.end);
    dist.push_back(interpolate(timesUs, distM, key.end) - d0);
    return fromPoints(std::move(times), std::move(dist), key);
}

Result<DistanceIndex> DistanceIndex::build(const store::Series& corrDist, const DistanceIndexKey& key,
                                           Priority prio, const CancelToken& cancel) {
    if (key.end <= key.start) {
        return invalid("DistanceIndex: empty lap [" + std::to_string(key.start) + ", " +
                       std::to_string(key.end) + ")");
    }
    // The window: the last sample at or before the start to the first sample at or after the end.
    Micros from = key.start;
    Micros to = key.end + 1;
    bool afterEnd = false;
    if (const auto* u = std::get_if<store::UniformSeries>(&corrDist)) {
        const SampleIdx n = u->size();
        if (n > 0 && u->rate.valid()) {
            const SampleIdx i0 = std::clamp<SampleIdx>(u->rate.indexAt(key.start, u->t0), 0, n - 1);
            const SampleIdx i1 = std::clamp<SampleIdx>(u->rate.indexAt(key.end - 1, u->t0) + 1, 0, n - 1);
            from = std::min(from, u->timeAt(i0));
            to = std::max(to, u->timeAt(i1) + 1);
        }
    } else {
        from = std::numeric_limits<Micros>::min(); // timestamped: visit everything, keep the neighbours
        to = std::numeric_limits<Micros>::max();
    }
    std::vector<Micros> times;
    std::vector<double> values;
    std::optional<double> held;
    std::size_t leadingInvalid = 0;
    const auto visit = [&](Micros t, double v, bool valid) {
        if (t < key.start) { // only the last sample before the start is kept
            times.clear();
            values.clear();
            leadingInvalid = 0;
        } else if (t > key.end) { // only the first sample after the end is kept
            if (afterEnd) {
                return;
            }
            afterEnd = true;
        }
        if (valid && std::isfinite(v)) {
            held = v;
        } else if (!held) {
            ++leadingInvalid;
        }
        times.push_back(t);
        values.push_back(valid && std::isfinite(v) ? v : held.value_or(0.0));
    };
    if (auto visited = detail::forEachSample(corrDist, from, to, prio, cancel, visit); !visited) {
        return Failure{visited.error()};
    }
    if (!held) {
        Error e{ErrorCode::NotFound, "DistanceIndex: no valid Corr Dist sample in the lap", {}};
        return Failure{std::move(e)};
    }
    // Leading invalid samples take the first valid value.
    if (leadingInvalid > 0 && leadingInvalid < values.size()) {
        std::fill_n(values.begin(), leadingInvalid, values[leadingInvalid]);
    }
    return fromSamples(times, values, key);
}

double DistanceIndex::distAt(Micros t) const noexcept {
    const std::size_t n = timeT_.size();
    if (n == 0) {
        return 0.0;
    }
    if (t <= timeT_.front()) {
        return dAtT_.front();
    }
    if (t >= timeT_.back()) {
        return dAtT_.back();
    }
    std::size_t k = 0;
    if (stepUs_ > 0) {
        if (t >= timeT_[1]) {
            k = 1 + static_cast<std::size_t>((t - timeT_[1]) / stepUs_);
            k = std::min(k, n - 2);
        }
    } else {
        k = static_cast<std::size_t>(std::upper_bound(timeT_.begin(), timeT_.end(), t) - timeT_.begin()) - 1;
    }
    const double f = static_cast<double>(t - timeT_[k]) / static_cast<double>(timeT_[k + 1] - timeT_[k]);
    return dAtT_[k] + f * (dAtT_[k + 1] - dAtT_[k]);
}

double DistanceIndex::timeAt(double d) const noexcept {
    if (tAtD_.empty()) {
        return 0.0;
    }
    if (!(d > 0.0)) {
        return tAtD_.front();
    }
    if (d >= lengthM_) {
        return tLengthS_;
    }
    if (riseIdx_.empty()) {
        return tAtD_.front();
    }
    // The rise segment that first reaches d (not the straight line between two steps: a stop inside a step
    // would put d up to the whole stop later than its first reach).
    // The segment lies between the first ones reaching steps k and k + 1: a binary search there (a slow car
    // at a high sample rate has many short segments per step).
    const auto k = std::min(static_cast<std::size_t>(std::floor(d / kStepM)), stepRise_.size() - 1);
    std::size_t lo = stepRise_[k];
    std::size_t hi = k + 1 < stepRise_.size() ? stepRise_[k + 1] : riseIdx_.size() - 1;
    while (lo < hi) { // the first segment whose end distance is >= d
        const std::size_t mid = lo + (hi - lo) / 2;
        if (dAtT_[riseIdx_[mid]] < d) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const std::size_t i = riseIdx_[lo];
    const double d0 = dAtT_[i - 1];
    const double d1 = dAtT_[i];
    const double t0 = static_cast<double>(timeT_[i - 1] - key_.start) / 1e6;
    const double t1 = static_cast<double>(timeT_[i] - key_.start) / 1e6;
    const double f = std::clamp((d - d0) / (d1 - d0), 0.0, 1.0); // d0 <= previous maximum < d <= d1
    return t0 + f * (t1 - t0);
}

SampleIdx DistanceIndex::indexAt(double d, Rate rate) const noexcept {
    if (!rate.valid()) {
        return 0;
    }
    return rate.indexAt(static_cast<Micros>(std::llround(timeAt(d) * 1e6)), 0);
}

} // namespace esses::analysis
