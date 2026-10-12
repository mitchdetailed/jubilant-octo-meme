// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Cooperative cancellation (D-031, 01 section 5.4). CancelSource is the owner side; CancelToken is a cheap,
// copyable, shared view checked at chunk/block granularity. A default-constructed token never cancels.
// std::stop_token is deliberately not used (01 section 5.1).
#pragma once

#include <esses/base/config.hpp>

#include <atomic>
#include <memory>
#include <utility>

namespace esses {

namespace detail {
class CancelState {
public:
    CancelState() = default;
    CancelState(const CancelState&) = delete;
    CancelState& operator=(const CancelState&) = delete;
    virtual ~CancelState() = default;
    virtual bool isCancelled() const noexcept = 0;
};

class FlagCancelState final : public CancelState {
public:
    // Sequentially consistent, so a cancel() that precedes another seq_cst access in the cancelling thread is
    // seen by the next checkpoint of a task that observes that access (no store-load reordering).
    bool isCancelled() const noexcept override { return flag_.load(); }
    void cancel() noexcept { flag_.store(true); }

private:
    std::atomic<bool> flag_{false};
};
} // namespace detail

class CancelToken {
public:
    CancelToken() noexcept = default; // never cancelled
    explicit CancelToken(std::shared_ptr<const detail::CancelState> state) noexcept
        : state_(std::move(state)) {}

    bool cancelled() const noexcept { return state_ != nullptr && state_->isCancelled(); }
    // Same as cancelled(); named for readability at chunk/block boundaries.
    bool checkpoint() const noexcept { return cancelled(); }
    // False for a default-constructed token, which can never fire.
    bool canBeCancelled() const noexcept { return state_ != nullptr; }

    static CancelToken none() noexcept { return {}; }

private:
    std::shared_ptr<const detail::CancelState> state_;
};

class CancelSource {
public:
    CancelSource() : state_(std::make_shared<detail::FlagCancelState>()) {}

    void cancel() noexcept { state_->cancel(); }
    bool cancelled() const noexcept { return state_->isCancelled(); }
    CancelToken token() const noexcept { return CancelToken(state_); }

private:
    std::shared_ptr<detail::FlagCancelState> state_;
};

} // namespace esses
