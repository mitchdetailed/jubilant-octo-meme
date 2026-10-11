// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Column: chunk slot states, tryPin/prefetch scheduling, require with Rule W, buildSummary (02 section 3.2
// behaviours 6-11; 01 sections 5.1 and 5.3; D-011, D-123).
#include "column_core.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/log.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace esses::store {

namespace {

std::atomic<std::uint64_t> gNextColumnId{0};

ErrorContext chunkContext(const std::string& name, SampleIdx n) {
    ErrorContext ctx;
    ctx.channel = name;
    ctx.detail = "chunk " + std::to_string(n);
    return ctx;
}

// Puts a slot that this thread set to Building back to Absent and wakes its waiters, unless dismissed: an
// exception between "Building" and "Sealed" (bad_alloc in summaryFor, insertLocked or a seal, a system_error
// from the mutex, a throwing provider version()) must never leave the slot Building, or every other thread
// would wait on it forever and this thread would see "recursive materialisation" (Rule W).
class BuildingGuard {
public:
    BuildingGuard(detail::Shard& shard, detail::Slot& slot, std::unique_lock<std::mutex>& lock) noexcept
        : shard_(shard), slot_(slot), lock_(lock) {}
    BuildingGuard(const BuildingGuard&) = delete;
    BuildingGuard& operator=(const BuildingGuard&) = delete;
    ~BuildingGuard() {
        if (!armed_) {
            return;
        }
        try {
            if (!lock_.owns_lock()) {
                lock_.lock();
            }
            slot_.builder = std::thread::id();
            slot_.state.store(detail::SlotState::Absent, std::memory_order_relaxed);
            lock_.unlock();
        } catch (...) { // NOLINT(bugprone-empty-catch): the mutex itself failed; nothing more can be done
        }
        shard_.cv.notify_all();
    }
    void dismiss() noexcept { armed_ = false; }

private:
    detail::Shard& shard_;
    detail::Slot& slot_;
    std::unique_lock<std::mutex>& lock_;
    bool armed_ = true;
};

} // namespace

