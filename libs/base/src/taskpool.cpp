// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>

namespace esses {

namespace {
thread_local TaskPool* tlsWorkerPool = nullptr;
std::atomic<TaskPool*> gInstalledPool{nullptr};

std::size_t band(Priority p) noexcept {
    return static_cast<std::size_t>(p);
}
} // namespace

// ---- TaskCancelState -------------------------------------------------------------------------------------

namespace detail {

bool TaskCancelState::isCancelled() const noexcept {
    if (poolCancelled_.load() || latched_.load()) {
        return true;
    }
    if (mode_ == Mode::PoolOnly) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (requesters_.empty()) {
        return false;
    }
    const auto isSet = [](const CancelToken& t) { return t.cancelled(); };
    const bool result = mode_ == Mode::AnyRequester
                            ? std::any_of(requesters_.begin(), requesters_.end(), isSet)
                            : std::all_of(requesters_.begin(), requesters_.end(), isSet);
    if (result) {
        latched_.store(true, std::memory_order_release);
    }
    return result;
}

void TaskCancelState::addRequester(CancelToken token) {
    std::lock_guard lock(mutex_);
    requesters_.push_back(std::move(token));
}

Failure taskExceptionFailure(const char* what) noexcept {
    try {
        ErrorContext ctx;
        if (what != nullptr) {
            ctx.detail = what;
        }
        return Failure{Error{ErrorCode::Internal, "exception in task", std::move(ctx)}};
    } catch (...) {
        return Failure{Error{ErrorCode::OutOfMemory, "bad_alloc", {}}};
    }
}

void blockingWait(const SharedStateBase& state) {
    if (state.ready()) {
        return;
    }
    ESSES_ASSERT_MSG(!ThreadRole::isNoBlock(), "blocking Future wait on a NoBlock (GUI) thread");
    if (TaskPool* pool = TaskPool::currentWorkerPool()) {
        if (const std::shared_ptr<TaskNode> node = state.node(); node && node->pool == pool) {
            pool->tryRunInlineNode(node); // Rule W (b): run the awaited task here if it is still queued
            if (!state.ready()) {
                const TaskStage stage = node->stage.load(std::memory_order_acquire);
                ESSES_ASSERT_MSG(stage == TaskStage::Running || stage == TaskStage::Done,
                                 "Rule W: a pool thread waits for a task that is not running");
            }
        }
    }
    state.waitReady();
}

} // namespace detail

// ---- TaskPool::Impl --------------------------------------------------------------------------------------

struct TaskPool::Impl {
    using NodePtr = std::shared_ptr<detail::TaskNode>;
    using Stage = detail::TaskStage;

    struct QueueEntry {
        NodePtr node;
        std::uint64_t generation;
    };
    struct KeyEntry {
        NodePtr active;  // share keys: the task; replace keys: the running task
        NodePtr pending; // replace keys only: the queued or parked successor
    };

    explicit Impl(TaskPool* o) : owner(o) {}

    TaskPool* owner;
    mutable std::mutex mutex;
    std::condition_variable workCv;
    std::array<std::deque<QueueEntry>, kPriorityBands> queues;
    std::array<std::size_t, kPriorityBands> queuedCount{};
    std::array<std::size_t, kPriorityBands> staleCount{};
    std::size_t parkedCount = 0;
    std::size_t runningCount = 0;
    std::unordered_map<std::string, KeyEntry> keys;
    std::unordered_map<const detail::TaskNode*, NodePtr> live;
    std::vector<std::thread> workers;
    std::uint64_t nextGeneration = 1;
    bool accepting = true;
    bool stopping = false;
    bool joined = false;

    std::size_t totalQueued() const noexcept {
        std::size_t n = 0;
        for (const std::size_t c : queuedCount) {
            n += c;
        }
        return n;
    }

    // All *Locked functions require `mutex`.
    void enqueueLocked(const NodePtr& n) {
        const std::size_t b = band(n->priority);
        n->queueGeneration = nextGeneration++;
        n->stage.store(Stage::Queued, std::memory_order_release);
        queues[b].push_back(QueueEntry{n, n->queueGeneration});
        ++queuedCount[b];
        workCv.notify_one();
    }

    void unqueueLocked(detail::TaskNode& n) {
        const std::size_t b = band(n.priority);
        --queuedCount[b];
        ++staleCount[b];
        n.queueGeneration = 0; // invalidates the entry left in the deque
        compactLocked(b);
    }

