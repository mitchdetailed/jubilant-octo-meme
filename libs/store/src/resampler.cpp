// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Resampler policies (02 section 3.3 rules 13-16). valueAt/sampleAt and ResampleView share resolvePolicy and
// the same index arithmetic, so a block and a single lookup at the same time agree bit for bit (rule 16).
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <utility>

namespace esses::store {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Given prev (last sample at or before t) and next (first at or after t), both inside the series, applies the
// policy. timeOf/valueOf read one sample; valueOf returns NaN for an invalid sample.
template <class TimeFn, class ValueFn>
double resolvePolicy(Interp interp, Micros t, SampleIdx prev, SampleIdx next, TimeFn&& timeOf,
                     ValueFn&& valueOf) {
    switch (interp) {
    case Interp::Previous:
        return valueOf(prev);
    case Interp::Next:
        return valueOf(next);
    case Interp::Closest: {
        const Micros tn = timeOf(next);
        if (tn == t) {
            return valueOf(next); // an exact hit (the earliest sample at t)
        }
        const Micros tp = timeOf(prev);
        return (t - tp) <= (tn - t) ? valueOf(prev) : valueOf(next); // the earlier one on a tie
    }
    case Interp::Linear: {
        const Micros tp = timeOf(prev);
        if (tp == t) {
            return valueOf(prev); // an exact hit is the sample itself
        }
        const Micros tn = timeOf(next);
        const double va = valueOf(prev);
        const double vb = valueOf(next);
        if (std::isnan(va) || std::isnan(vb)) {
            return kNaN; // invalid if either bracketing sample is invalid
        }
        if (tn == tp) {
            return va;
        }
        const double w = static_cast<double>(t - tp) / static_cast<double>(tn - tp);
        return va + (vb - va) * w;
    }
    }
    return kNaN;
}

// Uniform input: last index with time <= t, first index with time >= t (t inside [tFirst, tLast]).
SampleIdx uniformPrev(const Rate& rate, Micros t0, SampleIdx size, Micros t) noexcept {
    return std::clamp<SampleIdx>(rate.indexAt(t, t0), 0, size - 1);
}
SampleIdx uniformNext(const Rate& rate, Micros t0, SampleIdx size, Micros t) noexcept {
    return std::clamp<SampleIdx>(rate.indexAt(saturatingSub(t, 1), t0) + 1, 0, size - 1);
}

bool covers(const ChunkHandle& h, SampleIdx a, SampleIdx b) noexcept {
    if (a > b) {
        return true;
    }
    for (SampleIdx c = chunkOf(a); c <= chunkOf(b); ++c) {
        if (h.chunk(c) == nullptr) {
            return false;
        }
    }
    return true;
}

// Non-blocking reads of one column through tryPin; remembers the pins for the duration of one lookup.
class NonBlockingReader {
public:
    explicit NonBlockingReader(const Column& column) : column_(column) {}