namespace detail {

// ---- ColumnCore
// ----------------------------------------------------------------------------------------------

ColumnCore::ColumnCore(std::shared_ptr<IChunkProvider> p, ColumnOptions options)
    : provider(std::move(p)), cache(options.cache != nullptr ? options.cache : &ChunkCache::instance()),
      impl(cache->impl_.get()), id(gNextColumnId.fetch_add(1, std::memory_order_relaxed) + 1),
      size(std::max<SampleIdx>(provider->size(), 0)), chunks(chunkCountFor(size)), dtype(provider->dtype()),
      independent(provider->chunkIndependent()), onSealed(std::move(options.onChunkSealed)),
      slots_(std::make_unique<Slot[]>(static_cast<std::size_t>(chunks))) {
    shardIx = static_cast<std::uint32_t>(id % ChunkCache::Impl::kShards);
    shard = &impl->shards[shardIx];
    pool = options.pool;
    keyPrefix = options.taskKeyPrefix.empty() ? "chunk/column-" + std::to_string(id) : options.taskKeyPrefix;
    ESSES_ASSERT_MSG(keyPrefix.starts_with("chunk/"), "ColumnOptions::taskKeyPrefix must start with chunk/");
    impl->liveColumns.fetch_add(1, std::memory_order_relaxed);
}

ColumnCore::~ColumnCore() {
    {
        std::lock_guard lock(shard->mutex);
        for (SampleIdx n = 0; n < chunks; ++n) {
            Slot& s = slots_[n];
            if (s.entry != nullptr) {
                impl->retireLocked(*shard, s.entry); // freed now, or at the last unpin of a surviving handle
                s.entry = nullptr;
            }
        }
    }
    impl->liveColumns.fetch_sub(1, std::memory_order_relaxed);
}

TaskKey ColumnCore::keyFor(SampleIdx n) const {
    return TaskKey{keyPrefix + "/" + std::to_string(n)};
}

TaskPool* ColumnCore::taskPool() const noexcept {
    return pool != nullptr ? pool : TaskPool::installed();
}

void ColumnCore::dropSlotLocked(SampleIdx n, const CacheEntry* e) noexcept {
    if (n >= 0 && n < chunks && slots_[n].entry == e) {
        slots_[n].entry = nullptr;
        slots_[n].state.store(SlotState::Absent, std::memory_order_relaxed);
    }
}

ChunkHandle ColumnCore::makeHandle(SampleIdx firstChunk, std::uint32_t count) {
    return {cache, shardIx, firstChunk, count};
}

void ColumnCore::setPin(ChunkHandle& h, std::uint32_t k, CacheEntry* e) noexcept {
    h.pins()[k] = ChunkHandle::Pin{e, e->chunk.get()};
}

void ColumnCore::notifySealed(SampleIdx n) noexcept {
    if (onSealed) {
        try {
            onSealed(n);
        } catch (...) { // NOLINT(bugprone-empty-catch): hooks must not throw into a worker
        }
    }
}

std::shared_ptr<SummaryPyramid> ColumnCore::summaryFor(std::uint64_t v) {
    std::lock_guard lock(summaryMutex_);
    if (!summary_) {
        summary_ = std::make_shared<SummaryPyramid>(size, dtype, v);
    } else if (summary_->version() < v) {
        retiredSummaries_.push_back(std::move(summary_));
        summary_ = std::make_shared<SummaryPyramid>(size, dtype, v);
    }
    return summary_; // fill() rejects chunks of another version, so an older v never writes here
}

Result<void> ColumnCore::checkBuilt(const ChunkBuilder& b, SampleIdx n) const {
    if (!b.allocated()) {
        return fail(ErrorCode::Internal, "chunk provider returned without allocating the chunk",
                    chunkContext(provider->debugName(), n));
    }
    if (b.count() != chunkSampleCount(size, n) || b.dtype() != dtype) {
        return fail(ErrorCode::Internal, "chunk provider produced a chunk of the wrong size or dtype",
                    chunkContext(provider->debugName(), n));
    }
    return ok();
}

Result<OwnedChunk> ColumnCore::materialiseOne(SampleIdx n, std::uint64_t version, ChunkBuilder& builder,
                                              const CancelToken& cancel) {
    Result<void> r = ok();
    try {
        r = provider->materialise(n, builder, cancel);
    } catch (const std::bad_alloc&) {
        r = fail(ErrorCode::OutOfMemory, "bad_alloc in chunk provider",
                 chunkContext(provider->debugName(), n));
    } catch (...) {
        r = fail(ErrorCode::Internal, "exception in chunk provider", chunkContext(provider->debugName(), n));
    }
    if (!r) {
        ESSES_LOG(Debug, "store.column", "materialise failed for chunk ", n, " of ", provider->debugName(),
                  ": ", r.error().toString());
        return Failure{r.error()};
    }
    if (auto c = checkBuilt(builder, n); !c) {
        return Failure{c.error()};
    }
    return builder.seal(version);
}

Result<CacheEntry*> ColumnCore::ensure(SampleIdx n, EnsureMode mode, const CancelToken& cancel) {
    std::unique_lock lock(shard->mutex);
    bool counted = false;
    bool triedInline = false;
    for (;;) {
        Slot& s = slots_[n];
        const std::uint64_t v = provider->version();
        SlotState st = s.state.load(std::memory_order_relaxed);
        if (st == SlotState::Sealed) {
            if (s.entry->chunk->version() == v) {
                if (!counted) {
                    impl->hits.fetch_add(1, std::memory_order_relaxed);
                }
                if (mode.pin) {
                    impl->pinLocked(s.entry);
                }
                return s.entry;
            }
            impl->retireLocked(*shard, s.entry); // stale (behaviour 9): absent from now on
            s.entry = nullptr;
            s.state.store(SlotState::Absent, std::memory_order_relaxed);
            st = SlotState::Absent;
        }
        if (!counted) {
            impl->misses.fetch_add(1, std::memory_order_relaxed);
            counted = true;
        }
        if (st == SlotState::Building) {
            if (s.builder == std::this_thread::get_id()) {
                return fail(ErrorCode::Internal, "recursive materialisation of a chunk",
                            chunkContext(provider->debugName(), n));
            }
            if (!mode.wait) {
                return static_cast<CacheEntry*>(nullptr);
            }
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "cancelled while waiting for a chunk");
            }
            // The builder is a running thread (Building is set only by the thread that materialises), so this
            // wait always makes progress (Rule W).
            shard->cv.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        if (mode.runQueued && s.scheduled && !triedInline) {
            if (TaskPool* p = taskPool(); p != nullptr) {
                triedInline = true;
                const TaskKey key = keyFor(n);
                lock.unlock();
                p->tryRunInline(key); // Rule W (a): a still-queued chunk task runs here
                lock.lock();
                continue;
            }
        }
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled before materialising a chunk");
        }
        s.state.store(SlotState::Building, std::memory_order_relaxed);
        s.builder = std::this_thread::get_id();
        lock.unlock();