    void compactLocked(std::size_t b) {
        if (staleCount[b] < 64 || staleCount[b] < queuedCount[b]) {
            return;
        }
        auto& q = queues[b];
        q.erase(std::remove_if(q.begin(), q.end(),
                               [](const QueueEntry& e) {
                                   return e.node->queueGeneration != e.generation ||
                                          e.node->stage.load(std::memory_order_relaxed) != Stage::Queued;
                               }),
                q.end());
        staleCount[b] = 0;
    }

    NodePtr popLocked() {
        for (std::size_t b = 0; b < kPriorityBands; ++b) {
            auto& q = queues[b];
            while (!q.empty()) {
                QueueEntry e = std::move(q.front());
                q.pop_front();
                if (e.node->queueGeneration == e.generation &&
                    e.node->stage.load(std::memory_order_relaxed) == Stage::Queued) {
                    --queuedCount[b];
                    e.node->queueGeneration = 0;
                    return std::move(e.node);
                }
                if (staleCount[b] > 0) {
                    --staleCount[b];
                }
            }
        }
        return nullptr;
    }

    void markRunningLocked(const NodePtr& n) {
        n->stage.store(Stage::Running, std::memory_order_release);
        ++runningCount;
        if (n->kind == TaskKey::Kind::Replace) {
            KeyEntry& entry = keys[n->key.text];
            if (entry.pending == n) {
                entry.pending.reset();
            }
            entry.active = n;
        }
    }

    void removeFromKeysLocked(const NodePtr& n) {
        if (n->kind == TaskKey::Kind::None) {
            return;
        }
        const auto it = keys.find(n->key.text);
        if (it == keys.end()) {
            return;
        }
        KeyEntry& entry = it->second;
        if (entry.active == n) {
            entry.active.reset();
        }
        if (entry.pending == n) {
            entry.pending.reset();
        }
        if (!entry.active && !entry.pending) {
            keys.erase(it);
        }
    }

    void completeLocked(const NodePtr& n) {
        n->stage.store(Stage::Done, std::memory_order_release);
        --runningCount;
        live.erase(n.get());
        n->state.reset();
        if (n->kind == TaskKey::Kind::Replace) {
            const auto it = keys.find(n->key.text);
            if (it != keys.end()) {
                KeyEntry& entry = it->second;
                if (entry.active == n) {
                    entry.active.reset();
                }
                if (entry.pending && entry.pending->stage.load(std::memory_order_relaxed) == Stage::Parked) {
                    --parkedCount;
                    enqueueLocked(entry.pending); // the pending build starts now (01 section 5.4 rule 2)
                }
                if (!entry.active && !entry.pending) {
                    keys.erase(it);
                }
            }
        } else {
            removeFromKeysLocked(n);
        }
    }

    void runNode(const NodePtr& n) {
        const CancelToken token(n->cancel);
        if (token.cancelled()) {
            n->resolve(Error{ErrorCode::Cancelled, "task cancelled before it started", {}});
        } else {
            n->execute(token);
        }
        std::lock_guard lock(mutex);
        completeLocked(n);
    }

    void workerLoop() {
        tlsWorkerPool = owner;
        for (;;) {
            NodePtr n;
            {
                std::unique_lock lock(mutex);
                workCv.wait(lock, [this] { return stopping || totalQueued() > 0; });
                if (totalQueued() == 0) {
                    return; // stopping and nothing left
                }
                n = popLocked();
                if (!n) {
                    continue;
                }
                markRunningLocked(n);
            }
            runNode(n);
        }
    }

    // Claims a still-queued node for inline execution. Requires `mutex`.
    bool claimLocked(const NodePtr& n) {
        if (!n || n->pool != owner || n->stage.load(std::memory_order_relaxed) != Stage::Queued) {
            return false;
        }
        unqueueLocked(*n);
        markRunningLocked(n);
        return true;
    }

    NodePtr queuedCandidateLocked(const TaskKey& key) {
        const auto it = keys.find(key.text);
        if (it == keys.end()) {
            return nullptr;
        }
        const NodePtr& n = key.kind() == TaskKey::Kind::Replace ? it->second.pending : it->second.active;
        return n;
    }
};

// ---- TaskPool --------------------------------------------------------------------------------------------

TaskPool::TaskPool(unsigned workers) : impl_(std::make_unique<Impl>(this)) {
    const unsigned n = std::max(1u, workers);
    impl_->workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        impl_->workers.emplace_back([this] { impl_->workerLoop(); });
    }
}

