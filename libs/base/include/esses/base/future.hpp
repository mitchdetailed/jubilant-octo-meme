// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Future<T> (01 section 5.1): a copyable handle on a shared state whose result is a Result<T>.
//   ready()/tryGet()  non-blocking; the only accessors a NoBlock (GUI) thread may call
//   get()             blocking; on a TaskPool worker it first runs the awaited task inline if it is still
//                     queued (Rule W); debug builds assert on a NoBlock thread
//   take() &&         blocking like get(), then moves the result out; only for the sole holder
//   onReady(fn)       runs fn on the completing thread, or inline on the caller if already ready
// Dropping the last Future does not cancel the task (01 section 5.4 rule 4).
#pragma once

#include <esses/base/assert.hpp>
#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace esses {

namespace detail {

class TaskNode;

class SharedStateBase {
public:
    SharedStateBase() = default;
    SharedStateBase(const SharedStateBase&) = delete;
    SharedStateBase& operator=(const SharedStateBase&) = delete;
    virtual ~SharedStateBase() = default;

    bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    void waitReady() const {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return ready_.load(std::memory_order_acquire); });
    }

    // The pool task producing this state (empty for ready-made futures). Bound once, before the state is
    // shared with another thread, and read-only afterwards.
    void bindNode(std::weak_ptr<TaskNode> node) noexcept { node_ = std::move(node); }
    std::shared_ptr<TaskNode> node() const noexcept { return node_.lock(); }

protected:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<bool> ready_{false};
    bool taken_ = false; // guarded by mutex_

private:
    std::weak_ptr<TaskNode> node_;
};

// Blocking wait used by Future::get/take: the ThreadRole assertion, Rule W help (TaskPool::tryRunInline on a
// pool worker) and the debug check that a worker only waits for a running task. Defined in taskpool.cpp.
void blockingWait(const SharedStateBase& state);

template <class T> class SharedState final : public SharedStateBase {
public:
    using Continuation = std::function<void(const Result<T>&)>;

    // Publishes r once; later calls are ignored. Continuations run on the calling thread, outside the lock;
    // an exception thrown by a continuation is swallowed (exceptions never escape a worker).
    void set(Result<T> r) noexcept {
        std::vector<Continuation> continuations;
        {
            std::lock_guard lock(mutex_);
            if (ready_.load(std::memory_order_relaxed)) {
                return;
            }
            result_.emplace(std::move(r));
            ready_.store(true, std::memory_order_release);
            continuations.swap(continuations_);
        }
        cv_.notify_all();
        for (auto& fn : continuations) {
            try {
                fn(*result_);
            } catch (...) { // NOLINT(bugprone-empty-catch): continuations must not throw
            }
        }
    }

    const Result<T>& result() const {
        ESSES_ASSERT_MSG(ready(), "Future result read before it is ready");
        if (taken_) {
            ESSES_FATAL("Future result read after take()");
        }
        return published();
    }

    Result<T> takeResult() {
        std::lock_guard lock(mutex_);
        if (taken_) {
            ESSES_FATAL("Future::take() called twice on one shared state");
        }
        taken_ = true;
        return std::move(published());
    }

    void addContinuation(Continuation fn) {
        {
            std::lock_guard lock(mutex_);
            if (!ready_.load(std::memory_order_relaxed)) {
                continuations_.push_back(std::move(fn));
                return;
            }
        }
        fn(published());
    }

private:
    // The result set() emplaced before it published ready_; reaching it earlier is a programming error.
    const Result<T>& published() const {
        if (!result_) {
            ESSES_FATAL("Future result read before it is ready");
        }
        return *result_;
    }
    Result<T>& published() {
        if (!result_) {
            ESSES_FATAL("Future result read before it is ready");
        }
        return *result_;
    }

    std::optional<Result<T>> result_;
    std::vector<Continuation> continuations_;
};

} // namespace detail

template <class T> class Future {
public:
    using value_type = T;

    Future() noexcept = default;
    explicit Future(std::shared_ptr<detail::SharedState<T>> state) noexcept : state_(std::move(state)) {}

    bool valid() const noexcept { return state_ != nullptr; }
    bool ready() const noexcept { return state_ != nullptr && state_->ready(); }

    std::optional<Result<T>> tryGet() const
        requires std::copy_constructible<Result<T>>
    {
        if (!ready()) {
            return std::nullopt;
        }
        return state_->result();
    }

    const Result<T>& get() const {
        requireValid();
        detail::blockingWait(*state_);
        return state_->result();
    }

    Result<T> take() && {
        requireValid();
        detail::blockingWait(*state_);
        return state_->takeResult();
    }

    void onReady(std::function<void(const Result<T>&)> fn) {
        requireValid();
        state_->addContinuation(std::move(fn));
    }

    // True when both handles observe the same task (equal-key coalescing).
    bool sharesStateWith(const Future& other) const noexcept {
        return state_ != nullptr && state_ == other.state_;
    }

private:
    void requireValid() const {
        if (state_ == nullptr) {
            ESSES_FATAL("use of an invalid (default-constructed) Future");
        }
    }

    std::shared_ptr<detail::SharedState<T>> state_;
};

template <class T> Future<T> makeReadyFuture(Result<T> r) {
    auto state = std::make_shared<detail::SharedState<T>>();
    state->set(std::move(r));
    return Future<T>(std::move(state));
}

} // namespace esses
