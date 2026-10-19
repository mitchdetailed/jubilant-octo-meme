// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The open-time resident estimate (resident_estimate.hpp; 07 section 7.1, AC-M4-02; 01 sections 7.1-7.2).
// Root of the M4 budget package in esses_formats (docs/milestones/M4.md section 3). Every chunk is counted as
// store::ChunkBuilder::allocate lays it out with a validity bitmap (the upper bound: an AllValid chunk
// carries none), so the estimate never falls below what ChunkCache::Stats::residentBytes reports once every
// chunk of the source is materialised; an I64 time column takes neither a bitmap nor a pyramid (02
// section 3.1).

#include <esses/formats/resident_estimate.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/series.hpp>

#include <set>
#include <variant>

namespace esses::formats {

namespace {

// One chunk of `count` samples (1 <= count <= kChunkSamples).
ResidentEstimate estimateChunk(store::Dtype dtype, std::uint32_t count) {
    const bool bitmap = dtype != store::Dtype::I64;
    ResidentEstimate e;
    e.sampleBytes = store::Chunk::sampleBytes(count, dtype);
    e.bitmapBytes = bitmap ? store::Chunk::bitmapBytes(count) : 0U;
    e.pyramidBytes = store::hasPyramid(dtype)
                         ? static_cast<std::uint64_t>(store::pyramidEntries(count)) * sizeof(store::MinMax)
                         : 0U;
    const std::uint64_t layout = store::Chunk::layoutBytes(count, dtype, bitmap);
    // The header and the rounding of the layout to 64 bytes (samples and bitmap are padded already).
    e.overheadBytes = store::kChunkHeaderBytes + (layout - e.sampleBytes - e.bitmapBytes - e.pyramidBytes);
    e.chunks = 1;
    return e;
}

// Every chunk of a column of `sampleCount` samples, without the channel count: the full chunks are counted
// once and multiplied.
ResidentEstimate estimateColumn(store::Dtype dtype, std::uint64_t sampleCount) {
    ResidentEstimate total;
    if (sampleCount == 0) {
        return total;
    }
    const std::uint64_t full = sampleCount / store::kChunkSamples;
    const auto rest = static_cast<std::uint32_t>(sampleCount % store::kChunkSamples);
    if (full > 0) {
        const ResidentEstimate one = estimateChunk(dtype, store::kChunkSamples);
        total.sampleBytes = one.sampleBytes * full;
        total.pyramidBytes = one.pyramidBytes * full;
        total.bitmapBytes = one.bitmapBytes * full;
        total.overheadBytes = one.overheadBytes * full;
        total.chunks = full;
    }
    if (rest > 0) {
        total += estimateChunk(dtype, rest);
    }
    return total;
}

// A column of an opened source, once: copies of one column (aliases, a shared time column) share its chunks.
void addColumn(ResidentEstimate& total, std::set<std::uint64_t>& seen, const store::Column& column) {
    if (!column.valid() || column.size() <= 0 || !seen.insert(column.id()).second) {
        return;
    }
    total += estimateColumn(column.dtype(), static_cast<std::uint64_t>(column.size()));
}

} // namespace

ResidentEstimate estimateChannel(store::Dtype dtype, std::uint64_t sampleCount) {
    ResidentEstimate e = estimateColumn(dtype, sampleCount);
    e.channels = 1;
    return e;
}

ResidentEstimate estimateResident(const ProbeResult& probe) {
    ResidentEstimate total;
    for (const LayerInfo& layer : probe.layers) {
        for (const ChannelInfo& channel : layer.channels) {
            total += estimateChannel(channel.dtype, channel.sampleCount);
            if (std::holds_alternative<Timestamped>(channel.timing)) {
                // Its exact microsecond times: an I64 column of the same length (D-062).
                total += estimateColumn(store::Dtype::I64, channel.sampleCount);
            }
        }
    }
    return total;
}

ResidentEstimate estimateResident(const model::DataSource& source) {
    ResidentEstimate total;
    std::set<std::uint64_t> seen;
    for (const model::DataLayer& layer : source.layers) {
        for (const model::Channel& channel : layer.channels) {
            if (channel.desc.source != model::ChannelSource::Logged) {
                continue; // maths, details, aliases: not known at open or not stored as chunks
            }
            const std::uint64_t before = total.chunks;
            if (const auto* uniform = std::get_if<store::UniformSeries>(&channel.series)) {
                addColumn(total, seen, uniform->values);
            } else if (const auto* stamped = std::get_if<store::TimestampedSeries>(&channel.series)) {
                addColumn(total, seen, stamped->values);
                addColumn(total, seen, stamped->timesUs);
            }
            if (total.chunks != before) {
                ++total.channels;
            }
        }
    }
    return total;
}

} // namespace esses::formats