TaskPool::~TaskPool() {
    join();
    TaskPool* self = this;
    gInstalledPool.compare_exchange_strong(self, nullptr);
}

unsigned TaskPool::defaultWorkerCount() noexcept {
    const unsigned hc = std::thread::hardware_concurrency();
    return std::max(2u, hc > 1 ? hc - 1 : 2u);
}

TaskPool& TaskPool::instance() {
    TaskPool* p = gInstalledPool.load(std::memory_order_acquire);
    if (p == nullptr) {
        ESSES_FATAL("TaskPool::instance(): no pool installed by the runtime");
    }
    return *p;
}

TaskPool* TaskPool::installed() noexcept {
    return gInstalledPool.load(std::memory_order_acquire);
}

void TaskPool::install(TaskPool* pool) noexcept {
    gInstalledPool.store(pool, std::memory_order_release);
}

TaskPool* TaskPool::currentWorkerPool() noexcept {
    return tlsWorkerPool;
}

std::shared_ptr<detail::SharedStateBase> TaskPool::submitNode(const std::shared_ptr<detail::TaskNode>& node,
                                                              CancelToken requester) {
    using Mode = detail::TaskCancelState::Mode;
    using Kind = TaskKey::Kind;
    node->pool = this;
    const Kind kind = node->kind;
    node->cancel =
        std::make_shared<detail::TaskCancelState>(kind == Kind::Share                ? Mode::AllRequesters
                                                  : kind == Kind::ShareOwnedBySource ? Mode::PoolOnly
                                                                                     : Mode::AnyRequester);
    std::shared_ptr<detail::SharedStateBase> result = node->state;
    std::vector<std::pair<std::shared_ptr<detail::TaskNode>, Error>> toResolve;
    bool typeMismatch = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting) {
            node->stage.store(detail::TaskStage::Done, std::memory_order_release);
            toResolve.emplace_back(node, Error{ErrorCode::Cancelled, "task pool is shut down", {}});
        } else if (kind == Kind::None) {
            node->cancel->addRequester(std::move(requester));
            impl_->live.emplace(node.get(), node);
            impl_->enqueueLocked(node);
        } else if (kind == Kind::Share || kind == Kind::ShareOwnedBySource) {
            Impl::KeyEntry& entry = impl_->keys[node->key.text];
            // A task whose token has already fired (the task observed that all its requesters cancelled, or
            // cancelAll) will only resolve Cancelled: a new requester gets a task of its own instead. Until
            // the task observes it, a new requester still joins and keeps it alive (AllRequesters). The old
            // task stays in `live` until it completes, and completeLocked() leaves the key's new task alone.
            if (entry.active && entry.active->cancel->fired()) {
                entry.active.reset();
            }
            if (entry.active) {
                const std::shared_ptr<detail::TaskNode> existing = entry.active;
                if (existing->resultType != node->resultType) {
                    typeMismatch = true;
                    node->stage.store(detail::TaskStage::Done, std::memory_order_release);
                    toResolve.emplace_back(node, Error{ErrorCode::Internal, "task key type mismatch", {}});
                } else {
                    existing->cancel->addRequester(std::move(requester));
                    if (node->priority < existing->priority &&
                        existing->stage.load(std::memory_order_relaxed) == detail::TaskStage::Queued) {
                        impl_->unqueueLocked(*existing);
                        existing->priority = node->priority; // raised to the higher of the two (rule 1)
                        impl_->enqueueLocked(existing);
                    }
                    result = existing->state;
                    node->stage.store(detail::TaskStage::Done,
                                      std::memory_order_release); // discarded duplicate
                }
            } else {
                entry.active = node;
                node->cancel->addRequester(std::move(requester));
                impl_->live.emplace(node.get(), node);
                impl_->enqueueLocked(node);
            }
        } else { // Kind::Replace
            Impl::KeyEntry& entry = impl_->keys[node->key.text];
            if ((entry.active && entry.active->resultType != node->resultType) ||
                (entry.pending && entry.pending->resultType != node->resultType)) {
                typeMismatch = true;
                node->stage.store(detail::TaskStage::Done, std::memory_order_release);
                toResolve.emplace_back(node, Error{ErrorCode::Internal, "task key type mismatch", {}});
            } else {
                if (entry.pending) { // latest wins on the queue: the older pending task is replaced
                    const std::shared_ptr<detail::TaskNode> old = entry.pending;
                    const detail::TaskStage oldStage = old->stage.load(std::memory_order_relaxed);
                    if (oldStage == detail::TaskStage::Queued) {
                        impl_->unqueueLocked(*old);
                    } else if (oldStage == detail::TaskStage::Parked) {
                        --impl_->parkedCount;
                    }
                    old->stage.store(detail::TaskStage::Done, std::memory_order_release);
                    impl_->live.erase(old.get());
                    toResolve.emplace_back(old,
                                           Error{ErrorCode::Cancelled, "replaced by a newer request", {}});
                }
                entry.pending = node;
                node->cancel->addRequester(std::move(requester));
                impl_->live.emplace(node.get(), node);
                if (entry.active) {
                    node->stage.store(detail::TaskStage::Parked, std::memory_order_release);
                    ++impl_->parkedCount;
                } else {
                    impl_->enqueueLocked(node);
                }
            }
        }
    }
    if (typeMismatch) {
        ESSES_ASSERT_MSG(!typeMismatch, "TaskPool: a task key was reused with a different result type");
    }
    for (auto& [n, error] : toResolve) {
        n->resolve(std::move(error));
    }
    return result;
}

