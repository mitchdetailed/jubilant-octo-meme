// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helper of esses_analysis: visits the samples of a store::Series inside a time window, pinning them
// through Column::require (worker threads and tools only; Rule W) one chunk-sized block at a time, so a whole
// extent never stays pinned at once.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace esses::analysis::detail {

inline Failure cancelledFailure() {
    return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
}

// Calls fn(Micros t, double value, bool valid) for every sample with from <= t < to, in time order. The value
// is NaN when the sample is invalid. Errors: Cancelled, and the Column::require errors.
template <class Fn>
Result<void> forEachSample(const store::Series& series, Micros from, Micros to, Priority prio,
                           const CancelToken& cancel, Fn&& fn) {
    if (to <= from) {
        return ok();
    }
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        const SampleIdx n = u->size();
        if (n <= 0 || !u->rate.valid()) {
            return ok();
        }
        // First sample at or after `from`, last sample before `to`.
        SampleIdx first =
            from == std::numeric_limits<Micros>::min() ? 0 : u->rate.indexAt(from - 1, u->t0) + 1;
        SampleIdx last = u->rate.indexAt(to - 1, u->t0);
        first = std::max<SampleIdx>(first, 0);
        last = std::min<SampleIdx>(last, n - 1);
        for (SampleIdx a = first; a <= last;) {
            if (cancel.checkpoint()) {
                return cancelledFailure();
            }
            const SampleIdx b = std::min<SampleIdx>(last, a + store::kChunkSamples - 1);
            auto pinned = u->values.require(a, b, prio, cancel);
            if (!pinned) {
                return Failure{pinned.error()};
            }
            for (SampleIdx i = a; i <= b; ++i) {
                const bool valid = pinned->validAt(i);
                fn(u->timeAt(i), valid ? pinned->at(i) : std::numeric_limits<double>::quiet_NaN(), valid);
            }
            a = b + 1;
        }
        return ok();
    }
    const auto& ts = std::get<store::TimestampedSeries>(series);
    const SampleIdx n = ts.size();
    if (n <= 0 || ts.tMax < from || ts.tMin >= to) {
        return ok();
    }
    for (SampleIdx a = 0; a < n;) {
        if (cancel.checkpoint()) {
            return cancelledFailure();
        }
        const SampleIdx b = std::min<SampleIdx>(n - 1, a + store::kChunkSamples - 1);
        auto times = ts.timesUs.require(a, b, prio, cancel);
        if (!times) {
            return Failure{times.error()};
        }
        const std::optional<std::int64_t> lastT = times->int64At(b);
        if (lastT && *lastT < from) {
            a = b + 1;
            continue;
        }
        auto values = ts.values.require(a, b, prio, cancel);
        if (!values) {
            return Failure{values.error()};
        }
        for (SampleIdx i = a; i <= b; ++i) {
            const std::optional<std::int64_t> t = times->int64At(i);
            if (!t || *t < from) {
                continue;
            }
            if (*t >= to) {
                return ok();
            }
            const bool valid = values->validAt(i);
            fn(static_cast<Micros>(*t), valid ? values->at(i) : std::numeric_limits<double>::quiet_NaN(),
               valid);
        }
        a = b + 1;
    }
    return ok();
}

// Every sample of the series.
template <class Fn>
Result<void> forEachSample(const store::Series& series, Priority prio, const CancelToken& cancel, Fn&& fn) {
    return forEachSample(series, std::numeric_limits<Micros>::min(), std::numeric_limits<Micros>::max(), prio,
                         cancel, std::forward<Fn>(fn));
}

} // namespace esses::analysis::detail
