// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsScheduler (03 section 7.4): submits node evaluations to the TaskPool at the requester's priority under
// share keys that carry the chunk (".../all" for a task producing every chunk, ".../c<chunkNo>" for one chunk
// on the v1.0 chunk path). TaskPool coalesces by the key text: a repeat joins the queued or running task,
// adds its CancelToken as one more requester and raises the task's priority. MathTaskKey::node (the hash of
// the cache key) identifies the task in code; the text is what the pool and the logs see.

#include <esses/base/taskpool.hpp>
#include <esses/maths/maths_scheduler.hpp>

#include <string>
#include <utility>

namespace esses::maths {

Future<ColumnHandle> MathsScheduler::submit(const MathTaskKey& key, const std::string& text,
                                            Priority priority, const CancelToken& cancel,
                                            std::function<Result<ColumnHandle>(const CancelToken&)> work) {
    TaskPool* pool = pool_ != nullptr ? pool_ : TaskPool::installed();
    if (pool == nullptr) {
        return makeReadyFuture<ColumnHandle>(fail(ErrorCode::Invalid, "no TaskPool for maths evaluation"));
    }
    if (!work) {
        return makeReadyFuture<ColumnHandle>(fail(ErrorCode::Invalid, "maths task without work"));
    }
    if (text.empty()) {
        return makeReadyFuture<ColumnHandle>(fail(ErrorCode::Invalid, "maths task without a key"));
    }
    std::string full = text;
    full += key.chunk == MathTaskKey::kAll ? std::string("/all") : "/c" + std::to_string(key.chunk);
    return pool->submit(priority, TaskKey{std::move(full)}, cancel,
                        [fn = std::move(work)](const CancelToken& token) { return fn(token); });
}

} // namespace esses::maths
