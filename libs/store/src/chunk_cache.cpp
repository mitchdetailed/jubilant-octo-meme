// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChunkCache bookkeeping and ChunkHandle pins (02 section 3.2, D-011, D-043; 01 section 5.3 rules 5 and 11).
#include "column_core.hpp"
#include "system_memory.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/log.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace esses::store {

namespace {
std::atomic<ChunkCache*> gInstalled{nullptr};
constexpr std::size_t kGiB = std::size_t{1} << 30;
} // namespace

// ---- ChunkCache::Impl
// ----------------------------------------------------------------------------------------

detail::CacheEntry* ChunkCache::Impl::insertLocked(detail::Shard& shard, OwnedChunk chunk,
                                                   detail::ColumnCore* owner, SampleIdx chunkNo,
                                                   std::uint64_t group) {
    auto* e = new detail::CacheEntry();
    e->bytes = chunk->bytes();
    e->chunk = std::move(chunk);
    e->owner = owner;
    e->chunkNo = chunkNo;
    e->group = group;
    e->next = shard.head;
    if (shard.head != nullptr) {
        shard.head->prev = e;
    }
    shard.head = e;
    resident.fetch_add(e->bytes, std::memory_order_relaxed);
    chunks.fetch_add(1, std::memory_order_relaxed);
    return e;
}

void ChunkCache::Impl::pinLocked(detail::CacheEntry* e) noexcept {
    if (e->pins++ == 0) {
        pinned.fetch_add(e->bytes, std::memory_order_relaxed);
        pinnedChunks.fetch_add(1, std::memory_order_relaxed);
    }
}

void ChunkCache::Impl::unpinLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept {
    ESSES_ASSERT_MSG(e->pins > 0, "ChunkHandle unpinned a chunk that is not pinned");
    if (e->pins == 0) {
        return;
    }
    if (--e->pins == 0) {
        pinned.fetch_sub(e->bytes, std::memory_order_relaxed);
        pinnedChunks.fetch_sub(1, std::memory_order_relaxed);
        if (!e->current) {
            freeLocked(shard, e);
        }
    }
}

void ChunkCache::Impl::retireLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept {
    e->current = false;
    e->owner = nullptr;
    if (e->pins == 0) {
        freeLocked(shard, e);
    }
}

void ChunkCache::Impl::freeLocked(detail::Shard& shard, detail::CacheEntry* e) noexcept {
    ESSES_ASSERT_MSG(e->pins == 0, "chunk memory freed while pinned");
    if (e->prev != nullptr) {
        e->prev->next = e->next;
    } else {
        shard.head = e->next;
    }
    if (e->next != nullptr) {
        e->next->prev = e->prev;
    }
    const std::size_t nowResident = resident.fetch_sub(e->bytes, std::memory_order_relaxed) - e->bytes;
    chunks.fetch_sub(1, std::memory_order_relaxed);
    if (nowResident <= budget.load(std::memory_order_relaxed)) {
        warnedStep.store(-1, std::memory_order_relaxed); // re-arm the warning for the next crossing
    }
    delete e; // frees the chunk allocation (OwnedChunk)
}

void ChunkCache::Impl::afterInsert() {
    const std::size_t r = resident.load(std::memory_order_relaxed);
    const std::size_t b = budget.load(std::memory_order_relaxed);
    if (r <= b) {
        return;
    }
    // Step 0 at 100-110 % of the budget, 1 at 110-120 %, ... (one warning per 10 % step, 01 section 7.2).
    const std::size_t over = r - b;
    const auto step = b == 0 ? std::int64_t{0}
                             : static_cast<std::int64_t>(
                                   std::min<std::size_t>((over / b) * 10 + (over % b) * 10 / b, 1'000'000));
    std::int64_t prev = warnedStep.load(std::memory_order_relaxed);
    while (step > prev) {
        if (warnedStep.compare_exchange_weak(prev, step, std::memory_order_relaxed)) {
            fireWarning(r, b);
            return;
        }
    }
}

void ChunkCache::Impl::fireWarning(std::size_t r, std::size_t b) {
    BudgetWarning w;
    w.residentBytes = r;
    w.budgetBytes = b;
    w.percent = b == 0 ? std::numeric_limits<unsigned>::max()
                       : static_cast<unsigned>(std::min<std::size_t>((r / b) * 100 + (r % b) * 100 / b,
                                                                     std::numeric_limits<unsigned>::max()));
    ESSES_LOG(Warning, "store.cache", "chunk cache over budget: ", r, " of ", b, " bytes resident (",
              w.percent, " %); no eviction before M5 (D-043)");
    BudgetWarningHook h;
    {
        std::lock_guard lock(hookMutex);
        h = hook;
    }
    if (h) {
        try {
            h(w);
        } catch (...) { // NOLINT(bugprone-empty-catch): hooks must not throw into the allocating worker
        }
    }
}

