// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/plotcore/trace_builder.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/pyramid.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace esses::plot {

namespace {

using store::Chunk;
using store::ChunkHandle;
using store::kChunkSamples;
using store::MinMax;

constexpr SampleIdx kRegimeB = 2;  // n_c >= 2: raw M4 scan
constexpr SampleIdx kRegimeC = 32; // n_c >= 32: pyramid M4
constexpr int kCancelEvery = 256;
constexpr SampleIdx kSummaryBucket = store::kSummaryLeafSamples; // 256

Failure cancelled() {
    return fail(ErrorCode::Cancelled, "trace build cancelled");
}

// ---- validity scans inside one chunk
// ------------------------------------------------------------------------

std::optional<std::uint32_t> firstValid(const Chunk& c, std::uint32_t b, std::uint32_t e) noexcept {
    e = std::min(e, c.count());
    if (b >= e) {
        return std::nullopt;
    }
    switch (c.validity()) {
    case store::Validity::AllValid:
        return b;
    case store::Validity::AllInvalid:
        return std::nullopt;
    case store::Validity::Bitmap:
        break;
    }
    const std::uint64_t* bm = c.bitmap();
    for (std::uint32_t w = b >> 6; w <= (e - 1) >> 6; ++w) {
        std::uint64_t word = bm[w];
        if (w == b >> 6) {
            word &= ~std::uint64_t{0} << (b & 63U);
        }
        if (w == (e - 1) >> 6 && ((e & 63U) != 0)) {
            word &= (std::uint64_t{1} << (e & 63U)) - 1U;
        }
        if (word != 0) {
            return w * 64U + static_cast<std::uint32_t>(std::countr_zero(word));
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> lastValid(const Chunk& c, std::uint32_t b, std::uint32_t e) noexcept {
    e = std::min(e, c.count());
    if (b >= e) {
        return std::nullopt;
    }
    switch (c.validity()) {
    case store::Validity::AllValid:
        return e - 1;
    case store::Validity::AllInvalid:
        return std::nullopt;
    case store::Validity::Bitmap:
        break;
    }
    const std::uint64_t* bm = c.bitmap();
    for (std::uint32_t w = (e - 1) >> 6;; --w) {
        std::uint64_t word = bm[w];
        if (w == b >> 6) {
            word &= ~std::uint64_t{0} << (b & 63U);
        }
        if (w == (e - 1) >> 6 && ((e & 63U) != 0)) {
            word &= (std::uint64_t{1} << (e & 63U)) - 1U;
        }
        if (word != 0) {
            return w * 64U + 63U - static_cast<std::uint32_t>(std::countl_zero(word));
        }
        if (w == b >> 6) {
            break;
        }
    }
    return std::nullopt;
}

// ---- progressive pinning
// ------------------------------------------------------------------------------------

// Holds the one chunk under the current column pinned (04 section 4.5); entering a new chunk unpins the
// previous one and prefetches the next.
class PinCursor {
public:
    PinCursor(const store::Column& column, const CancelToken& cancel) : column_(column), cancel_(cancel) {}

    Result<const Chunk*> chunk(SampleIdx chunkNo) {
        if (chunkNo == current_ && chunk_ != nullptr) {
            return chunk_;
        }
        if (cancel_.checkpoint()) {
            return cancelled();
        }
        handle_.reset();
        chunk_ = nullptr;
        const SampleIdx first = store::chunkFirstSample(chunkNo);
        Result<ChunkHandle> pinned =
            column_.require(first, first + SampleIdx{kChunkSamples} - 1, Priority::Visible, cancel_);
        if (!pinned) {
            return Failure{pinned.error()};
        }
        handle_ = std::move(*pinned);
        current_ = chunkNo;
        chunk_ = handle_.chunk(chunkNo);
        if (chunk_ == nullptr) {
            return fail(ErrorCode::Internal, "Column::require did not pin the requested chunk");
        }
        if (chunkNo + 1 < column_.chunkCount()) {
            column_.prefetch(chunkNo + 1, Priority::Visible);
        }
        return chunk_;
    }

private:
    const store::Column& column_;
    const CancelToken& cancel_;
    ChunkHandle handle_;
    SampleIdx current_ = -1;
    const Chunk* chunk_ = nullptr;
};

// lower_bound over the I64 time column of a timestamped series (04 section 4.8): one binary search over
// chunks for the first call, then a monotone forward cursor.
class TimeCursor {
public:
    TimeCursor(const store::Column& times, const CancelToken& cancel) : times_(times), cancel_(cancel) {}

    // First index whose time is >= t. Calls must pass non-decreasing t.
    Result<SampleIdx> lowerBound(Micros t) {
        const SampleIdx size = times_.size();
        if (size <= 0) {
            return SampleIdx{0};
        }
        if (pos_ < 0) {
            Result<SampleIdx> start = seekChunk(t);
            if (!start) {
                return start;
            }
            pos_ = store::chunkFirstSample(*start);
        }
        while (pos_ < size) {
            const SampleIdx k = store::chunkOf(pos_);
            Result<std::span<const std::int64_t>> s = samples(k);
            if (!s) {
                return Failure{s.error()};
            }
            const SampleIdx base = store::chunkFirstSample(k);
            const auto local = static_cast<std::size_t>(pos_ - base);
            if (s->empty() || s->back() < t) {
                pos_ = base + static_cast<SampleIdx>(s->size());
                continue;
            }
            const auto it = std::lower_bound(s->begin() + static_cast<std::ptrdiff_t>(local), s->end(), t);
            pos_ = base + static_cast<SampleIdx>(it - s->begin());
            return pos_;
        }
        return size;
    }

    Result<Micros> timeOf(SampleIdx i) {
        Result<std::span<const std::int64_t>> s = samples(store::chunkOf(i));
        if (!s) {
            return Failure{s.error()};
        }
        const auto local = static_cast<std::size_t>(i - store::chunkFirstSample(store::chunkOf(i)));
        if (local >= s->size()) {
            return fail(ErrorCode::Internal, "timestamp index outside its chunk");
        }
        return Micros{(*s)[local]};
    }

private:
    Result<std::span<const std::int64_t>> samples(SampleIdx k) {
        if (k != current_ || handle_.empty()) {
            if (cancel_.checkpoint()) {
                return cancelled();
            }
            handle_.reset();
            const SampleIdx first = store::chunkFirstSample(k);
            Result<ChunkHandle> pinned =
                times_.require(first, first + SampleIdx{kChunkSamples} - 1, Priority::Visible, cancel_);
            if (!pinned) {
                return Failure{pinned.error()};
            }
            handle_ = std::move(*pinned);
            current_ = k;
        }
        return handle_.samples<std::int64_t>(k);
    }

    Result<Micros> lastTimeOf(SampleIdx k) {
        if (auto bounds = times_.summary().chunkTimeBounds(k)) {
            return Micros{bounds->last};
        }
        Result<std::span<const std::int64_t>> s = samples(k);
        if (!s) {
            return Failure{s.error()};
        }
        return s->empty() ? kMicrosMin : Micros{s->back()};
    }

    // The first chunk whose last time is >= t (the last chunk when none is).
    Result<SampleIdx> seekChunk(Micros t) {
        SampleIdx lo = 0;
        SampleIdx hi = times_.chunkCount() - 1;
        while (lo < hi) {
            const SampleIdx mid = lo + (hi - lo) / 2;
            Result<Micros> last = lastTimeOf(mid);
            if (!last) {
                return Failure{last.error()};
            }
            if (*last < t) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    const store::Column& times_;
    const CancelToken& cancel_;
    ChunkHandle handle_;
    SampleIdx current_ = -1;
    SampleIdx pos_ = -1;
};

// ---- column boundaries
// --------------------------------------------------------------------------------------

// The smallest uniform-series index whose time is >= t, clamped to [0, size].
SampleIdx uniformLowerBound(Rate rate, Micros t0, Micros t, SampleIdx size) noexcept {
    if (t <= kMicrosMin + 1) {
        return 0;
    }
    const SampleIdx j = rate.indexAt(t - 1, t0); // the largest index with timeAt <= t - 1
    if (j >= size) {
        return size;
    }
    return std::clamp<SampleIdx>(j + 1, 0, size);
}

std::vector<Micros> columnTimes(const DomainMapper& map, XSpan visible, int widthPx) {
    std::vector<Micros> t(static_cast<std::size_t>(widthPx) + 1);
    const double len = visible.length();
    for (int c = 0; c <= widthPx; ++c) {
        const double x = visible.a + static_cast<double>(c) * len / static_cast<double>(widthPx);
        Micros tc = map.timeAt(x);
        if (c > 0) {
            tc = std::max(tc, t[static_cast<std::size_t>(c) - 1]);
        }
        t[static_cast<std::size_t>(c)] = tc;
    }
    return t;
}

struct Interval {
    SampleIdx begin = 0;
    SampleIdx end = 0;
};

struct Segment {
    SampleIdx begin = 0;
    SampleIdx end = 0;
    bool excluded = false;
};

// The column's raw span split by the exclusion intervals (sorted, disjoint); `cursor` advances monotonically.
void splitColumn(SampleIdx i0, SampleIdx i1, const std::vector<Interval>& excl, std::size_t& cursor,
                 std::vector<Segment>& out) {
    out.clear();
    while (cursor < excl.size() && excl[cursor].end <= i0) {
        ++cursor;
    }
    SampleIdx pos = i0;
    for (std::size_t k = cursor; k < excl.size() && excl[k].begin < i1; ++k) {
        const SampleIdx eb = std::max(excl[k].begin, pos);
        const SampleIdx ee = std::min(excl[k].end, i1);
        if (eb > pos) {
            out.push_back(Segment{pos, eb, false});
        }
        if (ee > eb) {
            out.push_back(Segment{eb, ee, true});
        }
        pos = std::max(pos, ee);
    }
    if (pos < i1) {
        out.push_back(Segment{pos, i1, false});
    }
}

// ---- per-column fold
// ----------------------------------------------------------------------------------------

struct ColumnFold {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    double first = 0.0;
    double last = 0.0;
    SampleIdx firstIdx = -1;
    bool any = false;
    bool hasInvalid = false;
    bool breakBefore = false;
    bool invalidSinceLast = false; // regime B: an invalid sample after the latest valid one
};

void foldValue(ColumnFold& f, double v) noexcept {
    f.lo = store::foldMin(f.lo, v);
    f.hi = store::foldMax(f.hi, v);
}

class Decimator {
public:
    Decimator(const TraceSpec& spec, const DomainMapper& map, XSpan visible, int widthPx,
              std::span<M4Column> out, const CancelToken& cancel)
        : spec_(spec), map_(map), visible_(visible), widthPx_(widthPx), out_(out), cancel_(cancel),
          values_(spec.column, cancel), times_(spec.timesUs, cancel) {}

    Result<double> run() {
        for (int c = 0; c < widthPx_; ++c) {
            out_[static_cast<std::size_t>(c)] = M4Column{0.0F, 0.0F, 0.0F, 0.0F, M4Flag::Empty, 0.0F};
        }
        const SampleIdx size = spec_.column.size();
        if (widthPx_ <= 0 || size <= 0 || !(visible_.length() > 0.0)) {
            return 0.0;
        }
        if (map_.domain() == Domain::Distance && !map_.hasDistance()) {
            return 0.0;
        }
        timestamped_ = spec_.timesUs.valid();
        if (!timestamped_ && !spec_.rate.valid()) {
            return fail(ErrorCode::Invalid, "trace spec without a rate or a time column");
        }
        if (timestamped_ && spec_.timesUs.size() != size) {
            return fail(ErrorCode::Invalid, "timestamped trace with mismatched time and value columns");
        }
        if (Result<void> b = computeBounds(); !b) {
            return Failure{b.error()};
        }
        if (Result<void> e = computeExclusions(); !e) {
            return Failure{e.error()};
        }
        std::size_t exclCursor = 0;
        std::vector<Segment> segments;
        for (int c = 0; c < widthPx_; ++c) {
            if (c % kCancelEvery == 0 && cancel_.checkpoint()) {
                return cancelled();
            }
            const SampleIdx i0 = bounds_[static_cast<std::size_t>(c)];
            const SampleIdx i1 = bounds_[static_cast<std::size_t>(c) + 1];
            if (i1 <= i0) {
                continue; // no samples: a joinable Empty column
            }
            splitColumn(i0, i1, exclusions_, exclCursor, segments);
            Result<void> r = foldColumn(c, segments);
            if (!r) {
                return Failure{r.error()};
            }
        }
        return origin_;
    }

private:
    Result<void> computeBounds() {
        const std::vector<Micros> t = columnTimes(map_, visible_, widthPx_);
        bounds_.resize(t.size());
        const SampleIdx size = spec_.column.size();
        for (std::size_t c = 0; c < t.size(); ++c) {
            if (timestamped_) {
                Result<SampleIdx> b = times_.lowerBound(t[c]);
                if (!b) {
                    return Failure{b.error()};
                }
                bounds_[c] = std::clamp<SampleIdx>(*b, 0, size);
            } else {
                bounds_[c] = uniformLowerBound(spec_.rate, spec_.t0, t[c], size);
            }
        }
        return {};
    }

    Result<void> computeExclusions() {
        exclusions_.clear();
        if (map_.domain() != Domain::Distance || !map_.chain()) {
            return {};
        }
        const auto spans = map_.chain()->unindexedSpans();
        if (spans.empty()) {
            return {};
        }
        const SampleIdx size = spec_.column.size();
        TimeCursor cursor(spec_.timesUs, cancel_);
        for (const auto& [ts, te] : spans) {
            SampleIdx b = 0;
            SampleIdx e = 0;
            if (timestamped_) {
                Result<SampleIdx> rb = cursor.lowerBound(ts);
                if (!rb) {
                    return Failure{rb.error()};
                }
                Result<SampleIdx> re = cursor.lowerBound(te);
                if (!re) {
                    return Failure{re.error()};
                }
                b = *rb;
                e = *re;
            } else {
                b = uniformLowerBound(spec_.rate, spec_.t0, ts, size);
                e = uniformLowerBound(spec_.rate, spec_.t0, te, size);
            }
            if (e > b) {
                exclusions_.push_back(Interval{b, e});
            }
        }
        return {};
    }

    Result<double> xFracOf(int c, SampleIdx i) {
        Micros t = 0;
        if (timestamped_) {
            Result<Micros> ti = times_.timeOf(i);
            if (!ti) {
                return Failure{ti.error()};
            }
            t = *ti;
        } else {
            t = spec_.rate.timeAt(i, spec_.t0);
        }
        const double px = DomainMapper::pxOf(map_.xOf(t), visible_, 0.0, static_cast<double>(widthPx_));
        const double frac = px - static_cast<double>(c);
        if (!(frac > 0.0)) {
            return 0.0;
        }
        return frac < 1.0 ? frac : static_cast<double>(std::nextafter(1.0F, 0.0F));
    }

    Result<void> foldColumn(int c, const std::vector<Segment>& segments) {
        SampleIdx included = 0;
        bool anyExcluded = false;
        for (const Segment& s : segments) {
            if (s.excluded) {
                anyExcluded = true;
            } else {
                included += s.end - s.begin;
            }
        }
        M4Column& col = out_[static_cast<std::size_t>(c)];
        if (included == 0) {
            col.flags = M4Flag::Empty | M4Flag::HasInvalid; // excluded samples only: break the line
            return {};
        }
        ColumnFold f;
        if (included < kRegimeB) {
            // Regime A: the single included sample. Excluded samples (a collapsed lap without an index)
            // before or after it break the line on that side, as invalid samples do in regime B.
            SampleIdx idx = -1;
            bool excludedBefore = false;
            bool excludedAfter = false;
            for (const Segment& s : segments) {
                if (s.excluded) {
                    (idx < 0 ? excludedBefore : excludedAfter) = true;
                } else if (idx < 0) {
                    idx = s.begin;
                }
            }
            Result<const Chunk*> chunk = values_.chunk(store::chunkOf(idx));
            if (!chunk) {
                return Failure{chunk.error()};
            }
            const std::uint32_t local = store::chunkLocal(idx);
            if (!(*chunk)->validAt(local)) {
                col.flags = M4Flag::Empty | M4Flag::HasInvalid | M4Flag::RawRegime;
                return {};
            }
            const double v = (*chunk)->valueAt(local);
            Result<double> frac = xFracOf(c, idx);
            if (!frac) {
                return Failure{frac.error()};
            }
            f.any = true;
            f.first = f.last = f.lo = f.hi = v;
            f.hasInvalid = anyExcluded;
            std::uint8_t extra = M4Flag::RawRegime;
            if (excludedBefore) {
                extra |= M4Flag::BreakBefore;
            }
            if (excludedAfter) {
                extra |= M4Flag::BreakAfter;
            }
            write(col, f, extra);
            col.xFrac = static_cast<float>(*frac);
            return {};
        }
        if (included < kRegimeC) {
            Result<void> r = foldRaw(segments, f);
            if (!r) {
                return r;
            }
            std::uint8_t extra = 0;
            if (f.any && f.breakBefore) {
                extra |= M4Flag::BreakBefore;
            }
            if (f.any && f.invalidSinceLast) {
                extra |= M4Flag::BreakAfter;
            }
            write(col, f, extra);
            return {};
        }
        Result<void> r = foldPyramid(segments, store::descentLevel(included, 8), f);
        if (!r) {
            return r;
        }
        write(col, f, 0);
        return {};
    }

    // Regime B: every included sample is read.
    Result<void> foldRaw(const std::vector<Segment>& segments, ColumnFold& f) {
        for (const Segment& s : segments) {
            if (s.excluded) {
                f.hasInvalid = true;
                f.invalidSinceLast = true;
                if (!f.any) {
                    f.breakBefore = true;
                }
                continue;
            }
            for (SampleIdx i = s.begin; i < s.end;) {
                const SampleIdx k = store::chunkOf(i);
                Result<const Chunk*> chunk = values_.chunk(k);
                if (!chunk) {
                    return Failure{chunk.error()};
                }
                const SampleIdx chunkEnd =
                    std::min(s.end, store::chunkFirstSample(k) + SampleIdx{kChunkSamples});
                for (; i < chunkEnd; ++i) {
                    const std::uint32_t local = store::chunkLocal(i);
                    if (!(*chunk)->validAt(local)) {
                        f.hasInvalid = true;
                        f.invalidSinceLast = true;
                        if (!f.any) {
                            f.breakBefore = true;
                        }
                        continue;
                    }
                    const double v = (*chunk)->valueAt(local);
                    if (!f.any) {
                        f.any = true;
                        f.first = v;
                        f.firstIdx = i;
                    }
                    f.last = v;
                    f.invalidSinceLast = false;
                    foldValue(f, v);
                }
            }
        }
        return {};
    }

    // Regime C: pyramid descent per chunk part; first/last valid found with bitmap scans.
    Result<void> foldPyramid(const std::vector<Segment>& segments, int level, ColumnFold& f) {
        MinMax mm;
        for (const Segment& s : segments) {
            if (s.excluded) {
                f.hasInvalid = true;
                continue;
            }
            for (SampleIdx i = s.begin; i < s.end;) {
                const SampleIdx k = store::chunkOf(i);
                Result<const Chunk*> chunk = values_.chunk(k);
                if (!chunk) {
                    return Failure{chunk.error()};
                }
                const Chunk& ch = **chunk;
                const SampleIdx base = store::chunkFirstSample(k);
                const SampleIdx partEnd = std::min(s.end, base + SampleIdx{kChunkSamples});
                const auto b = static_cast<std::uint32_t>(i - base);
                const auto e = static_cast<std::uint32_t>(partEnd - base);
                mm.fold(store::chunkRangeMinMax(ch, b, e, level));
                if (ch.anyInvalid(b, e)) {
                    f.hasInvalid = true;
                }
                if (!f.any) {
                    if (const auto fv = firstValid(ch, b, e)) {
                        f.any = true;
                        f.first = ch.valueAt(*fv);
                        f.firstIdx = base + *fv;
                    }
                }
                if (const auto lv = lastValid(ch, b, e)) {
                    f.last = ch.valueAt(*lv);
                }
                i = partEnd;
            }
        }
        if (f.any && !mm.isEmpty()) {
            f.lo = static_cast<double>(mm.lo);
            f.hi = static_cast<double>(mm.hi);
        } else {
            f.any = false;
        }
        return {};
    }

    void write(M4Column& col, const ColumnFold& f, std::uint8_t extraFlags) {
        std::uint8_t flags = extraFlags;
        if (f.hasInvalid) {
            flags |= M4Flag::HasInvalid;
        }
        if (!f.any) {
            col.flags = static_cast<std::uint8_t>((flags & ~(M4Flag::BreakBefore | M4Flag::BreakAfter)) |
                                                  M4Flag::Empty | M4Flag::HasInvalid);
            return;
        }
        const units::Converter& conv = spec_.toDisplay;
        const double first = conv(f.first);
        const double last = conv(f.last);
        double lo = conv(f.lo);
        double hi = conv(f.hi);
        if (lo > hi) {
            std::swap(lo, hi); // a negative scale
        }
        if (!haveOrigin_) {
            haveOrigin_ = true;
            origin_ = first;
        }
        col.first = static_cast<float>(first - origin_);
        col.last = static_cast<float>(last - origin_);
        col.lo = static_cast<float>(lo - origin_);
        col.hi = static_cast<float>(hi - origin_);
        col.flags = flags;
        col.xFrac = 0.0F;
    }

    const TraceSpec& spec_;
    const DomainMapper& map_;
    XSpan visible_;
    int widthPx_;
    std::span<M4Column> out_;
    const CancelToken& cancel_;
    PinCursor values_;
    TimeCursor times_;
    bool timestamped_ = false;
    std::vector<SampleIdx> bounds_;
    std::vector<Interval> exclusions_;
    bool haveOrigin_ = false;
    double origin_ = 0.0;
};

// ---- summary path
// -------------------------------------------------------------------------------------------

class SummaryDecimator {
public:
    SummaryDecimator(const TraceSpec& spec, const DomainMapper& map, XSpan visible, int widthPx,
                     std::span<M4Column> out, const CancelToken& cancel)
        : spec_(spec), map_(map), visible_(visible), widthPx_(widthPx), out_(out), cancel_(cancel) {}

    Result<double> run() {
        for (int c = 0; c < widthPx_; ++c) {
            out_[static_cast<std::size_t>(c)] = M4Column{0.0F, 0.0F, 0.0F, 0.0F, M4Flag::Empty, 0.0F};
        }
        const SampleIdx size = spec_.column.size();
        if (widthPx_ <= 0 || size <= 0 || !(visible_.length() > 0.0)) {
            return 0.0;
        }
        if (map_.domain() == Domain::Distance && !map_.hasDistance()) {
            return 0.0;
        }
        summary_ = spec_.column.summarySnapshot();
        if (!summary_) {
            return 0.0;
        }
        computeBounds();
        for (int c = 0; c < widthPx_; ++c) {
            if (c % kCancelEvery == 0 && cancel_.checkpoint()) {
                return cancelled();
            }
            const SampleIdx i0 = bounds_[static_cast<std::size_t>(c)];
            const SampleIdx i1 = bounds_[static_cast<std::size_t>(c) + 1];
            if (i1 <= i0) {
                continue;
            }
            foldColumn(c, i0, i1);
        }
        return origin_;
    }

private:
    // Uniform bounds are exact; timestamped bounds use resident time chunks, else interpolate inside the
    // chunk's time bounds (Approx).
    void computeBounds() {
        const std::vector<Micros> t = columnTimes(map_, visible_, widthPx_);
        bounds_.resize(t.size());
        approxBound_.assign(t.size(), false);
        const SampleIdx size = spec_.column.size();
        const bool timestamped = spec_.timesUs.valid();
        for (std::size_t c = 0; c < t.size(); ++c) {
            if (!timestamped) {
                bounds_[c] = uniformLowerBound(spec_.rate, spec_.t0, t[c], size);
                continue;
            }
            bool approx = false;
            bounds_[c] = std::clamp<SampleIdx>(timestampedBound(t[c], approx), 0, size);
            approxBound_[c] = approx;
        }
    }

    SampleIdx timestampedBound(Micros t, bool& approx) {
        const store::SummaryPyramid& ts = spec_.timesUs.summary();
        const SampleIdx chunks = spec_.timesUs.chunkCount();
        // The first chunk whose last time is >= t, among filled chunks (an unfilled chunk is skipped as if
        // its times were unknown: Approx).
        for (SampleIdx k = 0; k < chunks; ++k) {
            const auto bounds = ts.chunkTimeBounds(k);
            if (!bounds) {
                approx = true;
                continue;
            }
            if (bounds->last < t) {
                continue;
            }
            const SampleIdx base = store::chunkFirstSample(k);
            if (bounds->first >= t) {
                return base;
            }
            if (auto pin = spec_.timesUs.tryPin(k, Priority::Visible)) {
                const auto s = pin->samples<std::int64_t>(k);
                const auto it = std::lower_bound(s.begin(), s.end(), t);
                return base + static_cast<SampleIdx>(it - s.begin());
            }
            approx = true;
            const auto count = static_cast<double>(store::chunkSampleCount(spec_.timesUs.size(), k));
            const double frac = static_cast<double>(t - bounds->first) /
                                static_cast<double>(std::max<std::int64_t>(1, bounds->last - bounds->first));
            return base + static_cast<SampleIdx>(std::ceil(frac * (count - 1.0)));
        }
        return spec_.timesUs.size();
    }

    const Chunk* resident(SampleIdx chunkNo) {
        if (chunkNo == pinnedChunk_ && pin_) {
            return pin_->chunk(chunkNo);
        }
        pin_.reset();
        pinnedChunk_ = chunkNo;
        pin_ = spec_.column.tryPin(chunkNo, Priority::Visible);
        return pin_ ? pin_->chunk(chunkNo) : nullptr;
    }

    // [b, e) inside one 256-sample bucket edge (or one chunk): exact through a resident chunk, else the
    // straddling buckets folded whole (Approx). `wantFirst`/`wantLast` read the edge values.
    void foldEdge(SampleIdx b, SampleIdx e, MinMax& mm, std::optional<double>& first,
                  std::optional<double>& last, bool& approx, bool& hasInvalid) {
        if (e <= b) {
            return;
        }
        for (SampleIdx i = b; i < e;) {
            const SampleIdx k = store::chunkOf(i);
            const SampleIdx base = store::chunkFirstSample(k);
            const SampleIdx partEnd = std::min(e, base + SampleIdx{kChunkSamples});
            const Chunk* ch = resident(k);
            if (ch != nullptr) {
                const auto lb = static_cast<std::uint32_t>(i - base);
                const auto le = static_cast<std::uint32_t>(partEnd - base);
                mm.fold(store::chunkRangeMinMax(*ch, lb, le));
                hasInvalid = hasInvalid || ch->anyInvalid(lb, le);
                if (!first) {
                    if (const auto fv = firstValid(*ch, lb, le)) {
                        first = ch->valueAt(*fv);
                    }
                }
                if (const auto lv = lastValid(*ch, lb, le)) {
                    last = ch->valueAt(*lv);
                }
            } else {
                approx = true;
                const SampleIdx firstBucket = i / kSummaryBucket;
                const SampleIdx lastBucket = (partEnd - 1) / kSummaryBucket;
                MinMax part;
                for (SampleIdx q = firstBucket; q <= lastBucket; ++q) {
                    if (auto bucket = summary_->bucket(store::kSummaryFirstLevel, q)) {
                        part.fold(*bucket);
                    }
                }
                mm.fold(part);
                if (!part.isEmpty()) {
                    const double mid = 0.5 * (static_cast<double>(part.lo) + static_cast<double>(part.hi));
                    if (!first) {
                        first = mid;
                    }
                    last = mid;
                }
            }
            i = partEnd;
        }
    }

    // The first or last value of the aligned middle [b, e): a raw read when the chunk is resident, else the
    // bucket midpoint (Approx).
    std::optional<double> probe(SampleIdx b, SampleIdx e, bool wantLast, bool& approx) {
        MinMax scratch;
        std::optional<double> first;
        std::optional<double> last;
        bool unused = false;
        foldEdge(b, e, scratch, first, last, approx, unused);
        return wantLast ? last : first;
    }

    void foldColumn(int c, SampleIdx i0, SampleIdx i1) {
        MinMax mm;
        std::optional<double> first;
        std::optional<double> last;
        bool approx =
            approxBound_[static_cast<std::size_t>(c)] || approxBound_[static_cast<std::size_t>(c) + 1];
        bool hasInvalid = false;
        const SampleIdx a = (i0 + kSummaryBucket - 1) / kSummaryBucket * kSummaryBucket; // first aligned
        const SampleIdx b = i1 / kSummaryBucket * kSummaryBucket;                        // last aligned
        if (a >= b) {
            foldEdge(i0, i1, mm, first, last, approx, hasInvalid);
        } else {
            std::optional<double> leftFirst;
            std::optional<double> leftLast;
            foldEdge(i0, a, mm, leftFirst, leftLast, approx, hasInvalid);
            std::optional<double> rightFirst;
            std::optional<double> rightLast;
            foldEdge(b, i1, mm, rightFirst, rightLast, approx, hasInvalid);
            bool covered = true;
            const MinMax mid = summary_->minmax(a, b - 1, &covered);
            std::optional<double> midFirst;
            std::optional<double> midLast;
            if (covered) {
                mm.fold(mid);
                if (!leftFirst) {
                    midFirst = probe(a, std::min(b, a + kSummaryBucket), false, approx);
                }
                if (!rightLast) {
                    midLast = probe(std::max(a, b - kSummaryBucket), b, true, approx);
                }
            } else {
                // An unfilled summary chunk: exact through resident chunks, else the filled buckets (Approx).
                foldEdge(a, b, mm, midFirst, midLast, approx, hasInvalid);
            }
            first = leftFirst ? leftFirst : (midFirst ? midFirst : rightFirst);
            last = rightLast ? rightLast : (midLast ? midLast : leftLast);
        }
        if (!mm.isEmpty() && (!first || !last)) {
            // Valid samples exist but none near the probed edges (a long invalid run): the column midpoint.
            approx = true;
            const double mid = 0.5 * (static_cast<double>(mm.lo) + static_cast<double>(mm.hi));
            first = first ? first : mid;
            last = last ? last : mid;
        }
        M4Column& col = out_[static_cast<std::size_t>(c)];
        std::uint8_t flags = 0;
        if (approx) {
            flags |= M4Flag::Approx;
        }
        if (hasInvalid) {
            flags |= M4Flag::HasInvalid;
        }
        if (mm.isEmpty() || !first || !last) {
            col.flags = static_cast<std::uint8_t>(flags | M4Flag::Empty);
            return;
        }
        const units::Converter& conv = spec_.toDisplay;
        double lo = conv(static_cast<double>(mm.lo));
        double hi = conv(static_cast<double>(mm.hi));
        if (lo > hi) {
            std::swap(lo, hi);
        }
        const double f = conv(*first);
        const double l = conv(*last);
        if (!haveOrigin_) {
            haveOrigin_ = true;
            origin_ = f;
        }
        col.first = static_cast<float>(f - origin_);
        col.last = static_cast<float>(l - origin_);
        col.lo = static_cast<float>(lo - origin_);
        col.hi = static_cast<float>(hi - origin_);
        col.flags = flags;
    }

    const TraceSpec& spec_;
    const DomainMapper& map_;
    XSpan visible_;
    int widthPx_;
    std::span<M4Column> out_;
    const CancelToken& cancel_;
    std::shared_ptr<const store::SummaryPyramid> summary_;
    std::vector<SampleIdx> bounds_;
    std::vector<bool> approxBound_;
    std::optional<ChunkHandle> pin_;
    SampleIdx pinnedChunk_ = -1;
    bool haveOrigin_ = false;
    double origin_ = 0.0;
};

Result<DecimatedTrace> runJob(const TraceJob& job, XSpan visible, int widthPx, const CancelToken& cancel) {
    DecimatedTrace trace;
    trace.columns.resize(static_cast<std::size_t>(std::max(widthPx, 0)));
    Result<double> origin =
        TraceBuilder::decimate(job.spec, job.map, visible, widthPx, trace.columns, cancel);
    if (!origin) {
        return Failure{origin.error()};
    }
    trace.origin = *origin;
    return trace;
}

} // namespace

float YMap::yOf(double v) const noexcept {
    if (vMax == vMin) {
        return yBottomPx;
    }
    const double f = (v - vMin) / (vMax - vMin);
    return static_cast<float>(static_cast<double>(yBottomPx) +
                              f * (static_cast<double>(yTopPx) - static_cast<double>(yBottomPx)));
}

Result<double> TraceBuilder::decimate(const TraceSpec& spec, const DomainMapper& map, XSpan visible,
                                      int widthPx, std::span<M4Column> out, const CancelToken& cancel) {
    if (widthPx < 0 || out.size() < static_cast<std::size_t>(widthPx)) {
        return fail(ErrorCode::Invalid, "decimate: the output span is smaller than widthPx");
    }
    if (!spec.column.valid()) {
        for (int c = 0; c < widthPx; ++c) {
            out[static_cast<std::size_t>(c)] = M4Column{0.0F, 0.0F, 0.0F, 0.0F, M4Flag::Empty, 0.0F};
        }
        return 0.0;
    }
    Decimator d(spec, map, visible, widthPx, out, cancel);
    return d.run();
}

Result<double> TraceBuilder::decimateSummary(const TraceSpec& spec, const DomainMapper& map, XSpan visible,
                                             int widthPx, std::span<M4Column> out,
                                             const CancelToken& cancel) {
    if (widthPx < 0 || out.size() < static_cast<std::size_t>(widthPx)) {
        return fail(ErrorCode::Invalid, "decimateSummary: the output span is smaller than widthPx");
    }
    if (!spec.column.valid()) {
        for (int c = 0; c < widthPx; ++c) {
            out[static_cast<std::size_t>(c)] = M4Column{0.0F, 0.0F, 0.0F, 0.0F, M4Flag::Empty, 0.0F};
        }
        return 0.0;
    }
    SummaryDecimator d(spec, map, visible, widthPx, out, cancel);
    return d.run();
}

std::vector<Result<DecimatedTrace>> TraceBuilder::decimateMany(std::span<const TraceJob> jobs, XSpan visible,
                                                               int widthPx, const CancelToken& cancel,
                                                               TaskPool* pool) {
    std::vector<Result<DecimatedTrace>> results;
    results.reserve(jobs.size());
    if (pool == nullptr || jobs.size() < 2) {
        for (const TraceJob& job : jobs) {
            results.push_back(runJob(job, visible, widthPx, cancel));
        }
        return results;
    }
    std::vector<Future<DecimatedTrace>> futures;
    futures.reserve(jobs.size() - 1);
    for (std::size_t i = 1; i < jobs.size(); ++i) {
        futures.push_back(pool->submit(Priority::Visible, TaskKey{}, cancel,
                                       [job = jobs[i], visible, widthPx](const CancelToken& token) {
                                           return runJob(job, visible, widthPx, token);
                                       }));
    }
    results.push_back(runJob(jobs.front(), visible, widthPx, cancel));
    for (Future<DecimatedTrace>& f : futures) {
        results.push_back(std::move(f).take());
    }
    return results;
}

void TraceBuilder::emit(DrawList& list, std::span<const M4Column> columns, double origin,
                        const TraceSpec& spec, const YMap& yMap, float xOriginPx, float dpr) {
    const float w = std::max(spec.widthLogicalPx * dpr, 0.0F);
    const auto yA = static_cast<double>(yMap.yTopPx);
    const auto yB = static_cast<double>(yMap.yBottomPx);
    const double clampLo = std::min(yA, yB) - 2.0 * static_cast<double>(w);
    const double clampHi = std::max(yA, yB) + 2.0 * static_cast<double>(w);
    const auto yOf = [&](float v) {
        const auto y = static_cast<double>(yMap.yOf(origin + static_cast<double>(v)));
        return static_cast<float>(std::clamp(y, clampLo, clampHi));
    };
    const bool pointsStyle = spec.style == TraceStyle::Points;
    const bool step = spec.interp == store::Interp::Previous;

    std::size_t rawColumns = 0;
    bool allRaw = true;
    for (const M4Column& col : columns) {
        if ((col.flags & M4Flag::Empty) != 0) {
            continue;
        }
        if ((col.flags & M4Flag::RawRegime) != 0) {
            ++rawColumns;
        } else {
            allRaw = false;
        }
    }
    // Point markers when zoomed in below 0.25 samples per pixel (04 section 4.4 regime A).
    const bool markers = !pointsStyle && allRaw && rawColumns > 0 && rawColumns * 4 < columns.size();
    const float pointSize = (2.0F + spec.widthLogicalPx) * dpr;

    std::vector<float> line;
    std::vector<float> pts;
    std::vector<float> segs;
    line.reserve(columns.size() * 8);
    const auto flush = [&] {
        if (line.size() >= 4) {
            list.polyline(line.data(), line.size() / 2, spec.colour, w, spec.dash);
        } else if (line.size() == 2) {
            pts.push_back(line[0]); // an isolated sample between gaps stays visible as a dot
            pts.push_back(line[1]);
        }
        line.clear();
    };
    const auto push = [&](float x, float y) {
        if (step && !line.empty() && line[line.size() - 2] < x) {
            const float prevY = line.back();
            line.push_back(x);
            line.push_back(prevY);
        }
        line.push_back(x);
        line.push_back(y);
    };

    for (std::size_t c = 0; c < columns.size(); ++c) {
        const M4Column& col = columns[c];
        const float xc = xOriginPx + static_cast<float>(c);
        if ((col.flags & M4Flag::Empty) != 0) {
            if ((col.flags & M4Flag::HasInvalid) != 0) {
                flush();
            }
            continue;
        }
        if (pointsStyle) {
            if ((col.flags & M4Flag::RawRegime) != 0) {
                pts.push_back(xc + col.xFrac);
                pts.push_back(yOf(col.first));
            } else {
                segs.push_back(xc + 0.5F);
                segs.push_back(yOf(col.lo));
                segs.push_back(xc + 0.5F);
                segs.push_back(yOf(col.hi));
            }
            continue;
        }
        if ((col.flags & M4Flag::RawRegime) != 0) {
            const float x = xc + col.xFrac;
            const float y = yOf(col.first);
            if ((col.flags & M4Flag::BreakBefore) != 0) {
                flush();
            }
            push(x, y);
            if (markers) {
                pts.push_back(x);
                pts.push_back(y);
            }
            if ((col.flags & M4Flag::BreakAfter) != 0) {
                flush();
            }
            continue;
        }
        if ((col.flags & M4Flag::BreakBefore) != 0) {
            flush();
        }
        push(xc, yOf(col.first));
        if (col.first <= col.last) {
            line.insert(line.end(), {xc + 0.5F, yOf(col.lo), xc + 0.5F, yOf(col.hi)});
        } else {
            line.insert(line.end(), {xc + 0.5F, yOf(col.hi), xc + 0.5F, yOf(col.lo)});
        }
        line.insert(line.end(), {xc + 1.0F, yOf(col.last)});
        if ((col.flags & M4Flag::BreakAfter) != 0) {
            flush();
        }
    }
    flush();
    if (!segs.empty()) {
        list.lines(segs.data(), segs.size() / 2, spec.colour, std::max(w, 1.0F));
    }
    if (!pts.empty()) {
        list.points(pts.data(), pts.size() / 2, spec.colour, pointSize);
    }
}

} // namespace esses::plot
