// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dispatcher (01 section 5.3 rule 3, D-123; AC-M1-12): the one worker -> GUI bridge. Tests:
// tests/tst_dispatcher.cpp (FIFO order, dead contexts, cross-thread posts, close, the NoBlock assertion and
// the shutdown with builds in flight of 01 section 5.3 rule 11).

#include <esses/base/assert.hpp>
#include <esses/qtmodel/dispatcher.hpp>

#include <QMetaObject>
#include <QThread>

namespace esses::qt {

Dispatcher::Dispatcher(QObject* parent) : QObject(parent) {}

Dispatcher::~Dispatcher() = default;

void Dispatcher::post(QObject* context, std::function<void()> fn) {
    if (context == nullptr || !fn) {
        return;
    }
    bool wake = false;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push_back(Item{QPointer<QObject>(context), std::move(fn)});
        if (!wakePending_) {
            wakePending_ = true;
            wake = true;
        }
    }
    if (wake) {
        QMetaObject::invokeMethod(this, &Dispatcher::drain, Qt::QueuedConnection);
    }
}

void Dispatcher::close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    queue_.clear();
}

bool Dispatcher::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

std::size_t Dispatcher::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

bool Dispatcher::isGuiThread() const noexcept {
    return QThread::currentThread() == thread();
}

void Dispatcher::assertGuiThread() const noexcept {
    ESSES_ASSERT(isGuiThread());
}

void Dispatcher::drain() {
    assertGuiThread(); // closures touch QObjects: only on the owning (GUI) thread (01 section 5.3 rule 1)
    std::deque<Item> batch;
    {
        std::lock_guard lock(mutex_);
        wakePending_ = false;
        batch.swap(queue_);
    }
    for (Item& item : batch) {
        if (!item.context.isNull()) {
            item.fn();
        }
    }
}

} // namespace esses::qt