    const ChunkHandle* pinChunk(SampleIdx c) {
        for (const ChunkHandle& h : pins_) {
            if (h.firstChunk() == c) {
                return &h;
            }
        }
        std::optional<ChunkHandle> h = column_.tryPin(c, Priority::Visible); // schedules an absent chunk
        if (!h) {
            absent_ = true;
            return nullptr;
        }
        pins_.push_back(std::move(*h));
        return &pins_.back();
    }
    double value(SampleIdx i) {
        const ChunkHandle* h = pinChunk(chunkOf(i));
        return h != nullptr ? h->at(i) : kNaN;
    }
    std::optional<Micros> time(SampleIdx i) {
        const ChunkHandle* h = pinChunk(chunkOf(i));
        if (h == nullptr) {
            return std::nullopt;
        }
        return h->int64At(i);
    }
    bool absent() const noexcept { return absent_; }

private:
    const Column& column_;
    std::deque<ChunkHandle> pins_; // stable addresses
    bool absent_ = false;
};

ValueSample finish(double v, bool absent) noexcept {
    if (absent) {
        return {kNaN, SampleState::Absent};
    }
    if (std::isnan(v)) {
        return {kNaN, SampleState::Invalid};
    }
    return {v, SampleState::Valid};
}

ValueSample sampleUniform(const UniformSeries& u, Micros t, Interp interp) {
    const SampleIdx size = u.values.size();
    if (size == 0 || !u.rate.valid() || t < u.timeAt(0) || t > u.timeAt(size - 1)) {
        return {kNaN, SampleState::Invalid};
    }
    const SampleIdx prev = uniformPrev(u.rate, u.t0, size, t);
    const SampleIdx next = uniformNext(u.rate, u.t0, size, t);
    NonBlockingReader values(u.values);
    const double v = resolvePolicy(
        interp, t, prev, next, [&](SampleIdx i) { return u.timeAt(i); },
        [&](SampleIdx i) { return values.value(i); });
    return finish(v, values.absent());
}

ValueSample sampleTimestamped(const TimestampedSeries& ts, Micros t, Interp interp) {
    const SampleIdx size = ts.size();
    if (size == 0 || ts.timesUs.size() != size || t < ts.tMin || t > ts.tMax) {
        return {kNaN, SampleState::Invalid};
    }
    NonBlockingReader times(ts.timesUs);
    const std::shared_ptr<const SummaryPyramid> bounds = ts.timesUs.summarySnapshot();
    // The last chunk whose first timestamp is <= t (chunk bounds from the summary when known).
    SampleIdx lo = 0;
    SampleIdx hi = ts.timesUs.chunkCount() - 1;
    while (lo < hi) {
        const SampleIdx mid = (lo + hi + 1) / 2;
        std::optional<Micros> first;
        if (const auto b = bounds->chunkTimeBounds(mid)) {
            first = b->first;
        } else {
            first = times.time(chunkFirstSample(mid));
        }
        if (!first) {
            return {kNaN, SampleState::Absent};
        }
        if (*first <= t) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const ChunkHandle* h = times.pinChunk(lo);
    if (h == nullptr) {
        return {kNaN, SampleState::Absent};
    }
    const std::span<const std::int64_t> s = h->samples<std::int64_t>(lo);
    const auto it = std::upper_bound(s.begin(), s.end(), t);
    const SampleIdx prev =
        chunkFirstSample(lo) + std::max<SampleIdx>(static_cast<SampleIdx>(it - s.begin()) - 1, 0);
    const SampleIdx next = s[static_cast<std::size_t>(prev - chunkFirstSample(lo))] == t ? prev : prev + 1;
    if (next >= size) {
        return {kNaN, SampleState::Invalid};
    }
    NonBlockingReader values(ts.values);
    bool timeAbsent = false;
    const double v = resolvePolicy(
        interp, t, prev, next,
        [&](SampleIdx i) {
            const std::optional<Micros> ti = times.time(i);
            timeAbsent = timeAbsent || !ti;
            return ti.value_or(t);
        },
        [&](SampleIdx i) { return values.value(i); });
    return finish(v, timeAbsent || times.absent() || values.absent());
}

// Worker-side lookups for pinFor: blocking reads through Column::require.
Result<Micros> requireTime(const Column& times, SampleIdx i, Priority prio, const CancelToken& cancel) {
    Result<ChunkHandle> h = times.require(i, i, prio, cancel);
    if (!h) {
        return Failure{h.error()};
    }
    const std::optional<std::int64_t> t = h->int64At(i);
    if (!t) {
        return fail(ErrorCode::Internal, "time column sample missing");
    }
    return *t;
}

// Last index with time <= t (t >= the first timestamp).
Result<SampleIdx> prevIndexBlocking(const Column& times, Micros t, Priority prio, const CancelToken& cancel) {
    const std::shared_ptr<const SummaryPyramid> bounds = times.summarySnapshot();
    SampleIdx lo = 0;
    SampleIdx hi = times.chunkCount() - 1;
    while (lo < hi) {
        const SampleIdx mid = (lo + hi + 1) / 2;
        Micros first = 0;
        if (const auto b = bounds->chunkTimeBounds(mid)) {
            first = b->first;
        } else {
            Result<Micros> r = requireTime(times, chunkFirstSample(mid), prio, cancel);
            if (!r) {
                return Failure{r.error()};
            }
            first = *r;
        }
        if (first <= t) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const SampleIdx c0 = chunkFirstSample(lo);
    Result<ChunkHandle> h = times.require(c0, c0, prio, cancel);
    if (!h) {
        return Failure{h.error()};
    }
    const std::span<const std::int64_t> s = h->samples<std::int64_t>(lo);
    const auto it = std::upper_bound(s.begin(), s.end(), t);
    return c0 + std::max<SampleIdx>(static_cast<SampleIdx>(it - s.begin()) - 1, 0);
}

} // namespace

// ---- ResampleView
// --------------------------------------------------------------------------------------------

double ResampleView::f64(SampleIdx k) const noexcept {
    if (k < 0 || k >= n || values_ == nullptr || inSize_ == 0) {
        return kNaN;
    }
    if (identity_) {
        const SampleIdx i = first + k + identityOffset_;
        return (i >= 0 && i < inSize_) ? values_->at(i) : kNaN;
    }
    const Micros t = out.timeAt(first + k, t0);
    if (t < tFirst_ || t > tLast_) {
        return kNaN;
    }
    auto valueOf = [this](SampleIdx i) { return values_->at(i); };
    if (uniformInput_) {
        return resolvePolicy(
            interp, t, uniformPrev(inRate_, inT0_, inSize_, t), uniformNext(inRate_, inT0_, inSize_, t),
            [this](SampleIdx i) { return inRate_.timeAt(i, inT0_); }, valueOf);
    }
    const SampleIdx prev = (*prev_)[static_cast<std::size_t>(k)];
    if (prev < 0 || prev >= inSize_) {
        return kNaN;
    }
    const std::optional<std::int64_t> tp = times_->int64At(prev);
    if (!tp) {
        return kNaN;
    }
    const SampleIdx next = *tp == t ? prev : prev + 1;
    if (next >= inSize_) {
        return kNaN;
    }
    return resolvePolicy(
        interp, t, prev, next, [this, t](SampleIdx i) { return times_->int64At(i).value_or(t); }, valueOf);
}

bool ResampleView::valid(SampleIdx k) const noexcept {
    return !std::isnan(f64(k));
}

// ---- Resampler
// -----------------------------------------------------------------------------------------------

double Resampler::valueAt(const Series& series, Micros t, Interp interp) noexcept {
    return sampleAt(series, t, interp).value;
}

ValueSample Resampler::sampleAt(const Series& series, Micros t, Interp interp) noexcept {
    try {
        if (const auto* u = std::get_if<UniformSeries>(&series)) {
            return sampleUniform(*u, t, interp);
        }
        if (const auto* ts = std::get_if<TimestampedSeries>(&series)) {
            return sampleTimestamped(*ts, t, interp);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): allocation failure while scheduling reads as "absent"
        return {kNaN, SampleState::Absent};
    }
    return {kNaN, SampleState::Invalid};
}

Result<ResampleView> Resampler::block(const Series& series, Rate out, Micros t0, SampleIdx first, SampleIdx n,
                                      Interp interp, const ChunkHandle& pinnedValues,
                                      const ChunkHandle* pinnedTimes) {
    if (!out.valid()) {
        return fail(ErrorCode::Invalid, "resample: the output rate is not valid");
    }
    if (n < 0) {
        return fail(ErrorCode::Invalid, "resample: negative block length");
    }
    ResampleView v;
    v.out = out;
    v.t0 = t0;
    v.first = first;
    v.n = n;
    v.interp = interp;
    v.values_ = &pinnedValues;

    if (const auto* u = std::get_if<UniformSeries>(&series)) {
        if (!u->rate.valid()) {
            return fail(ErrorCode::Invalid, "resample: the input rate is not valid");
        }
        v.uniformInput_ = true;
        v.inRate_ = u->rate;
        v.inT0_ = u->t0;
        v.inSize_ = u->values.size();
        if (v.inSize_ > 0) {
            v.tFirst_ = u->timeAt(0);
            v.tLast_ = u->timeAt(v.inSize_ - 1);
        }
        // Rule 14: same rate and phase is an identity gather. The phase matches when t0 is equal or, for an
        // integer period P, differs by a multiple of P. Rates above 1 MHz map several samples onto one
        // microsecond and always take the general path.
        const std::uint64_t d = 1'000'000ull * u->rate.den;
        if (u->rate == out && u->rate.num <= d) {
            if (t0 == u->t0) {
                v.identity_ = true;
            } else if (d % u->rate.num == 0) {
                const auto period = static_cast<Micros>(d / u->rate.num);
                const Micros diff = t0 - u->t0;
                if (diff % period == 0) {
                    v.identity_ = true;
                    v.identityOffset_ = diff / period;
                }
            }
        }
        if (n > 0 && v.inSize_ > 0) {
            const Micros a = std::max(out.timeAt(first, t0), v.tFirst_);
            const Micros b = std::min(out.timeAt(first + n - 1, t0), v.tLast_);
            if (a <= b && !covers(pinnedValues, uniformPrev(u->rate, u->t0, v.inSize_, a),
                                  uniformNext(u->rate, u->t0, v.inSize_, b))) {
                return fail(ErrorCode::Invalid, "resample: the pinned values do not cover the block");
            }
        }
        return v;
    }

    const auto* ts = std::get_if<TimestampedSeries>(&series);
    if (ts == nullptr) {
        return fail(ErrorCode::Invalid, "resample: empty series");
    }
    if (pinnedTimes == nullptr) {
        return fail(ErrorCode::Invalid, "resample: a timestamped series needs its pinned time column");
    }
    if (ts->timesUs.size() != ts->values.size()) {
        return fail(ErrorCode::Invalid, "resample: time and value columns differ in size");
    }
    v.uniformInput_ = false;
    v.times_ = pinnedTimes;
    v.inSize_ = ts->size();
    v.tFirst_ = ts->tMin;
    v.tLast_ = ts->tMax;
    auto prevMap = std::make_shared<std::vector<SampleIdx>>(static_cast<std::size_t>(n), SampleIdx{-1});
    if (n > 0 && v.inSize_ > 0) {
        const SampleIdx size = v.inSize_;
        auto timeOf = [&](SampleIdx i) { return pinnedTimes->int64At(i); };
        SampleIdx cursor = -1;
        SampleIdx needFirst = -1;
        SampleIdx needLast = -1;
        for (SampleIdx k = 0; k < n; ++k) {
            const Micros t = out.timeAt(first + k, t0);
            if (t < v.tFirst_) {
                (*prevMap)[static_cast<std::size_t>(k)] = -1;
                continue;
            }
            if (t > v.tLast_) {
                (*prevMap)[static_cast<std::size_t>(k)] = size;
                continue;
            }
            if (cursor < 0) {
                // One binary search inside the pinned span, then a monotone cursor (rule 14).
                SampleIdx lo = std::max<SampleIdx>(pinnedTimes->firstSample(), 0);
                SampleIdx hi = std::min(pinnedTimes->endSample(), size) - 1;
                const std::optional<Micros> tlo = lo <= hi ? timeOf(lo) : std::nullopt;
                if (!tlo || (*tlo > t && lo > 0)) {
                    return fail(ErrorCode::Invalid, "resample: the pinned times do not cover the block");
                }
                while (lo < hi) {
                    const SampleIdx mid = (lo + hi + 1) / 2;
                    const std::optional<Micros> tm = timeOf(mid);
                    if (!tm) {
                        return fail(ErrorCode::Invalid, "resample: the pinned times do not cover the block");
                    }
                    if (*tm <= t) {
                        lo = mid;
                    } else {
                        hi = mid - 1;
                    }
                }
                cursor = lo;
                needFirst = cursor;
            }
            for (;;) {
                if (cursor + 1 >= size) {
                    break;
                }
                const std::optional<Micros> tn = timeOf(cursor + 1);
                if (!tn) {
                    return fail(ErrorCode::Invalid, "resample: the pinned times do not cover the block");
                }
                if (*tn > t) {
                    break;
                }
                ++cursor;
            }
            (*prevMap)[static_cast<std::size_t>(k)] = cursor;
            const std::optional<Micros> tc = timeOf(cursor);
            needLast = (tc && *tc == t) ? cursor : std::min(cursor + 1, size - 1);
        }
        if (needFirst >= 0 &&
            (!covers(*pinnedTimes, needFirst, needLast) || !covers(pinnedValues, needFirst, needLast))) {
            return fail(ErrorCode::Invalid, "resample: the pinned input does not cover the block");
        }
    }
    v.prev_ = std::move(prevMap);
    return v;
}

Result<Resampler::PinnedInput> Resampler::pinFor(const Series& series, Rate out, Micros t0, SampleIdx first,
                                                 SampleIdx n, Priority prio, const CancelToken& cancel) {
    if (!out.valid() || n < 0) {
        return fail(ErrorCode::Invalid, "resample: bad output rate or block length");
    }
    PinnedInput pinned;
    if (n == 0) {
        return {std::move(pinned)};
    }
    const Micros ta = out.timeAt(first, t0);
    const Micros tb = out.timeAt(first + n - 1, t0);
    if (const auto* u = std::get_if<UniformSeries>(&series)) {
        const SampleIdx size = u->values.size();
        if (size == 0 || !u->rate.valid()) {
            return {std::move(pinned)};
        }
        const Micros a = std::max(ta, u->timeAt(0));
        const Micros b = std::min(tb, u->timeAt(size - 1));
        if (a > b) {
            return {std::move(pinned)};
        }
        Result<ChunkHandle> h = u->values.require(uniformPrev(u->rate, u->t0, size, a),
                                                  uniformNext(u->rate, u->t0, size, b), prio, cancel);
        if (!h) {
            return Failure{h.error()};
        }
        pinned.values = std::move(*h);
        return {std::move(pinned)};
    }
    const auto* ts = std::get_if<TimestampedSeries>(&series);
    if (ts == nullptr || ts->size() == 0) {
        return {std::move(pinned)};
    }
    const Micros a = std::max(ta, ts->tMin);
    const Micros b = std::min(tb, ts->tMax);
    if (a > b) {
        return {std::move(pinned)};
    }
    Result<SampleIdx> ia = prevIndexBlocking(ts->timesUs, a, prio, cancel);
    if (!ia) {
        return Failure{ia.error()};
    }
    Result<SampleIdx> pb = prevIndexBlocking(ts->timesUs, b, prio, cancel);
    if (!pb) {
        return Failure{pb.error()};
    }
    Result<Micros> tpb = requireTime(ts->timesUs, *pb, prio, cancel);
    if (!tpb) {
        return Failure{tpb.error()};
    }
    const SampleIdx ib = *tpb == b ? *pb : std::min(*pb + 1, ts->size() - 1);
    Result<ChunkHandle> ht = ts->timesUs.require(*ia, ib, prio, cancel);
    if (!ht) {
        return Failure{ht.error()};
    }
    Result<ChunkHandle> hv = ts->values.require(*ia, ib, prio, cancel);
    if (!hv) {
        return Failure{hv.error()};
    }
    pinned.times = std::move(*ht);
    pinned.values = std::move(*hv);
    return {std::move(pinned)};
}

Interp Resampler::defaultFor(SampleKind kind) noexcept {
    return kind == SampleKind::Real ? Interp::Linear : Interp::Previous;
}

} // namespace esses::store
