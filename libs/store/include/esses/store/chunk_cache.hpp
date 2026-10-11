// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChunkCache and ChunkHandle (02 section 3.2, D-011, D-043, D-123; 01 section 5.3 rules 5 and 11,
// section 7.2).
//
// The cache owns every chunk a Column materialises. A chunk is read only through a ChunkHandle pin: the pin
// is a reference-count increment under the cache shard lock, and chunk memory is freed on exactly one path,
// under that lock, for a chunk whose pin count is zero (stale drop after a provider version bump, the release
// of a destroyed Column's chunks and clear() today; LRU eviction from M5, D-043). M0-M4 never evict: the
// cache tracks resident bytes against the budget, raises Stats::overBudget and calls the budget-warning hook
// once per 10 % step above the budget (01 section 7.2; the GUI turns it into a Messages row).
#pragma once

#include <esses/base/assert.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace esses::store {

class ChunkCache;
class Column;

namespace detail {
struct CacheEntry;
class ColumnCore;
} // namespace detail

// RAII pin over the chunks [firstChunk, lastChunk] of one column; move-only. A pinned chunk is never freed
// (01 section 5.3 rule 5). A handle may outlive its Column but not the ChunkCache.
class ChunkHandle {
public:
    ChunkHandle() noexcept = default;
    ChunkHandle(ChunkHandle&& other) noexcept;
    ChunkHandle& operator=(ChunkHandle&& other) noexcept; // unpins the old span first
    ChunkHandle(const ChunkHandle&) = delete;
    ChunkHandle& operator=(const ChunkHandle&) = delete;
    ~ChunkHandle(); // unpins

    bool empty() const noexcept { return count_ == 0; }
    // nullptr outside the pinned span.
    const Chunk* chunk(SampleIdx chunkNo) const noexcept {
        const SampleIdx k = chunkNo - first_;
        return (k >= 0 && k < static_cast<SampleIdx>(count_)) ? pins()[k].chunk : nullptr;
    }
    template <class T> std::span<const T> samples(SampleIdx chunkNo) const noexcept {
        const Chunk* c = chunk(chunkNo);
        return c != nullptr ? c->samples<T>() : std::span<const T>{};
    }
    // Column sample i as double; NaN when invalid or outside the pinned span. Any dtype.
    double at(SampleIdx i) const noexcept {
        const Chunk* c = chunk(chunkOf(i));
        return c != nullptr ? c->valueAt(chunkLocal(i)) : std::numeric_limits<double>::quiet_NaN();
    }
    // Exact sample of an I64 (time) column; nullopt outside the pinned span or for another dtype.
    std::optional<std::int64_t> int64At(SampleIdx i) const noexcept;
    bool validAt(SampleIdx i) const noexcept {
        const Chunk* c = chunk(chunkOf(i));
        return c != nullptr && c->validAt(chunkLocal(i));
    }
    // True when sample i lies in a pinned chunk (i may still be past the column end in the last chunk).
    bool contains(SampleIdx i) const noexcept { return chunk(chunkOf(i)) != nullptr; }
    SampleIdx firstChunk() const noexcept { return first_; }
    SampleIdx lastChunk() const noexcept { return first_ + static_cast<SampleIdx>(count_) - 1; }
    // Pinned sample range [firstSample(), endSample()) (the last chunk may be partial).
    SampleIdx firstSample() const noexcept { return chunkFirstSample(first_); }
    SampleIdx endSample() const noexcept;
    // Releases the pins now.
    void reset() noexcept;

private:
    friend class ChunkCache;
    friend class detail::ColumnCore;

    struct Pin {
        detail::CacheEntry* entry = nullptr;
        const Chunk* chunk = nullptr;
    };
    static constexpr std::uint32_t kInlinePins = 2;

    ChunkHandle(ChunkCache* cache, std::uint32_t shard, SampleIdx firstChunk, std::uint32_t count);
    Pin* pins() noexcept { return heap_ ? heap_.get() : inline_.data(); }
    const Pin* pins() const noexcept { return heap_ ? heap_.get() : inline_.data(); }

    ChunkCache* cache_ = nullptr;
    std::uint32_t shard_ = 0;
    std::uint32_t count_ = 0;
    SampleIdx first_ = 0;
    std::array<Pin, kInlinePins> inline_{};
    std::unique_ptr<Pin[]> heap_;
};

// Process-wide chunk cache (D-011). Owned by the runtime (01 section 5.3 rule 11): instance() returns the
// cache the runtime installed and aborts when there is none; there is no function-local static. Tests
// construct their own and pass it through ColumnOptions.
class ChunkCache {
public:
    ChunkCache();                                 // budget = defaultBudgetBytes()
    explicit ChunkCache(std::size_t budgetBytes); // e.g. a small budget in tests
    ~ChunkCache();                                // frees every chunk; asserts that nothing is pinned
    ChunkCache(const ChunkCache&) = delete;
    ChunkCache& operator=(const ChunkCache&) = delete;

    static ChunkCache& instance();
    static ChunkCache* installed() noexcept;
    static void install(ChunkCache* cache) noexcept; // nullptr uninstalls
    // max(1 GiB, 25 % of physical RAM) (D-011, 01 section 7.2).
    static std::size_t defaultBudgetBytes() noexcept;

    void setBudgetBytes(std::size_t bytes);
    std::size_t budgetBytes() const noexcept;

    struct Stats {
        std::size_t residentBytes = 0; // every live chunk allocation, pinned or not, current or stale
        std::size_t pinnedBytes = 0;
        std::size_t chunks = 0;
        std::size_t pinnedChunks = 0;
        std::uint64_t hits = 0;      // tryPin/require lookups that found a current sealed chunk
        std::uint64_t misses = 0;    // lookups that had to build, wait or schedule
        std::uint64_t evictions = 0; // LRU evictions (M5; always 0 before)
        bool overBudget = false;     // residentBytes > budgetBytes
    };
    Stats stats() const;

    // Called once per 10 % step above the budget (100 %, 110 %, ...) on the thread whose insert crossed the
    // step, outside every cache lock; re-armed when the resident bytes fall back under the budget or the
    // budget changes. The GUI posts a Messages row (Budget, Warning) from it; it must not block.
    struct BudgetWarning {
        std::size_t residentBytes = 0;
        std::size_t budgetBytes = 0;
        unsigned percent = 0; // residentBytes as a percentage of the budget
    };
    using BudgetWarningHook = std::function<void(const BudgetWarning&)>;
    void setBudgetWarningHook(BudgetWarningHook hook);

    // Sibling chunks decoded in one sweep (a text slab, 05 section 7.1), inserted as one group: members whose
    // chunk is already resident, being built, or of another version than the column's provider are skipped;
    // the group is recorded so M5 eviction treats it as a unit and never evicts it while a member is pinned.
    // Each member's chunk must have the column's dtype and the chunk's sample count. Takes ownership of every
    // member's chunk; returns the number inserted.
    struct SealedChunk {
        const Column* column = nullptr;
        SampleIdx chunkNo = 0;
        std::uint64_t version = 0;
        OwnedChunk chunk;
    };
    std::size_t insertGroup(std::span<SealedChunk> members);

    // Frees every unpinned chunk (runtime shutdown, 01 section 5.3 rule 11; tests). Their columns
    // re-materialise on demand.
    void clear();

    class Impl;

private:
    friend class ChunkHandle;
    friend class detail::ColumnCore;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::store
