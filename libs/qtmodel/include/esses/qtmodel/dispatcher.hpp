// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dispatcher (01 section 5.3 rule 3, D-031 as refined by D-123; AC-M1-12): the only worker -> GUI bridge. One
// QObject living on the GUI thread. post(context, fn) is thread-safe: it captures a QPointer to `context`,
// enqueues the closure and wakes the GUI thread with a queued call; the GUI drains the queue in FIFO order
// and skips a closure whose context has been destroyed. Closures capture identifiers and values, never raw
// pointers to model or widget objects. After close() (AppRuntime::shutdown) pending closures are discarded
// and new posts are ignored. assertGuiThread() is the debug check behind "the GUI never blocks on a Future".
// Services that can be destroyed while their tasks run (MathsService, DistanceService, SourceLoader,
// RenderService) post with the Dispatcher itself as `context` and carry a QPointer to themselves made on the
// GUI thread when the task was submitted: a QPointer must never be created from a worker for an object the
// GUI thread may be deleting (a cancelAll in a destructor runs continuations inline).
#pragma once

#include <QObject>
#include <QPointer>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace esses::qt {

class Dispatcher final : public QObject {
    Q_OBJECT
public:
    explicit Dispatcher(QObject* parent = nullptr);
    ~Dispatcher() override;

    // Any thread. `context` must be non-null; the closure runs on the GUI thread only while it is alive.
    void post(QObject* context, std::function<void()> fn);
    // GUI thread: discard pending closures and refuse new ones (01 section 5.3 rule 11).
    void close();
    bool closed() const;
    std::size_t pending() const;

    // True on the thread that owns the Dispatcher (the GUI thread).
    bool isGuiThread() const noexcept;
    // Debug builds: ESSES_ASSERT(isGuiThread()). Release: no-op.
    void assertGuiThread() const noexcept;

    // Runs every queued closure now (GUI thread; tests and shutdown paths).
    void drain();

private:
    struct Item {
        QPointer<QObject> context;
        std::function<void()> fn;
    };
    mutable std::mutex mutex_;
    std::deque<Item> queue_;
    bool closed_ = false;
    bool wakePending_ = false;
};

} // namespace esses::qt
