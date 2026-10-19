// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ResidentEstimate (M4 contract, D-206; 07 section 7.1 "SourceLoader warns ... when the probe estimate (raw +
// pyramids) exceeds the budget", AC-M4-02; 01 sections 7.1-7.2): what the chunk cache would hold if every
// logged channel of a source were resident, computed from the probe (or from the skeleton DataSource the open
// pipeline builds from it) without decoding a sample. The figures follow the chunk layout of esses_store
// (store::Chunk::layoutBytes: 65,536-sample chunks, samples padded to 64 bytes, a validity bitmap counted for
// every chunk as an upper bound, the factor-4 pyramid of float pairs, the chunk header). The budget package's
// tests hold the estimate of a synthetic source against ChunkCache::Stats::residentBytes once every chunk of
// it is materialised (an AllValid chunk carries no bitmap, so the estimate may exceed it by at most
// store::kBitmapBytes per chunk). Counted as the cache counts against the budget (01 section 7.2): logged
// channels only (maths columns are not known at open), the SummaryPyramid excluded.
//
// An I64 time column (the exact times of a timestamped channel) counts its samples and header only: it has no
// bitmap and no pyramid. Owner: the M4 budget package (docs/milestones/M4.md section 3; defined in
// src/m4_budget/resident_estimate.cpp, tested by tests/m4/budget_*.cpp).
#pragma once

#include <esses/formats/probe.hpp>
#include <esses/model/source.hpp>
#include <esses/store/types.hpp>

#include <cstdint>

namespace esses::formats {

struct ResidentEstimate {
    std::uint64_t sampleBytes = 0;   // raw samples in the storage dtype, each chunk padded to 64 bytes
    std::uint64_t pyramidBytes = 0;  // the chunks' factor-4 MinMax pyramids (float pairs for every dtype)
    std::uint64_t bitmapBytes = 0;   // validity bitmaps (one per chunk, the upper bound)
    std::uint64_t overheadBytes = 0; // chunk headers and the rounding of each chunk's layout to 64 bytes
    std::uint64_t channels = 0;      // channels counted
    std::uint64_t chunks = 0;        // chunks counted

    // Final (D-206): every byte the cache would count.
    std::uint64_t total() const noexcept { return sampleBytes + pyramidBytes + bitmapBytes + overheadBytes; }
    ResidentEstimate& operator+=(const ResidentEstimate& other) noexcept {
        sampleBytes += other.sampleBytes;
        pyramidBytes += other.pyramidBytes;
        bitmapBytes += other.bitmapBytes;
        overheadBytes += other.overheadBytes;
        channels += other.channels;
        chunks += other.chunks;
        return *this;
    }
    friend bool operator==(const ResidentEstimate&, const ResidentEstimate&) = default;
};

// One channel of `sampleCount` samples stored as `dtype` (channels = 1; 0 samples estimate 0 bytes).
ResidentEstimate estimateChannel(store::Dtype dtype, std::uint64_t sampleCount);

// Every channel of every layer of a probe (05 section 1.2: ChannelInfo::dtype and sampleCount); a timestamped
// channel adds its I64 time column.
ResidentEstimate estimateResident(const ProbeResult& probe);

// Every logged channel (ChannelSource::Logged) of an opened source: the columns' sizes and dtypes as the open
// pipeline built them (step 3, no sample decoded), which is what SourceLoader checks before it publishes. A
// timestamped channel adds its time column; a column several channels share counts once (Column::id).
ResidentEstimate estimateResident(const model::DataSource& source);

} // namespace esses::formats