// ---- ChunkCache
// ----------------------------------------------------------------------------------------------

ChunkCache::ChunkCache() : ChunkCache(defaultBudgetBytes()) {}

ChunkCache::ChunkCache(std::size_t budgetBytes) : impl_(std::make_unique<Impl>(budgetBytes)) {}

ChunkCache::~ChunkCache() {
    ESSES_ASSERT_MSG(impl_->liveColumns.load() == 0, "ChunkCache destroyed while Columns still use it");
    for (detail::Shard& shard : impl_->shards) {
        std::lock_guard lock(shard.mutex);
        while (shard.head != nullptr) {
            detail::CacheEntry* e = shard.head;
            ESSES_ASSERT_MSG(e->pins == 0, "ChunkCache destroyed while a ChunkHandle still pins a chunk");
            if (e->owner != nullptr) {
                e->owner->dropSlotLocked(e->chunkNo, e);
            }
            e->pins = 0;
            impl_->freeLocked(shard, e);
        }
    }
    ChunkCache* self = this;
    gInstalled.compare_exchange_strong(self, nullptr);
}

ChunkCache& ChunkCache::instance() {
    ChunkCache* c = gInstalled.load(std::memory_order_acquire);
    if (c == nullptr) {
        ESSES_FATAL("ChunkCache::instance() called but the runtime installed no cache");
    }
    return *c;
}

ChunkCache* ChunkCache::installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

void ChunkCache::install(ChunkCache* cache) noexcept {
    gInstalled.store(cache, std::memory_order_release);
}

std::size_t ChunkCache::defaultBudgetBytes() noexcept {
    const std::uint64_t ram = detail::physicalMemoryBytes();
    const std::uint64_t quarter = ram / 4;
    const std::uint64_t budget = std::max<std::uint64_t>(kGiB, quarter);
    return static_cast<std::size_t>(std::min<std::uint64_t>(budget, std::numeric_limits<std::size_t>::max()));
}

void ChunkCache::setBudgetBytes(std::size_t bytes) {
    impl_->budget.store(bytes, std::memory_order_relaxed);
    impl_->warnedStep.store(-1, std::memory_order_relaxed);
    impl_->afterInsert();
}

std::size_t ChunkCache::budgetBytes() const noexcept {
    return impl_->budget.load(std::memory_order_relaxed);
}

ChunkCache::Stats ChunkCache::stats() const {
    Stats s;
    s.residentBytes = impl_->resident.load(std::memory_order_relaxed);
    s.pinnedBytes = impl_->pinned.load(std::memory_order_relaxed);
    s.chunks = impl_->chunks.load(std::memory_order_relaxed);
    s.pinnedChunks = impl_->pinnedChunks.load(std::memory_order_relaxed);
    s.hits = impl_->hits.load(std::memory_order_relaxed);
    s.misses = impl_->misses.load(std::memory_order_relaxed);
    s.evictions = impl_->evictions.load(std::memory_order_relaxed);
    s.overBudget = s.residentBytes > impl_->budget.load(std::memory_order_relaxed);
    return s;
}

void ChunkCache::setBudgetWarningHook(BudgetWarningHook hook) {
    std::lock_guard lock(impl_->hookMutex);
    impl_->hook = std::move(hook);
}

