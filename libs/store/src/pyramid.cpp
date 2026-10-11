// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MinMaxPyramid queries and the SummaryPyramid (02 section 3.1, 04 sections 4.1, 4.5 and 4.8, D-012, D-054).
#include "descent.hpp"

#include <esses/store/pyramid.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

namespace esses::store {

namespace {

// Folds the valid raw samples [p, q) of a chunk (directional enclosure for wider dtypes).
template <class T>
void foldRaw(const Chunk& chunk, std::uint32_t p, std::uint32_t q, MinMax& acc,
             DescentStats* stats) noexcept {
    const T* s = reinterpret_cast<const T*>(chunk.data());
    const bool all = chunk.validity() == Validity::AllValid;
    if (stats != nullptr) {
        stats->rawReads += q - p;
    }
    if constexpr (std::is_same_v<T, float>) {
        for (std::uint32_t i = p; i < q; ++i) {
            if (all || chunk.validAt(i)) {
                acc.fold(s[i]);
            }
        }
    } else {
        bool any = false;
        T lo{};
        T hi{};
        for (std::uint32_t i = p; i < q; ++i) {
            if (!all && !chunk.validAt(i)) {
                continue;
            }
            if (!any) {
                lo = s[i];
                hi = s[i];
                any = true;
            } else {
                lo = foldMin(lo, s[i]);
                hi = foldMax(hi, s[i]);
            }
        }
        if (any) {
            acc.fold(encloseDirectional(static_cast<double>(lo), static_cast<double>(hi)));
        }
    }
}

void foldRawAny(const Chunk& chunk, std::uint32_t p, std::uint32_t q, MinMax& acc,
                DescentStats* stats) noexcept {
    switch (chunk.dtype()) {
    case Dtype::F32:
        foldRaw<float>(chunk, p, q, acc, stats);
        break;
    case Dtype::F64:
        foldRaw<double>(chunk, p, q, acc, stats);
        break;
    case Dtype::I32:
        foldRaw<std::int32_t>(chunk, p, q, acc, stats);
        break;
    case Dtype::I64:
        foldRaw<std::int64_t>(chunk, p, q, acc, stats);
        break;
    }
}

// Summary slice layout per chunk: pyramid levels 3..7 at these entry offsets (256 + 64 + 16 + 4 + 1 = 341).
constexpr std::array<std::uint32_t, kPyramidLevels - kSummaryFirstLevel> kSummaryOffset = {0, 256, 320, 336,
                                                                                           340};
constexpr unsigned kSummaryLevels = kPyramidLevels - kSummaryFirstLevel; // 5; the first one is the leaf level
constexpr std::uint32_t kLeavesPerChunk = kChunkSamples / kSummaryLeafSamples; // 256

} // namespace

MinMax chunkRangeMinMax(const Chunk& chunk, std::uint32_t begin, std::uint32_t end, int topLevel,
                        DescentStats* stats) noexcept {
    end = std::min(end, chunk.count());
    if (begin >= end || chunk.validity() == Validity::AllInvalid) {
        return MinMax::empty();
    }
    MinMax acc;
    if (!hasPyramid(chunk.dtype())) {
        foldRawAny(chunk, begin, end, acc, stats);
        return acc;
    }
    int level = topLevel == kAutoLevel ? descentLevel(end - begin) : topLevel;
    level = std::clamp(level, -1, static_cast<int>(kPyramidLevels) - 1);
    auto foldLeaves = [&](std::uint64_t p, std::uint64_t q) {
        foldRawAny(chunk, static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(q), acc, stats);
    };
    auto foldBuckets = [&](unsigned k, std::uint64_t j0, std::uint64_t j1) {
        const std::span<const MinMax> entries = chunk.level(k);
        for (std::uint64_t j = j0; j < j1; ++j) {
            acc.fold(entries[j]);
        }
        if (stats != nullptr) {
            stats->bucketReads += static_cast<std::uint32_t>(j1 - j0);
        }
    };
    detail::descend(begin, end, level, chunk.count(), foldLeaves, foldBuckets);
    return acc;
}

// ---- MinMaxPyramid
// -------------------------------------------------------------------------------------------

MinMax MinMaxPyramid::query(std::uint32_t first, std::uint32_t last) const noexcept {
    return query(first, last, kAutoLevel, nullptr);
}

MinMax MinMaxPyramid::query(std::uint32_t first, std::uint32_t last, int topLevel,
                            DescentStats* stats) const noexcept {
    if (chunk_->count() == 0 || first > last || first >= chunk_->count()) {
        return MinMax::empty();
    }
    last = std::min(last, chunk_->count() - 1);
    return chunkRangeMinMax(*chunk_, first, last + 1, topLevel, stats);
}

unsigned MinMaxPyramid::levels() const noexcept {
    return hasPyramid(chunk_->dtype()) ? kPyramidLevels : 0u;
}

// ---- SummaryPyramid
// ------------------------------------------------------------------------------------------

SummaryPyramid::SummaryPyramid(SampleIdx size, Dtype dtype, std::uint64_t version)
    : size_(std::max<SampleIdx>(size, 0)), chunks_(chunkCountFor(size)), version_(version), dtype_(dtype) {
    if (chunks_ > 0) {
        const auto n = static_cast<std::size_t>(chunks_);
        state_ = std::make_unique<std::atomic<std::uint8_t>[]>(n);
        if (hasPyramid(dtype_)) {
            entries_ = std::make_unique<MinMax[]>(n * kSummaryEntriesPerChunk); // empty sentinels
        } else {
            times_ = std::make_unique<TimeBounds[]>(n);
        }
    }
}

SummaryPyramid::~SummaryPyramid() = default;

bool SummaryPyramid::chunkFilled(SampleIdx chunkNo) const noexcept {
    return chunkNo >= 0 && chunkNo < chunks_ && state_[chunkNo].load(std::memory_order_acquire) == Filled;
}

bool SummaryPyramid::complete() const noexcept {
    return filled_.load(std::memory_order_acquire) == chunks_;
}

const MinMax* SummaryPyramid::slice(SampleIdx chunkNo) const noexcept {
    return entries_.get() + static_cast<std::size_t>(chunkNo) * kSummaryEntriesPerChunk;
}

std::size_t SummaryPyramid::bytes() const noexcept {
    const auto n = static_cast<std::size_t>(chunks_);
    return n * (entries_ ? kSummaryEntriesPerChunk * sizeof(MinMax) : sizeof(TimeBounds)) + n;
}

bool SummaryPyramid::fill(SampleIdx chunkNo, const Chunk& chunk) noexcept {
    if (chunkNo < 0 || chunkNo >= chunks_ || chunk.version() != version_ || chunk.dtype() != dtype_ ||
        chunk.count() != chunkSampleCount(size_, chunkNo)) {
        return false;
    }
    std::uint8_t expected = Empty;
    if (!state_[chunkNo].compare_exchange_strong(expected, Writing, std::memory_order_acq_rel)) {
        return false; // filled, or another thread is copying the same (identical) levels right now
    }
    if (entries_) {
        MinMax* dst = entries_.get() + static_cast<std::size_t>(chunkNo) * kSummaryEntriesPerChunk;
        for (unsigned s = 0; s < kSummaryLevels; ++s) {
            const std::span<const MinMax> level = chunk.level(kSummaryFirstLevel + s);
            std::copy(level.begin(), level.end(), dst + kSummaryOffset[s]);
        }
    } else {
        const std::span<const std::int64_t> t = chunk.samples<std::int64_t>();
        if (!t.empty()) {
            times_[chunkNo] = TimeBounds{t.front(), t.back()};
        }
    }
    state_[chunkNo].store(Filled, std::memory_order_release);
    filled_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

MinMax SummaryPyramid::minmax(SampleIdx first, SampleIdx last) const noexcept {
    return minmax(first, last, nullptr, nullptr);
}

MinMax SummaryPyramid::minmax(SampleIdx first, SampleIdx last, bool* covered,
                              DescentStats* stats) const noexcept {
    if (covered != nullptr) {
        *covered = true;
    }
    if (!entries_ || size_ == 0) {
        return MinMax::empty();
    }
    first = std::max<SampleIdx>(first, 0);
    last = std::min(last, size_ - 1);
    if (first > last) {
        return MinMax::empty();
    }
    // Column-wide leaf (256-sample bucket) span [g0, g1).
    const SampleIdx g0 = first / kSummaryLeafSamples;
    const SampleIdx g1 = last / kSummaryLeafSamples + 1;
    const SampleIdx c0 = g0 / kLeavesPerChunk;
    const SampleIdx c1 = (g1 - 1) / kLeavesPerChunk;
    MinMax acc;
    for (SampleIdx c = c0; c <= c1; ++c) {
        if (!chunkFilled(c)) {
            if (covered != nullptr) {
                *covered = false;
            }
            continue;
        }
        const std::uint64_t leafCount = levelBuckets(kSummaryFirstLevel, chunkSampleCount(size_, c));
        const SampleIdx base = c * kLeavesPerChunk;
        const auto a = static_cast<std::uint64_t>(std::max(g0, base) - base);
        const auto b = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(std::min(g1, base + kLeavesPerChunk) - base), leafCount);
        const MinMax* sl = slice(c);
        // Summary level s (0..3) is pyramid level 4 + s: buckets of 4^(s+1) leaves.
        int level =
            std::min(descentLevel(static_cast<std::int64_t>(b - a)), static_cast<int>(kSummaryLevels) - 2);
        auto foldLeaves = [&](std::uint64_t p, std::uint64_t q) {
            for (std::uint64_t j = p; j < q; ++j) {
                acc.fold(sl[kSummaryOffset[0] + j]);
            }
            if (stats != nullptr) {
                stats->bucketReads += static_cast<std::uint32_t>(q - p);
            }
        };
        auto foldBuckets = [&](unsigned s, std::uint64_t j0, std::uint64_t j1) {
            for (std::uint64_t j = j0; j < j1; ++j) {
                acc.fold(sl[kSummaryOffset[s + 1] + j]);
            }
            if (stats != nullptr) {
                stats->bucketReads += static_cast<std::uint32_t>(j1 - j0);
            }
        };
        detail::descend(a, b, level, leafCount, foldLeaves, foldBuckets);
    }
    return acc;
}

std::optional<MinMax> SummaryPyramid::bucket(unsigned level, SampleIdx index) const noexcept {
    if (!entries_ || level < kSummaryFirstLevel || level >= kPyramidLevels || index < 0) {
        return std::nullopt;
    }
    const SampleIdx perChunk = kChunkSamples / bucketSize(level);
    const SampleIdx c = index / perChunk;
    const SampleIdx local = index % perChunk;
    if (!chunkFilled(c) || local >= levelBuckets(level, chunkSampleCount(size_, c))) {
        return std::nullopt;
    }
    return slice(c)[kSummaryOffset[level - kSummaryFirstLevel] + static_cast<std::size_t>(local)];
}

std::optional<SummaryPyramid::TimeBounds> SummaryPyramid::chunkTimeBounds(SampleIdx chunkNo) const noexcept {
    if (!times_ || !chunkFilled(chunkNo)) {
        return std::nullopt;
    }
    return times_[chunkNo];
}

} // namespace esses::store
