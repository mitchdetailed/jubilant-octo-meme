// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportEngine (04 section 12, AC-M3-03). Owner: the reports work (src/reports/).
//
// One pass over a (row, channel) interval feeds every statistic: a uniform series is read chunk by chunk
// straight from the pinned chunk's samples and validity bitmap (no per-sample handle lookup), a timestamped
// one through forEachSample. The mean and standard deviation use sums of (v - shift), shift = the first valid
// sample, which keeps the cancellation of the one-pass variance small without a division per sample.

#include "../series_access.hpp"
#include "report_common.hpp"

#include <esses/analysis/report_engine.hpp>
#include <esses/base/hash.hpp>
#include <esses/model/detail.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <variant>

namespace esses::analysis {

namespace {

constexpr Priority kPrio = Priority::Background;
// Chunks pinned at once by a scan: a lap is one or two chunks, a whole outing never stays pinned at once.
constexpr SampleIdx kScanChunks = 8;
// Chunks prefetched ahead of the scan (about 64 MB of F32 samples): enough to keep a pool's workers busy for
// a report of short ranges, never the whole of a long log, so prefetched chunks are not evicted unread.
constexpr SampleIdx kPrefetchChunks = 256;

// ---- one pass over an interval ---------------------------------------------------------------------------

// Positions are sample indices for a uniform series and times for a timestamped one; scanSeries returns
// times.
struct Scan {
    std::int64_t samples = 0; // every sample in the interval
    std::int64_t valid = 0;
    std::int64_t firstSample = 0;
    std::int64_t lastSample = 0;
    std::int64_t firstValidPos = 0;
    std::int64_t lastValidPos = 0;
    double firstValid = 0.0;
    double lastValid = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::int64_t minPos = 0;
    std::int64_t maxPos = 0;
    double absMax = 0.0;
    std::int64_t absMaxPos = 0;
    double shift = 0.0;
    double sum = 0.0; // of (v - shift)
    double sumSq = 0.0;
    // A uniform series covers the interval's bounds when it has a sample within one sample period of them;
    // a series that starts (ends) inside the interval has no "first (last) sample of the range" and its start
    // (end) needs the probe (D-197 (a)). A timestamped series always counts as covering them.
    bool coversStart = true;
    bool coversEnd = true;

