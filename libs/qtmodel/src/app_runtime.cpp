// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Implemented in the skeleton (start-up and shutdown order are shared by esses, esses-render and the Qt
// tests); the qtmodel agent owns it from here.

#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/app_runtime.hpp>

#include <algorithm>
#include <utility>

namespace esses::qt {

AppRuntime::AppRuntime(const AppRuntimeOptions& options)
    : cache_(options.cacheBudget ? std::make_unique<store::ChunkCache>(*options.cacheBudget)
                                 : std::make_unique<store::ChunkCache>()),
      pool_(std::make_unique<TaskPool>(options.workers == 0 ? TaskPool::defaultWorkerCount()
                                                            : options.workers)),
      dispatcher_(std::make_unique<Dispatcher>()) {
    store::ChunkCache::install(cache_.get());
    TaskPool::install(pool_.get());
    if (options.markGuiThreadNoBlock) {
        previousRole_ = ThreadRole::current();
        markedNoBlock_ = true;
        ThreadRole::set(ThreadRole::NoBlock);
    }
}

AppRuntime::~AppRuntime() {
    shutdown();
}

void AppRuntime::addSink(std::shared_ptr<ILogSink> sink) {
    Log::addSink(sink);
    sinks_.push_back(std::move(sink));
}

void AppRuntime::removeSink(const std::shared_ptr<ILogSink>& sink) {
    const auto it = std::find(sinks_.begin(), sinks_.end(), sink);
    if (it == sinks_.end()) {
        return;
    }
    Log::removeSink(*it);
    sinks_.erase(it);
}

void AppRuntime::shutdown() {
    if (shutDown_) {
        return;
    }
    shutDown_ = true;
    pool_->cancelAll("");
    pool_->join();
    for (const auto& sink : sinks_) {
        Log::removeSink(sink);
    }
    sinks_.clear();
    dispatcher_->close();
    cache_->clear();
    if (TaskPool::installed() == pool_.get()) {
        TaskPool::install(nullptr);
    }
    if (store::ChunkCache::installed() == cache_.get()) {
        store::ChunkCache::install(nullptr);
    }
    if (markedNoBlock_) {
        ThreadRole::set(previousRole_); // shutdown runs on the GUI thread that the constructor marked
        markedNoBlock_ = false;
    }
}

} // namespace esses::qt
