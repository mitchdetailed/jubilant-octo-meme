// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/store/range_query.hpp>

#include <algorithm>

namespace esses::store {

namespace {

// Clamps [begin, end) to what the handle can hold: below 0 nothing exists, and when the last pinned chunk is
// partial it is the column's last chunk, so nothing exists past it.
bool clampToPinned(const ChunkHandle& pinned, SampleIdx& begin, SampleIdx& end) noexcept {
    begin = std::max<SampleIdx>(begin, 0);
    if (!pinned.empty()) {
        const Chunk* last = pinned.chunk(pinned.lastChunk());
        if (last != nullptr && last->count() < kChunkSamples) {
            end = std::min(end, pinned.endSample());
        }
    }
    return begin < end;
}

} // namespace

std::optional<MinMax> rangeMinMax(const ChunkHandle& pinned, SampleIdx begin, SampleIdx end, int topLevel,
                                  DescentStats* stats) noexcept {
    if (!clampToPinned(pinned, begin, end)) {
        return MinMax::empty();
    }
    MinMax acc;
    for (SampleIdx c = chunkOf(begin); c <= chunkOf(end - 1); ++c) {
        const Chunk* chunk = pinned.chunk(c);
        if (chunk == nullptr) {
            return std::nullopt;
        }
        const SampleIdx base = chunkFirstSample(c);
        const auto lo = static_cast<std::uint32_t>(std::max(begin, base) - base);
        const auto hi = static_cast<std::uint32_t>(std::min(end, base + SampleIdx{kChunkSamples}) - base);
        acc.fold(chunkRangeMinMax(*chunk, lo, hi, topLevel, stats));
    }
    return acc;
}

std::optional<bool> rangeHasInvalid(const ChunkHandle& pinned, SampleIdx begin, SampleIdx end) noexcept {
    if (!clampToPinned(pinned, begin, end)) {
        return false;
    }
    bool any = false;
    for (SampleIdx c = chunkOf(begin); c <= chunkOf(end - 1); ++c) {
        const Chunk* chunk = pinned.chunk(c);
        if (chunk == nullptr) {
            return std::nullopt;
        }
        const SampleIdx base = chunkFirstSample(c);
        const auto lo = static_cast<std::uint32_t>(std::max(begin, base) - base);
        const auto hi = static_cast<std::uint32_t>(std::min(end, base + SampleIdx{kChunkSamples}) - base);
        any = any || chunk->anyInvalid(lo, hi);
    }
    return any;
}

} // namespace esses::store