    void addSample(std::int64_t pos) noexcept {
        if (samples == 0) {
            firstSample = pos;
        }
        lastSample = pos;
        ++samples;
    }
    void addValid(std::int64_t pos, double v) noexcept {
        if (valid == 0) {
            shift = v;
            firstValidPos = pos;
            firstValid = v;
            min = v;
            max = v;
            minPos = pos;
            maxPos = pos;
            absMax = std::abs(v);
            absMaxPos = pos;
        } else {
            if (v < min) {
                min = v;
                minPos = pos;
            }
            if (v > max) {
                max = v;
                maxPos = pos;
            }
            if (const double a = std::abs(v); a > absMax) {
                absMax = a;
                absMaxPos = pos;
            }
        }
        const double d = v - shift;
        sum += d;
        sumSq += d * d;
        lastValidPos = pos;
        lastValid = v;
        ++valid;
    }
};

// Appends the scan of a later, disjoint interval (both in times) to `into`: the trusted pieces of a row that
// untrusted laps were cut out of (D-197 (b)). The sums move to into's shift exactly (sum of (v - a) over the
// later piece is sum + n (b - a), and the squares follow), extremes keep the earlier position on a tie.
void appendScan(Scan& into, const Scan& next) noexcept {
    if (next.samples == 0) {
        return;
    }
    if (into.samples == 0) {
        into = next;
        return;
    }
    into.lastSample = next.lastSample;
    into.samples += next.samples;
    into.coversEnd = next.coversEnd;
    if (next.valid == 0) {
        return;
    }
    if (into.valid == 0) {
        const std::int64_t samples = into.samples;
        const std::int64_t firstSample = into.firstSample;
        const bool coversStart = into.coversStart;
        into = next;
        into.samples = samples;
        into.firstSample = firstSample;
        into.coversStart = coversStart;
        return;
    }
    const auto n = static_cast<double>(next.valid);
    const double d = next.shift - into.shift;
    into.sumSq += next.sumSq + 2.0 * d * next.sum + n * d * d;
    into.sum += next.sum + n * d;
    if (next.min < into.min) {
        into.min = next.min;
        into.minPos = next.minPos;
    }
    if (next.max > into.max) {
        into.max = next.max;
        into.maxPos = next.maxPos;
    }
    if (next.absMax > into.absMax) {
        into.absMax = next.absMax;
        into.absMaxPos = next.absMaxPos;
    }
    into.lastValidPos = next.lastValidPos;
    into.lastValid = next.lastValid;
    into.valid += next.valid;
}

// The extreme of two lanes: `better` takes the candidate, equal values keep the earlier index (the first
// occurrence).
void pickExtreme(double v, std::uint32_t at, bool better, double& best, std::uint32_t& bestAt) noexcept {
    if (better || (v == best && at < bestAt)) {
        best = v;
        bestAt = at;
    }
}

// A run [i0, i1) of valid samples in one pass. Two interleaved lanes of plain locals (they stay in
// registers) halve the dependency chains of the sums and the extremes; the extremes keep the index of their
// first occurrence through selects (a smooth signal moves them often, so branches would mispredict). The
// largest magnitude is max(-min, max) at the earlier of the two positions on a tie. The lanes and the run
// boundaries depend only on the indices, so a result is reproducible.
template <class T>
void addRun(const T* data, std::uint32_t i0, std::uint32_t i1, SampleIdx base, Scan& s) noexcept {
    if (i0 >= i1) {
        return;
    }
    if (s.valid == 0) {
        const auto v = static_cast<double>(data[i0]);
        s.shift = v;
        s.firstValidPos = base + i0;
        s.firstValid = v;
        s.min = v;
        s.max = v;
        s.minPos = base + i0;
        s.maxPos = base + i0;
        s.absMax = std::abs(v);
        s.absMaxPos = base + i0;
    }
    const double shift = s.shift;
    const auto first = static_cast<double>(data[i0]);
    double sum0 = 0.0;
    double sum1 = 0.0;
    double sq0 = 0.0;
    double sq1 = 0.0;
    double lo0 = first;
    double lo1 = first;
    double hi0 = first;
    double hi1 = first;
    std::uint32_t loAt0 = i0;
    std::uint32_t loAt1 = i0;
    std::uint32_t hiAt0 = i0;
    std::uint32_t hiAt1 = i0;
    std::uint32_t i = i0;
    for (; i + 2 <= i1; i += 2) {
        const auto v0 = static_cast<double>(data[i]);
        const auto v1 = static_cast<double>(data[i + 1]);
        const double d0 = v0 - shift;
        const double d1 = v1 - shift;
        sum0 += d0;
        sum1 += d1;
        sq0 += d0 * d0;
        sq1 += d1 * d1;
        loAt0 = v0 < lo0 ? i : loAt0;
        lo0 = v0 < lo0 ? v0 : lo0;
        loAt1 = v1 < lo1 ? i + 1 : loAt1;
        lo1 = v1 < lo1 ? v1 : lo1;
        hiAt0 = v0 > hi0 ? i : hiAt0;
        hi0 = v0 > hi0 ? v0 : hi0;
        hiAt1 = v1 > hi1 ? i + 1 : hiAt1;
        hi1 = v1 > hi1 ? v1 : hi1;
    }
    if (i < i1) {
        const auto v0 = static_cast<double>(data[i]);
        const double d0 = v0 - shift;
        sum0 += d0;
        sq0 += d0 * d0;
        loAt0 = v0 < lo0 ? i : loAt0;
        lo0 = v0 < lo0 ? v0 : lo0;
        hiAt0 = v0 > hi0 ? i : hiAt0;
        hi0 = v0 > hi0 ? v0 : hi0;
    }
    pickExtreme(lo1, loAt1, lo1 < lo0, lo0, loAt0);
    pickExtreme(hi1, hiAt1, hi1 > hi0, hi0, hiAt0);
    s.sum += sum0 + sum1;
    s.sumSq += sq0 + sq1;
    if (lo0 < s.min) {
        s.min = lo0;
        s.minPos = base + loAt0;
    }
    if (hi0 > s.max) {
        s.max = hi0;
        s.maxPos = base + hiAt0;
    }
    const double absRun = std::max(-lo0, hi0);
    const std::uint32_t absAt = -lo0 > hi0 ? loAt0 : (hi0 > -lo0 ? hiAt0 : std::min(loAt0, hiAt0));
    if (absRun > s.absMax) {
        s.absMax = absRun;
        s.absMaxPos = base + absAt;
    }
    s.lastValidPos = base + i1 - 1;
    s.lastValid = static_cast<double>(data[i1 - 1]);
    s.valid += static_cast<std::int64_t>(i1 - i0);
}

// Calls fn(a, b) for every run [a, b) of set bits of `bits` inside [i0, i1).
template <class Fn>
void forEachSetRun(const std::uint64_t* bits, std::uint32_t i0, std::uint32_t i1, Fn&& fn) noexcept {
    std::uint32_t i = i0;
    while (i < i1) {
        std::uint32_t w = i >> 6;
        std::uint64_t word = bits[w] & (~std::uint64_t{0} << (i & 63u));
        while (word == 0) {
            ++w;
            if ((w << 6) >= i1) {
                return;
            }
            word = bits[w];
        }
        const std::uint32_t a = (w << 6) + static_cast<std::uint32_t>(std::countr_zero(word));
        if (a >= i1) {
            return;
        }
        w = a >> 6;
        std::uint64_t gaps = ~bits[w] & (~std::uint64_t{0} << (a & 63u));
        std::uint32_t b = i1;
        while (true) {
            if (gaps != 0) {
                b = std::min(i1, (w << 6) + static_cast<std::uint32_t>(std::countr_zero(gaps)));
                break;
            }
            ++w;
            if ((w << 6) >= i1) {
                break;
            }
            gaps = ~bits[w];
        }
        fn(a, b);
        i = b;
    }
}

template <class T>
void scanSamples(const T* data, const std::uint64_t* bits, std::uint32_t i0, std::uint32_t i1, SampleIdx base,
                 Scan& s) noexcept {
    if (bits == nullptr) {
        addRun(data, i0, i1, base, s);
        return;
    }
    forEachSetRun(bits, i0, i1, [&](std::uint32_t a, std::uint32_t b) { addRun(data, a, b, base, s); });
}

// Valid samples [i0, i1) of one chunk whose first sample is `base`.
void scanChunk(const store::Chunk& chunk, std::uint32_t i0, std::uint32_t i1, SampleIdx base,
               Scan& s) noexcept {
    const std::uint64_t* bits = nullptr;
    switch (chunk.validity()) {
    case store::Validity::AllInvalid:
        return;
    case store::Validity::AllValid:
        break;
    case store::Validity::Bitmap:
        bits = chunk.bitmap();
        break;
    }
    switch (chunk.dtype()) {
    case store::Dtype::F32:
        scanSamples(chunk.samples<float>().data(), bits, i0, i1, base, s);
        return;
    case store::Dtype::F64:
        scanSamples(chunk.samples<double>().data(), bits, i0, i1, base, s);
        return;
    case store::Dtype::I32:
        scanSamples(chunk.samples<std::int32_t>().data(), bits, i0, i1, base, s);
        return;
    case store::Dtype::I64:
        for (std::uint32_t i = i0; i < i1; ++i) {
            if (chunk.validAt(i)) {
                s.addValid(base + i, chunk.valueAt(i));
            }
        }
        return;
    }
}

// The samples of [start, end).
Result<Scan> scanSeries(const store::Series& series, Micros start, Micros end, const CancelToken& cancel) {
    Scan s;
    if (end <= start) {
        return s;
    }
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        const SampleIdx n = u->size();
        if (n <= 0 || !u->rate.valid()) {
            return s;
        }
        const SampleIdx first = std::max<SampleIdx>(u->rate.indexAt(saturatingSub(start, 1), u->t0) + 1, 0);
        const SampleIdx last = std::min<SampleIdx>(u->rate.indexAt(end - 1, u->t0), n - 1);
        if (first > last) {
            return s;
        }
        s.samples = last - first + 1;
        s.firstSample = first;
        s.lastSample = last;
        for (SampleIdx c = store::chunkOf(first); c <= store::chunkOf(last); c += kScanChunks) {
            if (cancel.checkpoint()) {
                return detail::cancelledFailure();
            }
            const SampleIdx blockFirst = std::max(first, store::chunkFirstSample(c));
            const SampleIdx blockLast = std::min(last, store::chunkFirstSample(c + kScanChunks) - 1);
            Result<store::ChunkHandle> pinned = u->values.require(blockFirst, blockLast, kPrio, cancel);
            if (!pinned) {
                return Failure{pinned.error()};
            }
            for (SampleIdx k = store::chunkOf(blockFirst); k <= store::chunkOf(blockLast); ++k) {
                const store::Chunk* chunk = pinned->chunk(k);
                if (chunk == nullptr) {
                    return Failure{Error{ErrorCode::Internal, "a required chunk is not pinned", {}}};
                }
                const SampleIdx a = std::max(blockFirst, store::chunkFirstSample(k));
                const SampleIdx b = std::min(blockLast, store::chunkFirstSample(k + 1) - 1);
                scanChunk(*chunk, store::chunkLocal(a), store::chunkLocal(b) + 1, store::chunkFirstSample(k),
                          s);
            }
        }
        const auto timeOf = [u](std::int64_t i) { return u->timeAt(i); };
        const Micros period = u->timeAt(1) - u->timeAt(0);
        s.coversStart = first > 0 || timeOf(first) - start < period;
        s.coversEnd = last < n - 1 || end - timeOf(last) <= period;
        s.firstSample = timeOf(s.firstSample);
        s.lastSample = timeOf(s.lastSample);
        if (s.valid > 0) {
            s.firstValidPos = timeOf(s.firstValidPos);
            s.lastValidPos = timeOf(s.lastValidPos);
            s.minPos = timeOf(s.minPos);
            s.maxPos = timeOf(s.maxPos);
            s.absMaxPos = timeOf(s.absMaxPos);
        }
        return s;
    }
    Result<void> visited =
        detail::forEachSample(series, start, end, kPrio, cancel, [&s](Micros t, double v, bool valid) {
            s.addSample(t);
            if (valid) {
                s.addValid(t, v);
            }
        });
    if (!visited) {
        return Failure{visited.error()};
    }
    return s;
}

// The value at exactly `t` by `interp` (store::Resampler's policy); nullopt when invalid or outside the
// series.
Result<std::optional<double>> valueAtEdge(const store::Series& series, Micros t, store::Interp interp,
                                          const CancelToken& cancel) {
    const Rate one = Rate::fromHz(1); // one output sample at t0 = t
    Result<store::Resampler::PinnedInput> pins =
        store::Resampler::pinFor(series, one, t, 0, 1, kPrio, cancel);
    if (!pins) {
        return Failure{pins.error()};
    }
    const bool timestamped = std::holds_alternative<store::TimestampedSeries>(series);
    Result<store::ResampleView> view = store::Resampler::block(series, one, t, 0, 1, interp, pins->values,
                                                               timestamped ? &pins->times : nullptr);
    if (!view || !view->valid(0)) {
        return std::optional<double>{};
    }
    const double v = view->f64(0);
    return std::isnan(v) ? std::optional<double>{} : std::optional<double>{v};
}

struct Edges {
    std::optional<double> start;
    std::optional<double> end;
};

bool needsEdges(ReportStat stat) noexcept {
    return stat == ReportStat::Start || stat == ReportStat::End || stat == ReportStat::Change;
}

StatValue startOf(const Scan& s, Micros rangeStart, Micros probeUs, const Edges& edges) {
    if (edges.start) {
        return StatValue{*edges.start, rangeStart, true};
    }
    if (s.valid > 0 &&
        ((s.firstValidPos == s.firstSample && s.coversStart) || s.firstValidPos - rangeStart <= probeUs)) {
        return StatValue{s.firstValid, s.firstValidPos, true};
    }
    return {};
}

StatValue endOf(const Scan& s, Micros rangeEnd, Micros probeUs, const Edges& edges) {
    if (edges.end) {
        return StatValue{*edges.end, rangeEnd, true};
    }
    if (s.valid > 0 &&
        ((s.lastValidPos == s.lastSample && s.coversEnd) || rangeEnd - s.lastValidPos <= probeUs)) {
        return StatValue{s.lastValid, s.lastValidPos, true};
    }
    return {};
}

StatValue statOf(const Scan& s, ReportStat stat, Micros start, Micros end, Micros probeUs,
                 const Edges& edges) {
    const auto n = static_cast<double>(s.valid);
    switch (stat) {
    case ReportStat::Avg:
        return s.valid > 0 ? StatValue{s.shift + s.sum / n, 0, true} : StatValue{};
    case ReportStat::Min:
        return s.valid > 0 ? StatValue{s.min, s.minPos, true} : StatValue{};
    case ReportStat::Max:
        return s.valid > 0 ? StatValue{s.max, s.maxPos, true} : StatValue{};
    case ReportStat::AbsMax:
        return s.valid > 0 ? StatValue{s.absMax, s.absMaxPos, true} : StatValue{};
    case ReportStat::StdDev: {
        if (s.valid == 0) {
            return {};
        }
        const double variance = std::max(0.0, (s.sumSq - s.sum * s.sum / n) / n);
        return StatValue{std::sqrt(variance), 0, true};
    }
    case ReportStat::Delta:
        return s.valid > 0 ? StatValue{s.max - s.min, 0, true} : StatValue{};
    case ReportStat::Start:
        return startOf(s, start, probeUs, edges);
    case ReportStat::End:
        return endOf(s, end, probeUs, edges);
    case ReportStat::Change: {
        const StatValue a = startOf(s, start, probeUs, edges);
        const StatValue b = endOf(s, end, probeUs, edges);
        return a.valid && b.valid ? StatValue{b.value - a.value, b.at, true} : StatValue{};
    }
    }
    return {};
}

Micros probeMicros(const ReportOptions& options) noexcept {
    const double us = std::max(0.0, options.probeSeconds) * 1'000'000.0;
    return us >= 9.0e18 ? kMicrosMax : static_cast<Micros>(std::llround(us));
}

// Whether start/end/change read the value at the exact range edge: interpolateBoundaries, unless
// ignoreInterpolated keeps every statistic on the channel's own samples (D-210 (c)).
bool interpolatesEdges(const ReportOptions& options) noexcept {
    return options.interpolateBoundaries && !options.ignoreInterpolated;
}

// The edges of [start, end) when the options interpolate and a requested statistic needs them.
Result<Edges> edgesFor(const store::Series& series, Micros start, Micros end, const ReportOptions& options,
                       store::Interp interp, bool needed, const CancelToken& cancel) {
    Edges e;
    if (!interpolatesEdges(options) || !needed) {
        return e;
    }
    Result<std::optional<double>> a = valueAtEdge(series, start, interp, cancel);
    if (!a) {
        return Failure{a.error()};
    }
    Result<std::optional<double>> b = valueAtEdge(series, end, interp, cancel);
    if (!b) {
        return Failure{b.error()};
    }
    e.start = *a;
    e.end = *b;
    return e;
}

// ---- rows ------------------------------------------------------------------------------------------------

struct Row {
    std::size_t source = 0; // index into ReportInputs::sources
    std::vector<std::string> path;
    // The names of the path's levels that lie inside the source range (sections of a lap, laps of an outing):
    // the Main row pairs with the Overlay 0 row of the same pairKey (D-197 (c)). The levels at or above the
    // source range (the range itself, an enclosing outing, a detail label) differ between Main and Overlay 0
    // whenever they compare different laps, outings or drivers, so they never take part.
    std::vector<std::string> pairKey;
    // The trusted pieces of [start, end) when untrusted laps were cut out of it (D-197 (b)); empty: the
    // whole.
    std::vector<std::pair<Micros, Micros>> pieces;
    Micros start = 0;
    Micros end = 0;
    bool trusted = true;
    model::RangeId range; // the narrowest range the row lies in (the cache key's range)
    Micros rangeDuration = 0;
    ReportSlotRole role = ReportSlotRole::Main;
    bool hidden = false; // an Overlay 0 row kept only to pair the delta columns (excludeOverlays)
};

// The detail-group headings of the circuit profile (data/rules/circuit.detail-groups.rule.json): the heading
// a picker offers, the detail it reads and the label of a source without that detail (D-197 (b)). A heading
// outside the table reads the detail of its own name and is "" without one.
struct DetailHeading {
    std::string_view heading;
    std::string_view detail;
    std::string_view missing;
};
constexpr std::array<DetailHeading, 6> kDetailHeadings{{{"Events", "Event", "(no event)"},
                                                        {"Vehicles", "Vehicle Id", "(no vehicle)"},
                                                        {"Sessions", "Session", "(no session)"},
                                                        {"Driver", "Driver", "(no driver)"},
                                                        {"Vehicle Id", "Vehicle Id", "(none)"},
                                                        {"Vehicle Number", "Vehicle Number", "(none)"}}};

std::string detailLabel(const model::DataSource& src, std::string_view heading) {
    std::string_view id = heading;
    std::string_view missing;
    for (const DetailHeading& h : kDetailHeadings) {
        if (h.heading == heading) {
            id = h.detail;
            missing = h.missing;
            break;
        }
    }
    const model::Detail* d = model::findDetail(src.details, id);
    if (d == nullptr) {
        return std::string(missing);
    }
    std::string text = model::detailValueText(d->value);
    return text.empty() ? std::string(missing) : text;
}

// [start, end) minus the untrusted laps of `laps` (time-ordered, disjoint); empty when nothing remains.
std::vector<std::pair<Micros, Micros>> trustedPieces(Micros start, Micros end,
                                                     const model::RangeGroup* laps) {
    std::vector<std::pair<Micros, Micros>> out;
    Micros at = start;
    if (laps != nullptr) {
        for (const model::Range& lap : laps->ranges) {
            if (lap.start >= end) {
                break;
            }
            if (lap.trusted || lap.end <= at) {
                continue;
            }
            if (lap.start > at) {
                out.emplace_back(at, lap.start);
            }
            at = std::max(at, lap.end);
        }
    }
    if (at < end) {
        out.emplace_back(at, end);
    }
    return out;
}

// The rows of one source range, divided by the headers in order (see the header file).
void divide(const ReportSpec& spec, const ReportSource& source, std::size_t sourceIndex,
            const reports_detail::ResolvedRange& resolved, bool hidden, std::vector<Row>& out) {
    const model::Range& range = *resolved.range;
    Micros start = range.start;
    Micros end = range.end;
    if (spec.options.zoomLinked && source.clip) {
        start = std::max(start, source.clip->first);
        end = std::min(end, source.clip->second);
    }
    if (end <= start) {
        return;
    }
    Row seed;
    seed.source = sourceIndex;
    seed.start = start;
    seed.end = end;
    seed.trusted = range.trusted;
    seed.range = range.id;
    seed.rangeDuration = range.duration();
    seed.role = source.role;
    seed.hidden = hidden;
    std::vector<Row> rows{seed};
    if (spec.headers.empty()) {
        rows.front().path.push_back(range.name);
    }
    // A level inside the source range: a group range that does not enclose the whole source range.
    const auto inside = [&range](const model::Range& g) {
        return g.start > range.start || g.end < range.end;
    };
    for (const std::string& header : spec.headers) {
        std::vector<Row> next;
        const model::RangeGroup* group = resolved.layer->ranges.find(header);
        if (group == nullptr) {
            const std::string label = detailLabel(*source.source, header);
            for (Row& r : rows) {
                r.path.push_back(label);
                next.push_back(std::move(r));
            }
            rows = std::move(next);
            continue;
        }
        for (const Row& r : rows) {
            for (const model::Range& g : group->ranges) {
                if (g.start >= r.end) {
                    break; // time-ordered by start
                }
                const Micros a = std::max(r.start, g.start);
                const Micros b = std::min(r.end, g.end);
                if (b <= a || !g.enabled || (!g.trusted && !spec.options.includeUntrusted)) {
                    continue;
                }
                Row child = r;
                child.path.push_back(g.name);
                if (inside(g)) {
                    child.pairKey.push_back(g.name);
                }
                child.start = a;
                child.end = b;
                child.trusted = r.trusted && g.trusted;
                if (g.duration() < child.rangeDuration) {
                    child.range = g.id;
                    child.rangeDuration = g.duration();
                }
                next.push_back(std::move(child));
            }
        }
        rows = std::move(next);
    }
    // Untrusted data stays out of every row (D-197 (b)): the untrusted laps are cut out of rows coarser than
    // a lap (an outing, a detail heading); a row that lies wholly in them is dropped.
    const model::RangeGroup* laps =
        spec.options.includeUntrusted ? nullptr : resolved.layer->ranges.find("Outings:Laps");
    for (Row& r : rows) {
        if (laps != nullptr) {
            std::vector<std::pair<Micros, Micros>> pieces = trustedPieces(r.start, r.end, laps);
            if (pieces.empty()) {
                continue;
            }
            if (pieces.size() > 1 || pieces.front() != std::make_pair(r.start, r.end)) {
                r.pieces = std::move(pieces);
            }
        }
        out.push_back(std::move(r));
    }
}

// Rows of every source in scope, ordered chronologically and grouped by heading (see the header file).
std::vector<Row> buildRows(const ReportSpec& spec, const ReportInputs& inputs) {
    struct Candidate {
        std::size_t index = 0;
        std::size_t rank = 0;
        Micros start = 0;
        reports_detail::ResolvedRange resolved;
    };
    const std::vector<std::size_t> ranks = reports_detail::chronologicalRanks(inputs.sources);
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < inputs.sources.size(); ++i) {
        const reports_detail::ResolvedRange resolved = reports_detail::resolveRange(inputs.sources[i]);
        if (resolved.range == nullptr) {
            continue;
        }
        candidates.push_back(Candidate{i, ranks[i], resolved.range->start, resolved});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.start < b.start;
    });
    std::vector<Row> rows;
    for (const Candidate& c : candidates) {
        const ReportSource& s = inputs.sources[c.index];
        if (!c.resolved.range->trusted && !spec.options.includeUntrusted) {
            continue;
        }
        bool hidden = false;
        if (s.role != ReportSlotRole::Main && spec.options.excludeOverlays) {
            if (s.role != ReportSlotRole::Overlay0 || !spec.deltaVsOverlay0) {
                continue;
            }
            hidden = true;
        }
        divide(spec, s, c.index, c.resolved, hidden, rows);
    }
    // Group equal names at each level under their first occurrence; generation order within a heading.
    std::map<std::vector<std::string>, std::size_t> firstSeen;
    std::vector<std::vector<std::size_t>> keys(rows.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::vector<std::string> prefix;
        for (const std::string& name : rows[r].path) {
            prefix.push_back(name);
            keys[r].push_back(firstSeen.try_emplace(prefix, r).first->second);
        }
    }
    std::vector<std::size_t> order(rows.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&keys](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
    std::vector<Row> sorted;
    sorted.reserve(rows.size());
    for (const std::size_t r : order) {
        sorted.push_back(std::move(rows[r]));
    }
    return sorted;
}

