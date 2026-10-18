// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Streaming series reads of the binners (series_cursor.hpp).

#include "series_cursor.hpp"

#include <utility>

namespace esses::analysis::detail {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

Result<void> PinnedColumn::ensure(SampleIdx i) {
    if (!handle_.empty() && handle_.contains(i)) {
        return ok();
    }
    if (i < 0 || i >= size_) {
        return Failure{Error{ErrorCode::Internal, "sample index outside the column", {}}};
    }
    if (cancel_.checkpoint()) {
        return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
    }
    const SampleIdx first = (i >> store::kChunkShift) << store::kChunkShift;
    const SampleIdx last = std::min<SampleIdx>(size_ - 1, first + 2 * SampleIdx{store::kChunkSamples} - 1);
    handle_.reset();
    auto pinned = column_->require(first, last, prio_, cancel_);
    if (!pinned) {
        return Failure{pinned.error()};
    }
    handle_ = std::move(pinned).value();
    return ok();
}

Micros PinnedColumn::time(SampleIdx i) const noexcept {
    const std::optional<std::int64_t> t = handle_.int64At(i);
    return t ? static_cast<Micros>(*t) : Micros{0};
}

Result<SampleIdx> firstStampedAtOrAfter(PinnedColumn& times, Micros t) {
    SampleIdx lo = 0;
    SampleIdx hi = times.size();
    while (lo < hi) {
        const SampleIdx mid = lo + (hi - lo) / 2;
        if (auto pinned = times.ensure(mid); !pinned) {
            return Failure{pinned.error()};
        }
        if (times.time(mid) < t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

SeriesCursor::SeriesCursor(const store::Series& series, store::Interp interp, Priority prio,
                           const CancelToken& cancel)
    : interp_(interp), values_(store::seriesValues(series), prio, cancel) {
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        uniform_ = u;
        n_ = u->rate.valid() ? u->size() : 0;
        if (n_ > 0) {
            tFirst_ = u->timeAt(0);
            tLast_ = u->timeAt(n_ - 1);
        }
    } else {
        stamped_ = &std::get<store::TimestampedSeries>(series);
        n_ = stamped_->size();
        times_.emplace(stamped_->timesUs, prio, cancel);
        if (n_ > 0) {
            tFirst_ = stamped_->tMin;
            tLast_ = stamped_->tMax;
        }
    }
}

Result<Micros> SeriesCursor::timeOf(SampleIdx i) {
    if (!times_) {
        return uniform_ != nullptr ? uniform_->timeAt(i) : Micros{0};
    }
    PinnedColumn& times = *times_;
    if (auto pinned = times.ensure(i); !pinned) {
        return Failure{pinned.error()};
    }
    return times.time(i);
}

Result<double> SeriesCursor::valueOf(SampleIdx i) {
    if (auto pinned = values_.ensure(i); !pinned) {
        return Failure{pinned.error()};
    }
    const double v = values_.value(i);
    return values_.valid(i) && std::isfinite(v) ? v : kNaN;
}

Result<SampleIdx> SeriesCursor::indexAtOrBefore(Micros t) {
    if (uniform_ != nullptr) {
        return std::clamp<SampleIdx>(uniform_->indexAt(t), 0, n_ - 1);
    }
    // Timestamped: gallop forward from the previous answer, else a binary search from the start.
    SampleIdx lo = 0;  // time(lo) <= t
    SampleIdx hi = n_; // time(hi) > t (or n_)
    if (last_ >= 0) {
        auto tl = timeOf(last_);
        if (!tl) {
            return Failure{tl.error()};
        }
        if (*tl <= t) {
            lo = last_;
            SampleIdx step = 1;
            hi = lo + 1;
            while (hi < n_) {
                auto th = timeOf(hi);
                if (!th) {
                    return Failure{th.error()};
                }
                if (*th > t) {
                    break;
                }
                lo = hi;
                step *= 2;
                hi = std::min<SampleIdx>(n_, lo + step);
            }
        }
    }
    while (hi - lo > 1) {
        const SampleIdx mid = lo + (hi - lo) / 2;
        auto tm = timeOf(mid);
        if (!tm) {
            return Failure{tm.error()};
        }
        if (*tm <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    last_ = lo;
    return lo;
}

Result<double> SeriesCursor::at(Micros t) {
    if (n_ <= 0 || t < tFirst_ || t > tLast_) {
        return kNaN;
    }
    auto found = indexAtOrBefore(t);
    if (!found) {
        return Failure{found.error()};
    }
    const SampleIdx j = *found;
    auto tj = timeOf(j);
    if (!tj) {
        return Failure{tj.error()};
    }
    if (*tj == t || interp_ == store::Interp::Previous) {
        return valueOf(j);
    }
    // Not an exact hit and t <= tLast: sample j + 1 exists.
    const SampleIdx k = j + 1;
    auto tk = timeOf(k);
    if (!tk) {
        return Failure{tk.error()};
    }
    switch (interp_) {
    case store::Interp::Next:
        return valueOf(k);
    case store::Interp::Closest:
        return valueOf(t - *tj <= *tk - t ? j : k);
    case store::Interp::Linear:
    case store::Interp::Previous:
        break;
    }
    auto a = valueOf(j);
    if (!a) {
        return a;
    }
    auto b = valueOf(k);
    if (!b) {
        return b;
    }
    if (std::isnan(*a) || std::isnan(*b)) {
        return kNaN;
    }
    const double f = static_cast<double>(t - *tj) / static_cast<double>(*tk - *tj);
    return *a + f * (*b - *a);
}

} // namespace esses::analysis::detail
