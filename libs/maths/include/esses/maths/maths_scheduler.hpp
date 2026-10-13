// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsScheduler and MathTaskKey (03 section 7.4, 01 section 5.4): Qt-free scheduling of node evaluations on
// the TaskPool at the requester's priority (01 section 5.2 table; no bands of its own). Tasks coalesce
// through share keys that carry the chunk:
// maths/<src>/<layer>/<channelKey>/<version>[/<selectionKey>]/c<chunkNo> for a Parallel chunk (v1.0) and
// .../all for a task producing every chunk (Ordered, Whole, every 0.1 task). Upstream inputs still absent are
// materialised inline (Rule W); continuations are submitted from the owning thread. Owned by MathGraph's host
// (MathsService in the GUI, the CLI directly).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/future.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/channel_key.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::maths {

struct MathTaskKey {
    static constexpr std::uint32_t kAll = std::numeric_limits<std::uint32_t>::max();
    Hash128 node;               // xxh3 of the cache key (ChannelKey, version[, selectionKey])
    std::uint32_t chunk = kAll; // kAll: the task produces every chunk
    friend bool operator==(const MathTaskKey&, const MathTaskKey&) = default;
};

class MathsScheduler {
public:
    // nullptr: TaskPool::installed() at submit time.
    explicit MathsScheduler(TaskPool* pool = nullptr) noexcept : pool_(pool) {}

    // Submits `work` under the share key `text` + "/all" (or "/c<chunk>") at `priority`: a repeat joins the
    // queued or running task, adds `cancel` as one more requester and raises its priority. `text` is the key
    // prefix maths/<src>/<layer>/<name>/<version>[/<selectionKey>]; MathTaskKey::node names the task in code.
    // Ready failures (Invalid) without a pool, without work or with an empty prefix; never blocks.
    Future<ColumnHandle> submit(const MathTaskKey& key, const std::string& text, Priority priority,
                                const CancelToken& cancel,
                                std::function<Result<ColumnHandle>(const CancelToken&)> work);

    TaskPool* pool() const noexcept { return pool_; }

private:
    TaskPool* pool_ = nullptr;
};

} // namespace esses::maths