// ---- cells -----------------------------------------------------------------------------------------------

struct ColumnPlan {
    std::size_t channel = 0;
    ReportStat stat = ReportStat::Avg;
    bool delta = false;
    std::size_t statColumn = 0; // a delta column: the column it takes the difference of
    bool hidden = false;        // a min or max the display conversion of min/max/absMax needs (not shown)
};

// The base minimum and maximum of a (row, channel), for the conversions that need both.
struct Extremes {
    std::optional<StatValue> min;
    std::optional<StatValue> max;
};

// A base-unit statistic in the display unit of `c` (an affine x -> a x + b). Values convert with c; a
// difference with |a| when it is a magnitude (delta, stdDev) and with a when it is signed (change). A
// negative scale swaps the minimum and the maximum; the largest display magnitude of an affine c lies at the
// base minimum or maximum, so absMax is max(|c(min)|, |c(max)|) (exactly |a| absMax when b is 0).
double toDisplay(ReportStat stat, double v, const units::Converter& c, const Extremes& e) noexcept {
    switch (stat) {
    case ReportStat::Change:
        return c.a * v;
    case ReportStat::Delta:
    case ReportStat::StdDev:
        return std::abs(c.a) * v;
    case ReportStat::Min:
        return c.a < 0.0 && e.max && e.max->valid ? c(e.max->value) : c(v);
    case ReportStat::Max:
        return c.a < 0.0 && e.min && e.min->valid ? c(e.min->value) : c(v);
    case ReportStat::AbsMax:
        if (c.b != 0.0 && e.min && e.max && e.min->valid && e.max->valid) {
            return std::max(std::abs(c(e.min->value)), std::abs(c(e.max->value)));
        }
        return std::abs(c.a) * v;
    case ReportStat::Avg:
    case ReportStat::Start:
    case ReportStat::End:
        break;
    }
    return c(v);
}

