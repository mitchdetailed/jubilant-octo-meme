// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MinMaxPyramid (per chunk) and SummaryPyramid (per column) with the hierarchical boundary descent of D-012
// and 04 section 4.5: whole buckets of the starting level are folded directly, and each unaligned edge is
// refined one level down at a time (at most 3 whole buckets per level and edge) until raw samples (chunk
// pyramid) or 256-sample buckets (summary) remain.
//
// Exactness. For F32 the result equals the brute-force min/max over the valid samples bit for bit. For F64
// and I32 the result is the brute-force min/max rounded directionally to float (lo toward -inf, hi toward
// +inf): directional rounding is monotone, so it commutes with min/max and every decomposition of a span
// gives the same bits. That is the "conservative enclosure within the directional rounding" of 02
// section 3.1, D-054. The folds order -0 below +0 (types.hpp), so zero signs do not depend on the
// decomposition either.
#pragma once

#include <esses/store/chunk.hpp>
#include <esses/store/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace esses::store {

// Min/max over the half-open chunk-local span [begin, end) (clamped to the chunk). The descent starts at
// `topLevel`: kAutoLevel picks descentLevel(end - begin) (the cheapest exact decomposition), kRawOnly reads
// every sample, and exact M4 passes descentLevel(n, 8) (04 section 4.5). Empty span or no valid sample: the
// empty sentinel.
MinMax chunkRangeMinMax(const Chunk& chunk, std::uint32_t begin, std::uint32_t end, int topLevel = kAutoLevel,
                        DescentStats* stats = nullptr) noexcept;

// View over one sealed chunk's levels (built by ChunkBuilder::seal()).
class MinMaxPyramid {
public:
    explicit MinMaxPyramid(const Chunk& chunk) noexcept : chunk_(&chunk) {}

    // Inclusive chunk-local span [first, last], clamped to the chunk; first > last gives the empty sentinel.
    // Hierarchical descent (D-012): exact for F32, the directionally rounded exact result for F64 and I32.
    MinMax query(std::uint32_t first, std::uint32_t last) const noexcept;
    MinMax query(std::uint32_t first, std::uint32_t last, int topLevel, DescentStats* stats) const noexcept;
    // kPyramidLevels, or 0 for an I64 chunk.
    unsigned levels() const noexcept;
    std::span<const MinMax> level(unsigned k) const noexcept { return chunk_->level(k); }

private:
    const Chunk* chunk_;
};

// The levels with bucket >= 256 (kSummaryFirstLevel and up) for a whole column, ~1 % of raw, always resident
// (D-011, 02 section 3.1). One object per (column, provider version): Column::buildSummary and every chunk
// the Column seals fill its per-chunk slices; a provider version bump gives the Column a fresh object. Slices
// are written once (first writer wins) and published with release ordering, so readers on any thread see
// either a complete slice or none. I64 time columns have no pyramid; their summary records each chunk's first
// and last timestamp instead (used by the non-blocking Resampler lookups).
class SummaryPyramid {
public:
    SummaryPyramid(SampleIdx size, Dtype dtype, std::uint64_t version);
    ~SummaryPyramid();
    SummaryPyramid(const SummaryPyramid&) = delete;
    SummaryPyramid& operator=(const SummaryPyramid&) = delete;

    // Inclusive sample span [first, last] at 256-sample bucket granularity, clamped to the column: every
    // bucket that intersects the span is folded, so the result encloses the exact min/max and equals it when
    // first is a multiple of 256 and last + 1 is a multiple of 256 or the column size. Unfilled chunks are
    // skipped (see the `covered` overload). Always the empty sentinel for I64 columns.
    MinMax minmax(SampleIdx first, SampleIdx last) const noexcept;
    // `covered` (optional) is set false when an unfilled chunk intersects the span.
    MinMax minmax(SampleIdx first, SampleIdx last, bool* covered,
                  DescentStats* stats = nullptr) const noexcept;
    // False while chunks are still unfilled (the background build is running).
    bool complete() const noexcept;
    std::uint64_t version() const noexcept { return version_; }

    SampleIdx size() const noexcept { return size_; }
    Dtype dtype() const noexcept { return dtype_; }
    SampleIdx chunkCount() const noexcept { return chunks_; }
    SampleIdx filledChunks() const noexcept { return filled_.load(std::memory_order_acquire); }
    bool chunkFilled(SampleIdx chunkNo) const noexcept;
    // Bucket `index` (column-wide numbering) of pyramid level `level` (kSummaryFirstLevel..kPyramidLevels-1);
    // nullopt when the level is not a summary level, the bucket is past the column end or its chunk is
    // unfilled.
    std::optional<MinMax> bucket(unsigned level, SampleIdx index) const noexcept;
    // I64 columns: first and last timestamp of a filled chunk.
    struct TimeBounds {
        std::int64_t first = 0;
        std::int64_t last = 0;
    };
    std::optional<TimeBounds> chunkTimeBounds(SampleIdx chunkNo) const noexcept;
    // Heap bytes held (not counted against the ChunkCache budget, 01 section 7.2).
    std::size_t bytes() const noexcept;

    // Copies the summary levels (or, for I64, the time bounds) of a sealed chunk of this column and version.
    // Returns true when this call filled the slice; false when it was already filled or being filled, or when
    // the chunk does not belong here (wrong number, count, dtype or version).
    bool fill(SampleIdx chunkNo, const Chunk& chunk) noexcept;

private:
    enum SliceState : std::uint8_t { Empty = 0, Writing = 1, Filled = 2 };

    const MinMax* slice(SampleIdx chunkNo) const noexcept;

    SampleIdx size_;
    SampleIdx chunks_;
    std::uint64_t version_;
    Dtype dtype_;
    std::unique_ptr<MinMax[]> entries_;   // chunks_ * kSummaryEntriesPerChunk (no I64)
    std::unique_ptr<TimeBounds[]> times_; // I64 only
    std::unique_ptr<std::atomic<std::uint8_t>[]> state_;
    std::atomic<SampleIdx> filled_{0};
};

} // namespace esses::store
