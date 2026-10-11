// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IChunkProvider (02 section 3.2, D-011): the source of a Column's chunks. Implementations: LdChunkProvider
// and ImporterChunkProvider (esses_formats), PlanProvider (esses_maths), later DcfProvider and LiveProvider;
// InMemoryProvider (memory_provider.hpp) for small in-memory columns and tests.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/types.hpp>

#include <cstdint>
#include <string>

namespace esses::store {

struct IChunkProvider {
    IChunkProvider() = default;
    IChunkProvider(const IChunkProvider&) = delete;
    IChunkProvider& operator=(const IChunkProvider&) = delete;
    virtual ~IChunkProvider() = default;

    // Fixed for the provider's lifetime (a Column reads them once).
    virtual SampleIdx size() const = 0;
    virtual Dtype dtype() const = 0;
    // Monotonic; a bump invalidates every chunk (maths definition change, source change). Called under the
    // cache shard lock: it must be cheap, must not block and must not call back into the store.
    virtual std::uint64_t version() const = 0;
    // true: any chunk can be materialised alone; false: sequential (stateful maths), chunks 0..n in order.
    virtual bool chunkIndependent() const = 0;
    // Thread-safe. Fills `out` with chunk `chunkNo`: out.allocate(chunkSampleCount(size(), chunkNo), dtype(),
    // wantBitmap) (or allocateAllInvalid for a chunk that cannot be decoded), writes the samples, clears the
    // valid bits of invalid samples. The Column validates count and dtype and seals the chunk. Honours
    // `cancel` at block granularity; must not throw.
    virtual Result<void> materialise(SampleIdx chunkNo, ChunkBuilder& out, CancelToken cancel) = 0;
    // "ld:Engine RPM", "plan:Corr Dist@v12"; logs and errors only.
    virtual std::string debugName() const = 0;
};

} // namespace esses::store