bool TaskPool::tryRunInlineNode(const std::shared_ptr<detail::TaskNode>& node) {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->claimLocked(node)) {
            return false;
        }
    }
    impl_->runNode(node);
    return true;
}

bool TaskPool::tryRunInline(const TaskKey& key) {
    std::shared_ptr<detail::TaskNode> node;
    {
        std::lock_guard lock(impl_->mutex);
        node = impl_->queuedCandidateLocked(key);
        if (!impl_->claimLocked(node)) {
            return false;
        }
    }
    impl_->runNode(node);
    return true;
}

void TaskPool::cancelAll(std::string_view keyPrefix) {
    std::vector<std::shared_ptr<detail::TaskNode>> toResolve;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto it = impl_->live.begin(); it != impl_->live.end();) {
            const std::shared_ptr<detail::TaskNode> n = it->second;
            if (!std::string_view(n->key.text).starts_with(keyPrefix)) {
                ++it;
                continue;
            }
            const detail::TaskStage stage = n->stage.load(std::memory_order_relaxed);
            if (stage == detail::TaskStage::Queued || stage == detail::TaskStage::Parked) {
                if (stage == detail::TaskStage::Queued) {
                    impl_->unqueueLocked(*n);
                } else {
                    --impl_->parkedCount;
                }
                n->stage.store(detail::TaskStage::Done, std::memory_order_release);
                impl_->removeFromKeysLocked(n);
                toResolve.push_back(n);
                it = impl_->live.erase(it);
            } else {
                if (stage == detail::TaskStage::Running) {
                    n->cancel->cancelByPool();
                }
                ++it;
            }
        }
    }
    for (const auto& n : toResolve) {
        n->resolve(Error{ErrorCode::Cancelled, "cancelled by cancelAll", {}});
    }
}

void TaskPool::raise(const TaskKey& key, Priority priority) {
    std::lock_guard lock(impl_->mutex);
    const std::shared_ptr<detail::TaskNode> n = impl_->queuedCandidateLocked(key);
    if (!n || !(priority < n->priority)) {
        return;
    }
    const detail::TaskStage stage = n->stage.load(std::memory_order_relaxed);
    if (stage == detail::TaskStage::Queued) {
        impl_->unqueueLocked(*n);
        n->priority = priority;
        impl_->enqueueLocked(n);
    } else if (stage == detail::TaskStage::Parked) {
        n->priority = priority;
    }
}

void TaskPool::join() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->joined) {
            return;
        }
        impl_->accepting = false;
    }
    ESSES_ASSERT_MSG(currentWorkerPool() != this, "TaskPool::join() called from one of its own workers");
    cancelAll("");
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->workCv.notify_all();
    for (std::thread& t : impl_->workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::lock_guard lock(impl_->mutex);
    impl_->joined = true;
}

TaskPool::Stats TaskPool::stats() const {
    std::lock_guard lock(impl_->mutex);
    Stats s;
    s.queued = impl_->queuedCount;
    s.parked = impl_->parkedCount;
    s.running = impl_->runningCount;
    s.workers = impl_->workers.size();
    return s;
}

void TaskPool::run(Priority priority, TaskKey key, CancelToken token, std::function<void(CancelToken)> fn) {
    static_cast<void>(submit(priority, std::move(key), std::move(token), [f = std::move(fn)](CancelToken t) {
        if (f) {
            f(std::move(t));
        }
    }));
}

} // namespace esses
