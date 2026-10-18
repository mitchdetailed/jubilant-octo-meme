// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helpers of the binners (src/binning/): streaming reads of a store::Series on a worker, pinning
// through Column::require at most two chunks of a column at a time (Rule W; a whole extent is never pinned).
//
//   PinnedColumn  random access to one column: ensure(i) pins the chunk of i and the next one
//   SeriesCursor  the value of a series at non-decreasing query times under an Interp policy, with the rules
//                 of store::Resampler (02 section 3.3 rule 13): Linear interpolates between the bracketing
//                 samples (the sample itself at an exact hit) and is invalid when either is invalid; Closest
//                 takes the nearer sample (the earlier on a tie); Previous the last sample at or before t;
//                 Next the first at or after t; every policy is invalid outside [first, last sample time].
//                 Queries that go back in time are answered too (a fresh search), only slower.
//   visitSamples  the samples of a series with from <= t < to in time order, each with the time of the
//                 series' next sample (the period a binner credits to it)
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace esses::analysis::detail {

class PinnedColumn {
public:
    // The column must outlive this object; the token is copied (a temporary token is fine).
    PinnedColumn(const store::Column& column, Priority prio, CancelToken cancel) noexcept
        : column_(&column), size_(column.size()), prio_(prio), cancel_(std::move(cancel)) {}

    SampleIdx size() const noexcept { return size_; }
    // Pins the chunk holding sample i and the following chunk (so i + 1 is readable after ensure(i)).
    Result<void> ensure(SampleIdx i);
    double value(SampleIdx i) const noexcept { return handle_.at(i); } // NaN when invalid
    bool valid(SampleIdx i) const noexcept { return handle_.validAt(i); }
    Micros time(SampleIdx i) const noexcept; // an I64 time column; 0 outside the pinned span

private:
    const store::Column* column_;
    SampleIdx size_;
    Priority prio_;
    CancelToken cancel_;
    store::ChunkHandle handle_;
};

class SeriesCursor {
public:
    // The series must outlive the cursor; the token is copied.
    SeriesCursor(const store::Series& series, store::Interp interp, Priority prio, const CancelToken& cancel);

    // The value at t; NaN when invalid, outside the samples or not finite. Errors: Cancelled and the
    // Column::require errors.
    Result<double> at(Micros t);

private:
    // The last sample index with time <= t (t within [tFirst, tLast]).
    Result<SampleIdx> indexAtOrBefore(Micros t);
    Result<Micros> timeOf(SampleIdx i);
    Result<double> valueOf(SampleIdx i); // NaN when invalid or not finite

    const store::UniformSeries* uniform_ = nullptr;
    const store::TimestampedSeries* stamped_ = nullptr;
    store::Interp interp_;
    SampleIdx n_ = 0;
    Micros tFirst_ = 0;
    Micros tLast_ = -1;
    PinnedColumn values_;
    std::optional<PinnedColumn> times_;
    SampleIdx last_ = -1; // timestamped: the index found by the previous query (-1: none yet)
};

// The first sample index with time >= t of a timestamped series (size() when none).
Result<SampleIdx> firstStampedAtOrAfter(PinnedColumn& times, Micros t);

inline constexpr SampleIdx kVisitBlock = 4096; // samples between two cancellation checkpoints

// fn(Micros t, double value, bool valid, std::optional<Micros> next) -> Result<void> for every sample with
// from <= t < to, in time order; `value` is NaN when invalid or not finite; `next` is the time of the
// series' following sample (uniform: always the next grid time; timestamped: nullopt after the last sample).
// A failed fn stops the visit and is returned. Errors: Cancelled, the Column::require errors.
template <class Fn>
Result<void> visitSamples(const store::Series& series, Micros from, Micros to, Priority prio,
                          const CancelToken& cancel, Fn&& fn) {
    if (to <= from) {
        return ok();
    }
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        const SampleIdx n = u->size();
        if (n <= 0 || !u->rate.valid()) {
            return ok();
        }
        const SampleIdx first = std::max<SampleIdx>(0, u->rate.indexAt(from - 1, u->t0) + 1);
        const SampleIdx last = std::min<SampleIdx>(n - 1, u->rate.indexAt(to - 1, u->t0));
        PinnedColumn values(u->values, prio, cancel);
        for (SampleIdx i = first; i <= last; ++i) {
            if ((i - first) % kVisitBlock == 0 && cancel.checkpoint()) {
                return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
            }
            if (auto pinned = values.ensure(i); !pinned) {
                return pinned;
            }
            const double v = values.value(i);
            const bool good = values.valid(i) && std::isfinite(v);
            if (auto r = fn(u->timeAt(i), good ? v : std::numeric_limits<double>::quiet_NaN(), good,
                            std::optional<Micros>(u->timeAt(i + 1)));
                !r) {
                return r;
            }
        }
        return ok();
    }
    const auto& ts = std::get<store::TimestampedSeries>(series);
    const SampleIdx n = ts.size();
    if (n <= 0 || ts.tMax < from || ts.tMin >= to) {
        return ok();
    }
    PinnedColumn times(ts.timesUs, prio, cancel);
    PinnedColumn values(ts.values, prio, cancel);
    auto start = firstStampedAtOrAfter(times, from);
    if (!start) {
        return Failure{start.error()};
    }
    for (SampleIdx i = *start; i < n; ++i) {
        if ((i - *start) % kVisitBlock == 0 && cancel.checkpoint()) {
            return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
        }
        if (auto pinned = times.ensure(i); !pinned) {
            return pinned;
        }
        const Micros t = times.time(i);
        if (t >= to) {
            break;
        }
        const std::optional<Micros> next =
            i + 1 < n ? std::optional<Micros>(times.time(i + 1)) : std::optional<Micros>();
        if (auto pinned = values.ensure(i); !pinned) {
            return pinned;
        }
        const double v = values.value(i);
        const bool good = values.valid(i) && std::isfinite(v);
        if (auto r = fn(t, good ? v : std::numeric_limits<double>::quiet_NaN(), good, next); !r) {
            return r;
        }
    }
    return ok();
}

} // namespace esses::analysis::detail