std::uint64_t piecesHashOf(const std::vector<std::pair<Micros, Micros>>& pieces) {
    if (pieces.empty()) {
        return 0;
    }
    Hash128Stream h;
    for (const auto& [a, b] : pieces) {
        h.updateU64(static_cast<std::uint64_t>(a));
        h.updateU64(static_cast<std::uint64_t>(b));
    }
    return h.digest().lo | 1u;
}

// One scan over the row's interval, or over its trusted pieces.
Result<Scan> scanRow(const store::Series& series, Micros start, Micros end,
                     const std::vector<std::pair<Micros, Micros>>& pieces, const CancelToken& cancel) {
    if (pieces.empty()) {
        return scanSeries(series, start, end, cancel);
    }
    Scan acc;
    for (const auto& [a, b] : pieces) {
        Result<Scan> one = scanSeries(series, a, b, cancel);
        if (!one) {
            return Failure{one.error()};
        }
        appendScan(acc, *one);
    }
    return acc;
}

std::uint64_t optionsHashOf(const ReportOptions& options, store::Interp interp) {
    Hash128Stream h;
    h.updateU64(static_cast<std::uint64_t>(probeMicros(options)));
    // ignoreInterpolated gives the cells of interpolateBoundaries off, so it shares their key.
    h.updateU64(interpolatesEdges(options) ? static_cast<std::uint64_t>(interp) + 1u : 0u);
    return h.digest().lo | 1u; // never 0, the value of the statistics that ignore the options
}

