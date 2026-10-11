// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal state shared by ChunkCache, ChunkHandle and Column (not installed).
//
// Locking. The cache has kShards shards, each a mutex + condition variable + the list of its live entries. A
// column belongs to one shard; its chunk slots, the pin counts of its entries and the free path are all
// guarded by that shard's mutex. No TaskPool call, provider materialise() or user hook ever runs under a
// shard lock (provider version() does, which is why it must be cheap). Lock order: shard mutex -> summary
// mutex is never nested the other way; the summary mutex only guards the summary pointer.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/pyramid.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace esses::store {

namespace detail {

class ColumnCore;

enum class SlotState : std::uint8_t { Absent, Building, Sealed };

struct CacheEntry {
    OwnedChunk chunk;
    std::size_t bytes = 0;
    std::uint32_t pins = 0;
    bool current = true;         // false: retired (stale, or its column is gone); freed at pin count zero
    ColumnCore* owner = nullptr; // nullptr once retired
    SampleIdx chunkNo = 0;
    std::uint64_t group = 0; // insertGroup() group id, 0 for a single chunk (M5 evicts groups as a unit)
    CacheEntry* prev = nullptr;
    CacheEntry* next = nullptr;
};

struct Slot {
    std::atomic<SlotState> state{SlotState::Absent};
    std::thread::id builder; // valid while Building
    CacheEntry* entry = nullptr;
    bool scheduled = false; // a chunk/ task was submitted and has not resolved yet
    Priority scheduledPrio = Priority::Background;
    std::uint32_t scheduleGen = 0;
};

struct Shard {
    std::mutex mutex;
    std::condition_variable cv; // notified whenever a chunk of the shard leaves the Building state
    CacheEntry* head = nullptr;
};

} // namespace detail

class ChunkCache::Impl {
public:
    static constexpr std::size_t kShards = 16;

    explicit Impl(std::size_t budgetBytes) : budget(budgetBytes) {}

    detail::CacheEntry* insertLocked(detail::Shard& shard, OwnedChunk chunk, detail::ColumnCore* owner,
                                     SampleIdx chunkNo, std::uint64_t group);
    void pinLocked(detail::CacheEntry* e) noexcept;
    void unpinLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept;
    // The entry stops being current; freed now when unpinned, else at its last unpin.
    void retireLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept;
    // The single free path for chunk memory (01 section 5.3 rule 5): shard lock held, pin count zero.
    void freeLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept;
    // Budget warning check after an insert; outside every lock.
    void afterInsert();
    void fireWarning(std::size_t resident, std::size_t budgetBytes);

    std::array<detail::Shard, kShards> shards;
    std::atomic<std::size_t> budget;
    std::atomic<std::size_t> resident{0};
    std::atomic<std::size_t> pinned{0};
    std::atomic<std::size_t> chunks{0};
    std::atomic<std::size_t> pinnedChunks{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> groupCounter{0};
    std::atomic<std::int64_t> liveColumns{0};
    std::atomic<std::int64_t> warnedStep{-1}; // highest 10 % step reported; -1 = under budget
    std::mutex hookMutex;
    BudgetWarningHook hook;
};

namespace detail {

class ColumnCore : public std::enable_shared_from_this<ColumnCore> {
public:
    ColumnCore(std::shared_ptr<IChunkProvider> provider, ColumnOptions options);
    ~ColumnCore();
    ColumnCore(const ColumnCore&) = delete;
    ColumnCore& operator=(const ColumnCore&) = delete;

    struct EnsureMode {
        bool pin = false;       // pin the sealed entry for the caller
        bool wait = false;      // wait for another running builder (false: report "busy" as nullptr)
        bool runQueued = false; // run a still-queued chunk/ task of this chunk inline first (Rule W)
    };
    // Makes chunk n sealed and current: hit, wait for a running builder, run a queued task inline, or build
    // inline on this thread. Returns the entry (pinned when mode.pin), or nullptr when !mode.wait and another
    // thread is building it.
    Result<CacheEntry*> ensure(SampleIdx n, EnsureMode mode, const CancelToken& cancel);
    // Pins a resident current chunk; otherwise optionally schedules it and returns nullopt. Never blocks.
    std::optional<ChunkHandle> tryPin(SampleIdx n, Priority prio, bool scheduleIfAbsent);
    void schedule(SampleIdx n, Priority prio);
    bool isResident(SampleIdx n);
    Result<ChunkHandle> require(SampleIdx first, SampleIdx last, Priority prio, const CancelToken& cancel);
    Result<void> buildSummary(const CancelToken& cancel);
    // The summary of provider version v (a fresh one after a bump; the newest one if v is older).
    std::shared_ptr<SummaryPyramid> summaryFor(std::uint64_t v);
    TaskKey keyFor(SampleIdx n) const;
    // Cache side: drop the slot of an entry that clear() frees (shard lock held).
    void dropSlotLocked(SampleIdx n, const CacheEntry* e) noexcept;

    std::shared_ptr<IChunkProvider> provider;
    ChunkCache* cache = nullptr;
    ChunkCache::Impl* impl = nullptr;
    std::uint32_t shardIx = 0;
    Shard* shard = nullptr;
    TaskPool* pool = nullptr;
    std::string keyPrefix;
    // Sequential providers only: held around every materialise() call (ensure() builders and the
    // buildSummary() pass), never together with a shard lock.
    std::mutex sequentialMutex;
    std::uint64_t id = 0;
    SampleIdx size = 0;
    SampleIdx chunks = 0;
    Dtype dtype = Dtype::F32;
    bool independent = true;
    std::function<void(SampleIdx)> onSealed;

private:
    Result<OwnedChunk> materialiseOne(SampleIdx n, std::uint64_t version, ChunkBuilder& builder,
                                      const CancelToken& cancel);
    Result<void> checkBuilt(const ChunkBuilder& b, SampleIdx n) const;
    TaskPool* taskPool() const noexcept;
    void clearScheduled(SampleIdx n, std::uint32_t gen);
    void notifySealed(SampleIdx n) noexcept;
    ChunkHandle makeHandle(SampleIdx firstChunk, std::uint32_t count);
    static void setPin(ChunkHandle& h, std::uint32_t k, CacheEntry* e) noexcept;

    friend class esses::store::ChunkCache;

    std::unique_ptr<Slot[]> slots_;
    std::mutex summaryMutex_;
    std::shared_ptr<SummaryPyramid> summary_;
    std::vector<std::shared_ptr<SummaryPyramid>> retiredSummaries_; // keeps summary() references valid
};

} // namespace detail

} // namespace esses::store
