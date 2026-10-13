// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TaskPool (D-031 as amended by D-123; 01 sections 5.1 and 5.4): one Qt-free priority pool per process.
//   * Workers are std::threads stopped through a flag and joined by join()/~TaskPool (no std::jthread).
//   * Three bands; a strictly higher band is always dequeued first; FIFO within a band.
//   * submit(priority, key, requester, fn) calls fn(CancelToken) on a worker. If fn returns Result<X> the
//     Future is Future<X>; if it returns X, Future<X>; if void, Future<void>.
//   * Keys coalesce as documented on TaskKey (share keys share one Future and cancel collectively;
//     replace keys keep at most one running and one pending task; an empty key never coalesces). A key
//     reused with a different result type asserts in debug builds and resolves the new Future to Internal.
//   * Exceptions never escape a worker: std::bad_alloc becomes OutOfMemory, anything else Internal.
//   * A task whose token is cancelled before it starts resolves to Cancelled without running.
//   * Rule W: Future::get on a worker first runs the awaited task inline when it is still queued.
// Trace-build ordering by distance from the cursor's worksheet (01 section 5.1) is the caller's policy
// (RenderService, M1): it submits in the order it wants within a band.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/config.hpp>
#include <esses/base/executor.hpp>
#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace esses {

class TaskPool;

namespace detail {

// The token a task runs under: pool-owned, combining its requesters' tokens (01 section 5.1 semantics 3).
class TaskCancelState final : public CancelState {
public:
    enum class Mode : std::uint8_t {
        AnyRequester,  // unkeyed and replace-key tasks: the single requester's token
        AllRequesters, // share keys: cancelled only when every registered requester has cancelled
        PoolOnly       // chunk/ and summary/: only cancelAll() cancels
    };
    explicit TaskCancelState(Mode mode) noexcept : mode_(mode) {}

    bool isCancelled() const noexcept override;
    // Cancellation already observed (latched) or set by the pool; unlike isCancelled() it never latches, so
    // a share key whose requesters all cancelled can still be rescued by a new requester until the task saw
    // it.
    bool fired() const noexcept { return poolCancelled_.load() || latched_.load(); }
    void cancelByPool() noexcept { poolCancelled_.store(true); }
    void addRequester(CancelToken token);

private:
    Mode mode_;
    std::atomic<bool> poolCancelled_{false};
    mutable std::atomic<bool> latched_{false};
    mutable std::mutex mutex_;
    std::vector<CancelToken> requesters_;
};

enum class TaskStage : std::uint8_t { Queued, Parked, Running, Done };

class TaskNode {
public:
    TaskNode(Priority p, TaskKey k, std::type_index type, std::shared_ptr<SharedStateBase> st)
        : priority(p), key(std::move(k)), kind(key.kind()), resultType(type), state(std::move(st)) {}
    TaskNode(const TaskNode&) = delete;
    TaskNode& operator=(const TaskNode&) = delete;
    virtual ~TaskNode() = default;

    virtual void execute(const CancelToken& token) noexcept = 0; // runs the callable, publishes the result
    virtual void resolve(Error error) noexcept = 0;              // publishes a failure without running

    // Guarded by the owning pool's mutex, except `stage`, which Future::get also reads for its debug check.
    Priority priority;
    TaskKey key;
    TaskKey::Kind kind;
    std::type_index resultType;
    std::shared_ptr<SharedStateBase> state;
    std::shared_ptr<TaskCancelState> cancel;
    TaskPool* pool = nullptr;
    std::uint64_t queueGeneration = 0;
    std::atomic<TaskStage> stage{TaskStage::Queued};
};

template <class R> struct TaskValue {
    using type = R;
};
template <class X> struct TaskValue<Result<X>> {
    using type = X;
};
template <class Fn>
using TaskValueT = typename TaskValue<std::remove_cvref_t<std::invoke_result_t<Fn&, CancelToken>>>::type;

Failure taskExceptionFailure(const char* what) noexcept;

template <class T, class Fn> Result<T> invokeTask(Fn& fn, const CancelToken& token) noexcept {
    using R = std::remove_cvref_t<std::invoke_result_t<Fn&, CancelToken>>;
    try {
        if constexpr (std::is_void_v<R>) {
            std::invoke(fn, CancelToken(token));
            return Result<T>();
        } else if constexpr (kIsResult<R>) {
            return std::invoke(fn, CancelToken(token));
        } else {
            return Result<T>(std::invoke(fn, CancelToken(token)));
        }
    } catch (const std::bad_alloc&) {
        return Result<T>(Failure{Error{ErrorCode::OutOfMemory, "bad_alloc", {}}});
    } catch (const std::exception& e) {
        return Result<T>(taskExceptionFailure(e.what()));
    } catch (...) {
        return Result<T>(taskExceptionFailure(nullptr));
    }
}

template <class T, class Fn> class TaskNodeImpl final : public TaskNode {
public:
    TaskNodeImpl(Priority p, TaskKey k, std::shared_ptr<SharedState<T>> st, Fn fn)
        : TaskNode(p, std::move(k), std::type_index(typeid(T)), st), typed_(std::move(st)),
          fn_(std::move(fn)) {}

    void execute(const CancelToken& token) noexcept override {
        if (!fn_) {
            return;
        }
        Result<T> r = invokeTask<T>(*fn_, token);
        fn_.reset(); // release captured resources as soon as the task is done
        publish(std::move(r));
    }
    void resolve(Error error) noexcept override {
        fn_.reset();
        publish(Result<T>(Failure{std::move(error)}));
    }

private:
    void publish(Result<T> r) noexcept {
        if (auto st = std::move(typed_)) {
            st->set(std::move(r));
        }
    }

    std::shared_ptr<SharedState<T>> typed_;
    std::optional<Fn> fn_;
};

} // namespace detail