std::uint64_t columnVersion(const store::Column& column) noexcept {
    return column.version() ^ (column.id() * 0x9E3779B97F4A7C15ull);
}

void markExtremes(ReportTable& table) {
    const std::size_t nc = table.columns.size();
    for (std::size_t c = 0; c < nc; ++c) {
        std::size_t ok = 0;
        double lo = 0.0;
        double hi = 0.0;
        for (std::size_t r = 0; r < table.rows.size(); ++r) {
            const ReportCell& cell = table.cells[r * nc + c];
            if (cell.state != ReportCellState::Ok) {
                continue;
            }
            lo = ok == 0 ? cell.value : std::min(lo, cell.value);
            hi = ok == 0 ? cell.value : std::max(hi, cell.value);
            ++ok;
        }
        if (ok < 2 || lo == hi) {
            continue;
        }
        for (std::size_t r = 0; r < table.rows.size(); ++r) {
            ReportCell& cell = table.cells[r * nc + c];
            if (cell.state == ReportCellState::Ok) {
                cell.isMin = cell.value == lo;
                cell.isMax = cell.value == hi;
            }
        }
    }
}

} // namespace

std::size_t ReportTable::pending() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        cells.begin(), cells.end(), [](const ReportCell& c) { return c.state == ReportCellState::Pending; }));
}