std::size_t ChunkCache::insertGroup(std::span<SealedChunk> members) {
    const std::uint64_t group = impl_->groupCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    std::size_t inserted = 0;
    std::vector<std::pair<std::shared_ptr<detail::ColumnCore>, SampleIdx>> sealed;
    for (SealedChunk& m : members) {
        OwnedChunk chunk = std::move(m.chunk); // never published unless inserted below
        if (m.column == nullptr || !m.column->core_ || !chunk) {
            continue;
        }
        const std::shared_ptr<detail::ColumnCore> core = m.column->core_;
        ESSES_ASSERT_MSG(core->cache == this, "insertGroup: the column belongs to another ChunkCache");
        if (core->cache != this || m.chunkNo < 0 || m.chunkNo >= core->chunks ||
            chunk->dtype() != core->dtype || chunk->count() != chunkSampleCount(core->size, m.chunkNo) ||
            chunk->version() != m.version) {
            continue;
        }
        const std::uint64_t v = core->provider->version();
        if (m.version != v) {
            continue; // stale sweep
        }
        core->summaryFor(v)->fill(m.chunkNo, *chunk);
        {
            std::lock_guard lock(core->shard->mutex);
            detail::Slot& s = core->slots_[m.chunkNo];
            const detail::SlotState st = s.state.load(std::memory_order_relaxed);
            if (st == detail::SlotState::Building) {
                continue; // its builder produces the same chunk
            }
            if (st == detail::SlotState::Sealed) {
                if (s.entry->chunk->version() == v) {
                    continue; // already resident
                }
                impl_->retireLocked(*core->shard, s.entry);
                s.entry = nullptr;
            }
            s.entry = impl_->insertLocked(*core->shard, std::move(chunk), core.get(), m.chunkNo, group);
            s.state.store(detail::SlotState::Sealed, std::memory_order_relaxed);
            ++inserted;
        }
        core->shard->cv.notify_all();
        sealed.emplace_back(core, m.chunkNo);
    }
    impl_->afterInsert();
    for (auto& [core, n] : sealed) {
        if (core->onSealed) {
            try {
                core->onSealed(n);
            } catch (...) { // NOLINT(bugprone-empty-catch): hooks must not throw
            }
        }
    }
    return inserted;
}

void ChunkCache::clear() {
    for (detail::Shard& shard : impl_->shards) {
        std::lock_guard lock(shard.mutex);
        detail::CacheEntry* e = shard.head;
        while (e != nullptr) {
            detail::CacheEntry* next = e->next;
            if (e->pins == 0) {
                if (e->owner != nullptr) {
                    e->owner->dropSlotLocked(e->chunkNo, e);
                }
                impl_->freeLocked(shard, e);
            }
            e = next;
        }
    }
}

// ---- ChunkHandle
// ---------------------------------------------------------------------------------------------

ChunkHandle::ChunkHandle(ChunkCache* cache, std::uint32_t shard, SampleIdx firstChunk, std::uint32_t count)
    : cache_(cache), shard_(shard), count_(count), first_(firstChunk) {
    if (count > kInlinePins) {
        heap_ = std::make_unique<Pin[]>(count);
    }
}

ChunkHandle::ChunkHandle(ChunkHandle&& other) noexcept
    : cache_(other.cache_), shard_(other.shard_), count_(std::exchange(other.count_, 0)),
      first_(other.first_), inline_(other.inline_), heap_(std::move(other.heap_)) {
    other.inline_ = {};
}

ChunkHandle& ChunkHandle::operator=(ChunkHandle&& other) noexcept {
    if (this != &other) {
        reset();
        cache_ = other.cache_;
        shard_ = other.shard_;
        count_ = std::exchange(other.count_, 0);
        first_ = other.first_;
        inline_ = other.inline_;
        heap_ = std::move(other.heap_);
        other.inline_ = {};
    }
    return *this;
}

ChunkHandle::~ChunkHandle() {
    reset();
}

void ChunkHandle::reset() noexcept {
    if (count_ != 0 && cache_ != nullptr) {
        ChunkCache::Impl& impl = *cache_->impl_;
        detail::Shard& shard = impl.shards[shard_];
        std::lock_guard lock(shard.mutex);
        Pin* p = pins();
        for (std::uint32_t k = 0; k < count_; ++k) {
            if (p[k].entry != nullptr) {
                impl.unpinLocked(shard, p[k].entry);
            }
        }
    }
    count_ = 0;
    heap_.reset();
    inline_ = {};
}

SampleIdx ChunkHandle::endSample() const noexcept {
    if (count_ == 0) {
        return firstSample();
    }
    const Chunk* last = pins()[count_ - 1].chunk;
    return chunkFirstSample(lastChunk()) + (last != nullptr ? static_cast<SampleIdx>(last->count()) : 0);
}

std::optional<std::int64_t> ChunkHandle::int64At(SampleIdx i) const noexcept {
    const Chunk* c = chunk(chunkOf(i));
    if (c == nullptr || c->dtype() != Dtype::I64 || !c->validAt(chunkLocal(i))) {
        return std::nullopt;
    }
    return c->samples<std::int64_t>()[chunkLocal(i)];
}

} // namespace esses::store
