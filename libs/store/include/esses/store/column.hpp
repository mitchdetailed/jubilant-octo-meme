// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Column (02 section 3.2 behaviours 6-11, D-011, D-123; 01 section 5.1 Rule W): a lazily materialised
// sequence of chunks fed by an IChunkProvider and held in a ChunkCache.
//
// Column is a cheap, copyable handle on shared, internally synchronised state: copies (DataSource snapshots,
// Series values) share the same chunks and summary. Every chunk slot has the state {Absent, Building(owner),
// Sealed}; a chunk is materialised by at most one thread at a time:
//   tryPin/prefetch  any thread, never block: pin a resident current chunk, or schedule a `chunk/` task at
//   the
//                    given priority (coalesced: a chunk already scheduled or being built is not scheduled
//                    twice; a higher priority raises the queued task).
//   require          worker threads (never a NoBlock thread): pins [first, last]; an absent chunk is built
//                    inline on the calling thread (Rule W (a)), a still-queued `chunk/` task of the span is
//                    run inline through TaskPool::tryRunInline, and the caller waits only for a chunk another
//                    running thread is building. Chunk-independent providers first schedule the other absent
//                    chunks of the span for parallelism; sequential providers are materialised 0..last in
//                    order.
//   buildSummary     worker: fills the SummaryPyramid without inserting chunks into the cache.
// A provider version bump makes every cached chunk stale: stale chunks are treated as absent and freed by the
// cache at pin count zero (behaviour 9).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/chunk_provider.hpp>
#include <esses/store/pyramid.hpp>
#include <esses/store/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::store {

struct ColumnOptions {
    // nullptr: ChunkCache::instance() at construction (asserts when no runtime cache is installed).
    ChunkCache* cache = nullptr;
    // nullptr: TaskPool::installed() when a chunk must be scheduled; with no pool, tryPin/prefetch only
    // report absence and require builds everything inline.
    TaskPool* pool = nullptr;
    // Task key prefix of this column's chunk tasks, "chunk/<sourceId>/<layer>/<channelIx>" (01 section 5.4),
    // so cancelAll("chunk/<sourceId>") stops them when the source closes. Must start with "chunk/". Empty:
    // "chunk/column-<id>".
    std::string taskKeyPrefix{};
    // Called on the sealing thread, outside every lock, after a chunk of this column was sealed into the
    // cache (the ChannelReady hook of 01 section 5; the GUI wraps it in Dispatcher::post). Must not block or
    // throw.
    std::function<void(SampleIdx chunkNo)> onChunkSealed{};
};

class Column {
public:
    Column() noexcept = default; // the empty column: no provider, size 0
    explicit Column(std::shared_ptr<IChunkProvider> provider);
    Column(std::shared_ptr<IChunkProvider> provider, ColumnOptions options);

    bool valid() const noexcept { return core_ != nullptr; }
    SampleIdx size() const noexcept;
    Dtype dtype() const noexcept;
    std::uint64_t version() const noexcept; // provider version
    SampleIdx chunkCount() const noexcept;
    bool chunkIndependent() const noexcept;
    std::string debugName() const;
    std::uint64_t id() const noexcept; // process-unique, never reused
    TaskKey chunkTaskKey(SampleIdx chunkNo) const;

    // Any thread, never blocks: pins the chunk if it is resident and current, else schedules its
    // materialisation at `prio` and returns nullopt (behaviour 6).
    std::optional<ChunkHandle> tryPin(SampleIdx chunkNo, Priority prio) const;
    // Any thread, never blocks: schedules materialisation of an absent chunk at `prio` without pinning it.
    void prefetch(SampleIdx chunkNo, Priority prio) const;
    // Worker threads only (asserted; on a NoBlock thread it fails with Invalid in every build): returns the
    // inclusive sample range [first, last], clamped to the column, resident and pinned; absent chunks are
    // materialised inline (behaviour 7, Rule W). Honours `cancel`. Invalid on an empty clamped range.
    Result<ChunkHandle> require(SampleIdx first, SampleIdx last, Priority prio,
                                const CancelToken& cancel) const;
    // tryPin(chunk of i, Visible) -> read -> unpin; NaN when absent, invalid or out of range.
    double at(SampleIdx i) const noexcept;
    // True when the chunk is sealed, current and resident (no pin, no scheduling).
    bool isResident(SampleIdx chunkNo) const noexcept;

    // The summary of the current provider version; may be incomplete (SummaryPyramid::complete()). The
    // reference stays valid for the Column's lifetime; after a version bump it refers to the superseded
    // summary (compare version()), so long-lived readers should prefer summarySnapshot().
    const SummaryPyramid& summary() const;
    std::shared_ptr<const SummaryPyramid> summarySnapshot() const;
    // Worker: streams the provider chunk by chunk into a scratch ChunkBuilder, folds the levels >= 256 into
    // the SummaryPyramid and discards the samples without inserting them into the ChunkCache (a resident
    // chunk is read pinned instead). Never fills the cache (behaviour 10).
    Result<void> buildSummary(const CancelToken& cancel) const;

    // Same shared column.
    friend bool operator==(const Column& a, const Column& b) noexcept { return a.core_ == b.core_; }

private:
    friend class ChunkCache;
    std::shared_ptr<detail::ColumnCore> core_;
};

} // namespace esses::store
