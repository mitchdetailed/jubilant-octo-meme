// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AppRuntime (01 section 5.3 rule 11, 06 section 18 start-up sequence, D-123): owns the process TaskPool,
// ChunkCache, Dispatcher and log sinks; constructed in main() right after QApplication/QGuiApplication and
// before anything uses them; installs the pool and cache (TaskPool::instance(), ChunkCache::instance()).
// shutdown() runs, in order: pool.cancelAll(""); pool.join(); remove the Messages log sink;
// dispatcher.close(); cache.clear(); uninstall. Only then are the runtime and the application destroyed.
// esses (GUI) marks its GUI thread ThreadRole::NoBlock; esses-render and the golden tests do not. The role
// the constructing thread had before is restored by shutdown() (on that thread), so a runtime that marked
// the thread does not leave the mark behind (tests construct several runtimes in one process).
#pragma once

#include <esses/base/log.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/store/chunk_cache.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace esses::qt {

struct AppRuntimeOptions {
    unsigned workers = 0;                   // 0: TaskPool::defaultWorkerCount()
    std::optional<std::size_t> cacheBudget; // nullopt: ChunkCache default (25 % of RAM, min 1 GiB)
    bool markGuiThreadNoBlock = false;      // true in esses only (01 section 5.3 rule 4)
};

class AppRuntime {
public:
    explicit AppRuntime(const AppRuntimeOptions& options = {});
    ~AppRuntime(); // calls shutdown() when the owner did not
    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;

    TaskPool& pool() noexcept { return *pool_; }
    store::ChunkCache& cache() noexcept { return *cache_; }
    Dispatcher& dispatcher() noexcept { return *dispatcher_; }

    // Adds a log sink owned by the runtime (the Messages dock's sink; removed first on shutdown).
    void addSink(std::shared_ptr<ILogSink> sink);
    // Removes a sink added with addSink before shutdown (a window that owns it closes); no-op if unknown.
    void removeSink(const std::shared_ptr<ILogSink>& sink);
    void shutdown();
    bool isShutDown() const noexcept { return shutDown_; }

private:
    std::unique_ptr<store::ChunkCache> cache_;
    std::unique_ptr<TaskPool> pool_;
    std::unique_ptr<Dispatcher> dispatcher_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
    bool shutDown_ = false;
    bool markedNoBlock_ = false;
    ThreadRole::Kind previousRole_ = ThreadRole::Normal;
};

} // namespace esses::qt