        BuildingGuard guard(*shard, s, lock);
        CacheEntry* e = nullptr;
        bool raced = false;
        try {
            Result<OwnedChunk> built = Result<OwnedChunk>(OwnedChunk{});
            {
                // A sequential provider is stateful: one materialise() at a time per column, never
                // concurrently with buildSummary()'s pass (02 section 3.2 behaviour 7).
                std::unique_lock<std::mutex> serial;
                if (!independent) {
                    serial = std::unique_lock<std::mutex>(sequentialMutex);
                }
                ChunkBuilder builder;
                built = materialiseOne(n, v, builder, cancel);
            }
            // A version bump during the decode leaves it unknown which definition the samples came from:
            // discard the chunk and build again, so a chunk's samples always belong to the version it is
            // stamped with.
            raced = built && provider->version() != v;
            if (built && !raced) {
                summaryFor(v)->fill(n, **built); // levels already exist; still exclusive to this thread
            }

            lock.lock();
            if (!built || raced) {
                s.builder = std::thread::id();
                s.state.store(SlotState::Absent, std::memory_order_relaxed);
                guard.dismiss();
                lock.unlock();
                shard->cv.notify_all();
                if (!raced) {
                    return Failure{built.error()};
                }
                built = Result<OwnedChunk>(OwnedChunk{}); // freed here: it was never published
            } else {
                e = impl->insertLocked(*shard, std::move(*built), this, n, 0);
                s.builder = std::thread::id();
                s.entry = e;
                s.state.store(SlotState::Sealed, std::memory_order_relaxed);
                guard.dismiss();
                if (mode.pin) {
                    impl->pinLocked(e);
                }
                lock.unlock();
                shard->cv.notify_all();
            }
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "out of memory while building a chunk",
                        chunkContext(provider->debugName(), n)); // the guard resets the slot
        } catch (const std::exception& ex) {
            return fail(ErrorCode::Internal, std::string("exception while building a chunk: ") + ex.what(),
                        chunkContext(provider->debugName(), n));
        } catch (...) {
            return fail(ErrorCode::Internal, "exception while building a chunk",
                        chunkContext(provider->debugName(), n));
        }
        if (raced) {
            lock.lock();
            continue;
        }
        impl->afterInsert();
        notifySealed(n);
        return e;
    }
}

std::optional<ChunkHandle> ColumnCore::tryPin(SampleIdx n, Priority prio, bool scheduleIfAbsent) {
    if (n < 0 || n >= chunks) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(shard->mutex);
        Slot& s = slots_[n];
        if (s.state.load(std::memory_order_relaxed) == SlotState::Sealed) {
            if (s.entry->chunk->version() == provider->version()) {
                impl->hits.fetch_add(1, std::memory_order_relaxed);
                impl->pinLocked(s.entry);
                ChunkHandle h = makeHandle(n, 1);
                setPin(h, 0, s.entry);
                return {std::move(h)};
            }
            impl->retireLocked(*shard, s.entry);
            s.entry = nullptr;
            s.state.store(SlotState::Absent, std::memory_order_relaxed);
        }
        impl->misses.fetch_add(1, std::memory_order_relaxed);
    }
    if (scheduleIfAbsent) {
        schedule(n, prio);
    }
    return std::nullopt;
}

bool ColumnCore::isResident(SampleIdx n) {
    if (n < 0 || n >= chunks) {
        return false;
    }
    std::lock_guard lock(shard->mutex);
    const Slot& s = slots_[n];
    return s.state.load(std::memory_order_relaxed) == SlotState::Sealed &&
           s.entry->chunk->version() == provider->version();
}

void ColumnCore::clearScheduled(SampleIdx n, std::uint32_t gen) {
    std::lock_guard lock(shard->mutex);
    Slot& s = slots_[n];
    if (s.scheduleGen == gen) {
        s.scheduled = false;
    }
}