class TaskPool final : public Executor {
public:
    explicit TaskPool(unsigned workers = defaultWorkerCount());
    ~TaskPool() override; // join(): refuse new work, cancelAll(""), wait for running tasks, join the workers
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    template <class F>
    auto submit(Priority priority, TaskKey key, CancelToken requester, F&& fn)
        -> Future<detail::TaskValueT<std::decay_t<F>>>;

    // Still queued -> dequeue and run on the calling thread, return true; running, done or unknown -> false.
    bool tryRunInline(const TaskKey& key);
    // Cancels queued (their Futures resolve to Cancelled) and running (their tokens fire) tasks whose key
    // starts with the prefix; "" cancels everything, including unkeyed tasks.
    void cancelAll(std::string_view keyPrefix);
    // Moves a queued task to a strictly higher band; no-op if running, done, unknown or not higher.
    void raise(const TaskKey& key, Priority priority);
    // Refuses new work (later submits resolve to Cancelled), cancels everything, waits for running tasks and
    // joins the workers. Idempotent. Must not be called from one of this pool's workers.
    void join();

    struct Stats {
        std::array<std::size_t, kPriorityBands> queued{}; // runnable tasks per band
        std::size_t parked = 0;                           // replace-key tasks waiting for their running twin
        std::size_t running = 0;
        std::size_t workers = 0;
    };
    Stats stats() const;
    // Diagnostics and tests: the keys and bands of the runnable (queued) tasks whose key starts with
    // `keyPrefix` ("" includes unkeyed tasks), band by band, FIFO within a band. Parked and running tasks
    // are not listed.
    struct QueuedTask {
        std::string key;
        Priority priority = Priority::Background;
    };
    std::vector<QueuedTask> queuedTasks(std::string_view keyPrefix) const;

    // Executor: fire-and-forget submit.
    void run(Priority priority, TaskKey key, CancelToken token, std::function<void(CancelToken)> fn) override;

    static unsigned defaultWorkerCount() noexcept; // hardware threads - 1, minimum 2
    // The pool installed by the process runtime (01 section 5.3 rule 11); instance() aborts when none is
    // installed. Tests construct their own pools.
    static TaskPool& instance();
    static TaskPool* installed() noexcept;
    static void install(TaskPool* pool) noexcept;
    // The pool whose worker is the calling thread, or nullptr.
    static TaskPool* currentWorkerPool() noexcept;

    // Rule W support for Future::get: runs `node` inline if it is still queued in this pool.
    bool tryRunInlineNode(const std::shared_ptr<detail::TaskNode>& node);

private:
    std::shared_ptr<detail::SharedStateBase> submitNode(const std::shared_ptr<detail::TaskNode>& node,
                                                        CancelToken requester);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

template <class F>
auto TaskPool::submit(Priority priority, TaskKey key, CancelToken requester, F&& fn)
    -> Future<detail::TaskValueT<std::decay_t<F>>> {
    using Fn = std::decay_t<F>;
    using T = detail::TaskValueT<Fn>;
    auto state = std::make_shared<detail::SharedState<T>>();
    auto node = std::make_shared<detail::TaskNodeImpl<T, Fn>>(priority, std::move(key), state,
                                                              Fn(std::forward<F>(fn)));
    state->bindNode(node);
    state.reset();
    std::shared_ptr<detail::SharedStateBase> shared = submitNode(node, std::move(requester));
    return Future<T>(std::static_pointer_cast<detail::SharedState<T>>(std::move(shared)));
}

} // namespace esses