Result<StatValue> ReportEngine::statistic(const store::Series& series, ReportStat stat, Micros start,
                                          Micros end, const ReportOptions& options, const CancelToken& cancel,
                                          store::Interp interp) {
    if (cancel.checkpoint()) {
        return detail::cancelledFailure();
    }
    Result<Scan> scan = scanSeries(series, start, end, cancel);
    if (!scan) {
        return Failure{scan.error()};
    }
    Result<Edges> edges = edgesFor(series, start, end, options, interp, needsEdges(stat), cancel);
    if (!edges) {
        return Failure{edges.error()};
    }
    return statOf(*scan, stat, start, end, probeMicros(options), *edges);
}

Result<ReportTable> ReportEngine::cells(const ReportSpec& spec, const ReportInputs& inputs, StatCache& cache,
                                        const CancelToken& cancel) {
    if (spec.channels.empty()) {
        return Failure{Error{ErrorCode::Invalid, "a channel report needs at least one channel", {}}};
    }
    if (cancel.checkpoint()) {
        return detail::cancelledFailure();
    }
    std::vector<Row> rows = buildRows(spec, inputs);

    // Columns: (channel, stat) in spec order, each followed by its delta column.
    std::vector<ColumnPlan> plan;
    for (std::size_t ch = 0; ch < spec.channels.size(); ++ch) {
        const std::vector<ReportStat>& stats = spec.channels[ch].stats;
        for (const ReportStat stat : stats) {
            const std::size_t statColumn = plan.size();
            plan.push_back(ColumnPlan{ch, stat, false, statColumn, false});
            if (spec.deltaVsOverlay0) {
                plan.push_back(ColumnPlan{ch, stat, true, statColumn, false});
            }
        }
        // min, max and absMax convert through both extremes (toDisplay): a missing one is a hidden column.
        const auto has = [&stats](ReportStat st) {
            return std::find(stats.begin(), stats.end(), st) != stats.end();
        };
        if (has(ReportStat::Min) || has(ReportStat::Max) || has(ReportStat::AbsMax)) {
            for (const ReportStat extreme : {ReportStat::Min, ReportStat::Max}) {
                if (!has(extreme)) {
                    plan.push_back(ColumnPlan{ch, extreme, false, plan.size(), true});
                }
            }
        }
    }
    const std::size_t nc = plan.size();
    const std::size_t nch = spec.channels.size();

    // Channel lookups per (source, channel), on first use.
    std::vector<std::optional<ReportChannelData>> lookups(inputs.sources.size() * nch);
    const auto lookup = [&](std::size_t source, std::size_t ch) -> const ReportChannelData& {
        std::optional<ReportChannelData>& slot = lookups[source * nch + ch];
        if (!slot) {
            slot = inputs.channels ? inputs.channels(inputs.sources[source], spec.channels[ch])
                                   : ReportChannelData{};
        }
        return *slot;
    };

    std::vector<std::vector<std::size_t>> statColumnsOf(nch);
    for (std::size_t c = 0; c < nc; ++c) {
        if (!plan[c].delta) {
            statColumnsOf[plan[c].channel].push_back(c);
        }
    }

    // 1. Every (row, channel): its state, and the cells the StatCache already holds. The (row, channel)
    //    pairs with a missing cell become jobs.
    struct Job {
        std::size_t row = 0;
        std::size_t channel = 0;
        StatKey key;
        bool cacheable = false;
        store::Interp interp = store::Interp::Linear;
    };
    std::vector<ReportCell> grid(rows.size() * nc);
    std::vector<std::optional<StatValue>> values(rows.size() * nc);
    std::vector<Job> jobs;
    const std::array<std::uint64_t, 4> edgeHashes{optionsHashOf(spec.options, store::Interp::Linear),
                                                  optionsHashOf(spec.options, store::Interp::Closest),
                                                  optionsHashOf(spec.options, store::Interp::Previous),
                                                  optionsHashOf(spec.options, store::Interp::Next)};
    const auto keyFor = [&edgeHashes](StatKey key, ReportStat stat, store::Interp interp) {
        key.stat = stat;
        key.optionsHash = needsEdges(stat) ? edgeHashes[static_cast<std::size_t>(interp) & 3u] : 0;
        return key;
    };
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Row& row = rows[r];
        for (std::size_t ch = 0; ch < nch; ++ch) {
            const std::vector<std::size_t>& columns = statColumnsOf[ch];
            if (columns.empty()) {
                continue;
            }
            const ReportChannelData& data = lookup(row.source, ch);
            ReportCellState state = ReportCellState::Ok;
            if (data.status == model::ChannelStatus::Pending) {
                state = ReportCellState::Pending;
            } else if (data.status == model::ChannelStatus::Error) {
                state = ReportCellState::Error;
            } else if (!data.series) {
                state = ReportCellState::Missing;
            }
            for (const std::size_t c : columns) {
                grid[r * nc + c].state = state; // Ok: to be filled by phase 3
            }
            if (state != ReportCellState::Ok) {
                continue;
            }
            Job job;
            job.row = r;
            job.channel = ch;
            if (const std::optional<model::ChannelDescriptor>& desc = data.descriptor) {
                job.cacheable = true;
                job.interp = desc->interp;
                job.key.channel = desc->id;
                job.key.range = row.range;
                job.key.clipStart = row.start;
                job.key.clipEnd = row.end;
                job.key.piecesHash = piecesHashOf(row.pieces);
                if (const std::optional<store::Series>& series = data.series) {
                    job.key.version = columnVersion(store::seriesValues(*series));
                }
            }
            bool missing = false;
            for (const std::size_t c : columns) {
                if (job.cacheable) {
                    values[r * nc + c] = cache.find(keyFor(job.key, plan[c].stat, job.interp));
                }
                missing = missing || !values[r * nc + c];
            }
            if (missing) {
                jobs.push_back(job);
            }
        }
    }

    // 2. The jobs in order, one scan each. The chunks of the next jobs are prefetched (Column::prefetch,
    //    Background: never blocks, coalesced, a no-op without a TaskPool) up to kPrefetchChunks ahead of the
    //    scan, so idle workers materialise them while this thread scans.
    const auto prefetch = [&](const Job& job, SampleIdx budget) -> SampleIdx {
        const ReportChannelData& data = lookup(rows[job.row].source, job.channel);
        const store::UniformSeries* u =
            data.series ? std::get_if<store::UniformSeries>(&*data.series) : nullptr;
        if (u == nullptr || u->size() <= 0 || !u->rate.valid()) {
            return 0;
        }
        const SampleIdx first =
            std::max<SampleIdx>(u->rate.indexAt(saturatingSub(rows[job.row].start, 1), u->t0) + 1, 0);
        const SampleIdx last =
            std::min<SampleIdx>(u->rate.indexAt(rows[job.row].end - 1, u->t0), u->size() - 1);
        SampleIdx issued = 0;
        for (SampleIdx c = store::chunkOf(first);
             first <= last && c <= store::chunkOf(last) && issued < budget; ++c, ++issued) {
            u->values.prefetch(c, kPrio);
        }
        return issued;
    };
    const Micros probeUs = probeMicros(spec.options);
    std::vector<SampleIdx> issuedFor(jobs.size(), 0);
    std::size_t ahead = 0;  // the next job to prefetch
    SampleIdx inFlight = 0; // chunks prefetched for jobs not scanned yet
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (cancel.checkpoint()) {
            return detail::cancelledFailure();
        }
        for (; ahead < jobs.size() && inFlight < kPrefetchChunks; ++ahead) {
            issuedFor[ahead] = prefetch(jobs[ahead], kPrefetchChunks - inFlight);
            inFlight += issuedFor[ahead];
        }
        ahead = std::max(ahead, j + 1);
        inFlight -= issuedFor[j];
        const Job& job = jobs[j];
        const Row& row = rows[job.row];
        const std::vector<std::size_t>& columns = statColumnsOf[job.channel];
        const ReportChannelData& data = lookup(row.source, job.channel);
        const std::optional<store::Series>& series = data.series;
        if (!series) {
            continue; // not reached: jobs have a series
        }
        bool edgesNeeded = false;
        for (const std::size_t c : columns) {
            edgesNeeded = edgesNeeded || (!values[job.row * nc + c] && needsEdges(plan[c].stat));
        }
        const Micros scanStart = row.pieces.empty() ? row.start : row.pieces.front().first;
        const Micros scanEnd = row.pieces.empty() ? row.end : row.pieces.back().second;
        Result<Scan> scan = scanRow(*series, row.start, row.end, row.pieces, cancel);
        Result<Edges> edges =
            scan ? edgesFor(*series, scanStart, scanEnd, spec.options, job.interp, edgesNeeded, cancel)
                 : Result<Edges>(Failure{scan.error()});
        if (!edges) {
            if (edges.error().code == ErrorCode::Cancelled) {
                return Failure{edges.error()};
            }
            for (const std::size_t c : columns) {
                grid[job.row * nc + c].state = ReportCellState::Error;
            }
            continue;
        }
        for (const std::size_t c : columns) {
            std::optional<StatValue>& slot = values[job.row * nc + c];
            if (slot) {
                continue;
            }
            const StatValue v = statOf(*scan, plan[c].stat, scanStart, scanEnd, probeUs, *edges);
            slot = v;
            if (job.cacheable) {
                cache.insert(keyFor(job.key, plan[c].stat, job.interp), v);
            }
        }
    }

    // The column unit of each channel: the display unit and decimals of the first source that resolves it
    // (Main rows first). Every other source's values convert into it (D-197 (g)).
    std::vector<const ReportChannelData*> columnUnits(nch, nullptr);
    for (std::size_t ch = 0; ch < nch; ++ch) {
        for (int pass = 0; pass < 2 && columnUnits[ch] == nullptr; ++pass) {
            for (const Row& row : rows) {
                if ((pass == 0) != (row.role == ReportSlotRole::Main)) {
                    continue;
                }
                const ReportChannelData& data = lookup(row.source, ch);
                if (data.series || data.descriptor) {
                    columnUnits[ch] = &data;
                    break;
                }
            }
        }
    }
    // base -> the column's unit for one (source, channel); nullopt when the units do not convert.
    const auto converterOf = [&](const ReportChannelData& data,
                                 std::size_t ch) -> std::optional<units::Converter> {
        const ReportChannelData* column = columnUnits[ch];
        if (column == nullptr || column == &data || data.displayUnit == column->displayUnit) {
            return data.toDisplay;
        }
        if (inputs.units == nullptr || !data.descriptor) {
            return std::nullopt;
        }
        const units::UnitRegistry& registry = *inputs.units;
        const units::UnitId base = data.descriptor->unit;
        if (!registry.contains(base)) {
            return std::nullopt;
        }
        const std::optional<units::UnitId> target =
            registry.find(column->displayUnit, registry.quantityOf(base));
        if (!target) {
            return std::nullopt;
        }
        Result<units::Converter> c = registry.converter(base, *target);
        if (!c) {
            return std::nullopt;
        }
        return *c;
    };

    // 3. The values as display cells in the column's unit (the states set above stay).
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t ch = 0; ch < nch; ++ch) {
            const ReportChannelData& data = lookup(rows[r].source, ch);
            const std::optional<units::Converter> conv = converterOf(data, ch);
            Extremes extremes;
            for (const std::size_t c : statColumnsOf[ch]) {
                if (plan[c].stat == ReportStat::Min) {
                    extremes.min = values[r * nc + c];
                } else if (plan[c].stat == ReportStat::Max) {
                    extremes.max = values[r * nc + c];
                }
            }
            for (const std::size_t c : statColumnsOf[ch]) {
                ReportCell& cell = grid[r * nc + c];
                const std::optional<StatValue>& v = values[r * nc + c];
                if (cell.state != ReportCellState::Ok) {
                    continue;
                }
                if (!v || !v->valid) {
                    cell.state = ReportCellState::Blank;
                    continue;
                }
                if (!conv) {
                    cell.state = ReportCellState::Error; // the source's unit does not convert to the column's
                    continue;
                }
                cell.value = toDisplay(plan[c].stat, v->value, *conv, extremes);
            }
        }
    }

    // Delta columns: Main minus the Overlay 0 row of the same pairKey (D-197 (c)).
    if (spec.deltaVsOverlay0) {
        std::map<std::vector<std::string>, std::size_t> overlay0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].role == ReportSlotRole::Overlay0) {
                overlay0.try_emplace(rows[r].pairKey, r);
            }
        }
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const auto pair =
                rows[r].role == ReportSlotRole::Main ? overlay0.find(rows[r].pairKey) : overlay0.end();
            for (std::size_t c = 0; c < nc; ++c) {
                if (!plan[c].delta) {
                    continue;
                }
                ReportCell& cell = grid[r * nc + c];
                if (pair == overlay0.end()) {
                    cell.state = ReportCellState::Blank;
                    continue;
                }
                const ReportCell& m = grid[r * nc + plan[c].statColumn];
                const ReportCell& o = grid[pair->second * nc + plan[c].statColumn];
                if (m.state == ReportCellState::Pending || o.state == ReportCellState::Pending) {
                    cell.state = ReportCellState::Pending;
                } else if (m.state == ReportCellState::Ok && o.state == ReportCellState::Ok) {
                    cell.state = ReportCellState::Ok;
                    cell.value = m.value - o.value;
                } else {
                    cell.state = ReportCellState::Blank;
                }
            }
        }
    }

    // The table without the hidden rows and columns.
    ReportTable table;
    table.transposed = spec.layout == ReportLayout::ChannelsInColumns;
    std::vector<std::size_t> shown;
    for (std::size_t c = 0; c < nc; ++c) {
        const ColumnPlan& p = plan[c];
        if (p.hidden) {
            continue;
        }
        shown.push_back(c);
        ReportColumn column;
        column.channel = p.channel;
        column.stat = p.stat;
        column.deltaVsOverlay0 = p.delta;
        if (const ReportChannelData* unit = columnUnits[p.channel]) {
            column.unit = unit->displayUnit;
            column.dps = unit->dps;
        }
        table.columns.push_back(std::move(column));
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].hidden) {
            continue;
        }
        ReportHeading heading;
        heading.path = rows[r].path;
        heading.role = rows[r].role;
        heading.trusted = rows[r].trusted;
        heading.source = rows[r].source;
        table.rows.push_back(std::move(heading));
        for (const std::size_t c : shown) {
            table.cells.push_back(grid[r * nc + c]);
        }
    }
    markExtremes(table);
    return table;
}

} // namespace esses::analysis