void ColumnCore::schedule(SampleIdx n, Priority prio) {
    if (n < 0 || n >= chunks) {
        return;
    }
    TaskPool* p = taskPool();
    if (p == nullptr) {
        return;
    }
    bool submit = false;
    bool raise = false;
    std::uint32_t gen = 0;
    {
        std::lock_guard lock(shard->mutex);
        Slot& s = slots_[n];
        SlotState st = s.state.load(std::memory_order_relaxed);
        if (st == SlotState::Sealed) {
            if (s.entry->chunk->version() == provider->version()) {
                return;
            }
            impl->retireLocked(*shard, s.entry);
            s.entry = nullptr;
            s.state.store(SlotState::Absent, std::memory_order_relaxed);
            st = SlotState::Absent;
        }
        if (st == SlotState::Building) {
            return; // coalesced: being built right now
        }
        if (s.scheduled) {
            if (prio < s.scheduledPrio) { // coalesced: raise the queued task
                s.scheduledPrio = prio;
                raise = true;
            }
        } else {
            s.scheduled = true;
            s.scheduledPrio = prio;
            gen = ++s.scheduleGen;
            submit = true;
        }
    }
    const TaskKey key = keyFor(n);
    if (raise) {
        p->raise(key, prio);
    }
    if (!submit) {
        return;
    }
    const std::weak_ptr<ColumnCore> weak = weak_from_this();
    try {
        // chunk/ keys are share-owned-by-source: they ignore requester cancellation and stop only through
        // cancelAll(<source prefix>) (01 section 5.4 rule 3).
        Future<void> f =
            p->submit(prio, key, CancelToken::none(), [weak, n](const CancelToken& token) -> Result<void> {
                const std::shared_ptr<ColumnCore> core = weak.lock();
                if (!core) {
                    return ok();
                }
                // A sequential provider builds chunks 0..n in order (behaviour 7), as require() does: never
                // chunk n alone. A chunk another thread is building is waited for (Rule W).
                if (!core->independent) {
                    for (SampleIdx k = 0; k < n; ++k) {
                        if (Result<CacheEntry*> r = core->ensure(k, EnsureMode{false, true, false}, token);
                            !r) {
                            return Failure{r.error()};
                        }
                    }
                }
                Result<CacheEntry*> r = core->ensure(n, EnsureMode{false, false, false}, token);
                if (!r) {
                    return Failure{r.error()};
                }
                return ok();
            });
        f.onReady([weak, n, gen](const Result<void>&) {
            if (const std::shared_ptr<ColumnCore> core = weak.lock()) {
                core->clearScheduled(n, gen);
            }
        });
    } catch (...) {
        clearScheduled(n, gen);
    }
}

Result<ChunkHandle> ColumnCore::require(SampleIdx first, SampleIdx last, Priority prio,
                                        const CancelToken& cancel) {
    if (ThreadRole::isNoBlock()) {
        ESSES_ASSERT_MSG(false, "Column::require called on a NoBlock (GUI) thread; use tryPin");
        return fail(ErrorCode::Invalid, "Column::require on a NoBlock thread");
    }
    first = std::max<SampleIdx>(first, 0);
    last = std::min(last, size - 1);
    if (first > last) {
        return fail(ErrorCode::Invalid, "Column::require: empty sample range",
                    chunkContext(provider->debugName(), chunkOf(first)));
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled");
    }
    const SampleIdx c0 = chunkOf(first);
    const SampleIdx c1 = chunkOf(last);
    if (!independent) {
        // Sequential provider: chunks 0..c0-1 first, in order, on this thread (behaviour 7).
        for (SampleIdx n = 0; n < c0; ++n) {
            Result<CacheEntry*> r = ensure(n, EnsureMode{false, true, true}, cancel);
            if (!r) {
                return Failure{r.error()};
            }
        }
    } else if (c1 > c0) {
        // Parallelism: other workers may build the rest of the span while this thread builds the first.
        for (SampleIdx n = c0 + 1; n <= c1; ++n) {
            schedule(n, prio);
        }
    }
    ChunkHandle h = makeHandle(c0, static_cast<std::uint32_t>(c1 - c0 + 1));
    for (SampleIdx n = c0; n <= c1; ++n) {
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        Result<CacheEntry*> r = ensure(n, EnsureMode{true, true, true}, cancel);
        if (!r) {
            return Failure{r.error()};
        }
        setPin(h, static_cast<std::uint32_t>(n - c0), *r);
    }
    return {std::move(h)};
}

Result<void> ColumnCore::buildSummary(const CancelToken& cancel) {
    if (ThreadRole::isNoBlock()) {
        ESSES_ASSERT_MSG(false, "Column::buildSummary called on a NoBlock (GUI) thread");
        return fail(ErrorCode::Invalid, "Column::buildSummary on a NoBlock thread");
    }
    const std::uint64_t v = provider->version();
    const std::shared_ptr<SummaryPyramid> summary = summaryFor(v);
    // A sequential provider's pass holds the column's sequential mutex throughout, so no ensure() builder
    // calls the stateful provider in the middle of it.
    std::unique_lock<std::mutex> serial;
    if (!independent) {
        serial = std::unique_lock<std::mutex>(sequentialMutex);
    }
    ChunkBuilder scratch;
    for (SampleIdx n = 0; n < chunks; ++n) {
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "summary build cancelled");
        }
        if (independent) {
            if (summary->chunkFilled(n)) {
                continue;
            }
            if (std::optional<ChunkHandle> h = tryPin(n, Priority::Background, false)) {
                if (summary->fill(n, *h->chunk(n)) || summary->chunkFilled(n)) {
                    continue; // read pinned; nothing decoded
                }
            }
        }
        // A sequential provider is streamed through every chunk in order, resident or not.
        Result<void> r = ok();
        try {
            r = provider->materialise(n, scratch, cancel);
        } catch (...) {
            r = fail(ErrorCode::Internal, "exception in chunk provider",
                     chunkContext(provider->debugName(), n));
        }
        if (!r) {
            return r;
        }
        if (auto c = checkBuilt(scratch, n); !c) {
            return c;
        }
        if (provider->version() != v) {
            return ok(); // superseded: the summary of the new version starts empty and is built by a later
                         // pass
        }
        Result<const Chunk*> sealed = scratch.sealScratch(v);
        if (!sealed) {
            return Failure{sealed.error()};
        }
        summary->fill(n, **sealed); // the samples are discarded with the scratch block
    }
    return ok();
}

} // namespace detail

// ---- Column
// --------------------------------------------------------------------------------------------------

Column::Column(std::shared_ptr<IChunkProvider> provider) : Column(std::move(provider), ColumnOptions{}) {}

Column::Column(std::shared_ptr<IChunkProvider> provider, ColumnOptions options) {
    ESSES_ASSERT_MSG(provider != nullptr, "Column needs a chunk provider");
    if (provider != nullptr) {
        core_ = std::make_shared<detail::ColumnCore>(std::move(provider), std::move(options));
    }
}

SampleIdx Column::size() const noexcept {
    return core_ ? core_->size : 0;
}

Dtype Column::dtype() const noexcept {
    return core_ ? core_->dtype : Dtype::F32;
}

std::uint64_t Column::version() const noexcept {
    return core_ ? core_->provider->version() : 0;
}

SampleIdx Column::chunkCount() const noexcept {
    return core_ ? core_->chunks : 0;
}

bool Column::chunkIndependent() const noexcept {
    return core_ ? core_->independent : true;
}

std::string Column::debugName() const {
    return core_ ? core_->provider->debugName() : std::string("(empty column)");
}

std::uint64_t Column::id() const noexcept {
    return core_ ? core_->id : 0;
}

TaskKey Column::chunkTaskKey(SampleIdx chunkNo) const {
    return core_ ? core_->keyFor(chunkNo) : TaskKey{};
}

std::optional<ChunkHandle> Column::tryPin(SampleIdx chunkNo, Priority prio) const {
    if (!core_) {
        return std::nullopt;
    }
    return core_->tryPin(chunkNo, prio, true);
}

void Column::prefetch(SampleIdx chunkNo, Priority prio) const {
    if (core_) {
        core_->schedule(chunkNo, prio);
    }
}

Result<ChunkHandle> Column::require(SampleIdx first, SampleIdx last, Priority prio,
                                    const CancelToken& cancel) const {
    if (!core_) {
        return fail(ErrorCode::Invalid, "Column::require on an empty column");
    }
    return core_->require(first, last, prio, cancel);
}

double Column::at(SampleIdx i) const noexcept {
    if (!core_ || i < 0 || i >= core_->size) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    try {
        const std::optional<ChunkHandle> h = core_->tryPin(chunkOf(i), Priority::Visible, true);
        return h ? h->at(i) : std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

bool Column::isResident(SampleIdx chunkNo) const noexcept {
    return core_ && core_->isResident(chunkNo);
}

const SummaryPyramid& Column::summary() const {
    if (!core_) {
        static const SummaryPyramid kEmpty(0, Dtype::F32, 0);
        return kEmpty;
    }
    return *core_->summaryFor(core_->provider->version());
}

std::shared_ptr<const SummaryPyramid> Column::summarySnapshot() const {
    if (!core_) {
        return std::make_shared<const SummaryPyramid>(0, Dtype::F32, 0);
    }
    return core_->summaryFor(core_->provider->version());
}

Result<void> Column::buildSummary(const CancelToken& cancel) const {
    if (!core_) {
        return ok();
    }
    return core_->buildSummary(cancel);
}

} // namespace esses::store
