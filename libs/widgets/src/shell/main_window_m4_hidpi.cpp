// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HiDpiShell (hidpi_shell.hpp): the M4 hidpi package's shell wiring (docs/milestones/M4.md section 3; D-209).
// The window's state is reached with `auto& d = impl();`.

#include "main_window_impl.hpp"

#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/dpr_watcher.hpp>
#include <esses/widgets/shell/hidpi_shell.hpp>

#include <QWidget>

namespace esses::widgets {

HiDpiShell::HiDpiShell(MainWindow& window) : ShellExtension(window, kKey) {}

HiDpiShell::~HiDpiShell() = default;

DprWatcher* HiDpiShell::watcher() const {
    return watcher_.data();
}

void HiDpiShell::windowReady() {
    if (tornDown_ || watcher_ != nullptr) {
        return;
    }
    watcher_ = new DprWatcher(this);
    watcher_->setObjectName(QStringLiteral("m4.hidpi.watcher"));
    watcher_->watch(&window());
    auto& d = impl();
    if (d.docks == nullptr) {
        return;
    }
    // A perspective restored before now may already have floated docks.
    for (const QString& id : d.docks->dockIds()) {
        followDock(id);
    }
    stateChanged_ = connect(d.docks, &DockHost::stateChanged, this, [this](const QString& id) {
        followDock(id);
        queueRefresh();
    });
}

void HiDpiShell::followDock(const QString& id) {
    auto& d = impl();
    if (watcher_ == nullptr || d.docks == nullptr) {
        return;
    }
    QWidget* w = d.docks->windowOf(id);
    if (w != nullptr && w != &window()) {
        watcher_->watch(w); // a floating dock's window; an already watched one is ignored
    }
}

// The docking library moves a dock's widgets during and right after the state change; the re-check runs once
// the event loop has delivered that, and follows every dock again (a floating window created meanwhile).
void HiDpiShell::queueRefresh() {
    if (refreshQueued_ || tornDown_) {
        return;
    }
    refreshQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            refreshQueued_ = false;
            auto& d = impl();
            if (tornDown_ || watcher_ == nullptr || d.docks == nullptr) {
                return;
            }
            for (const QString& id : d.docks->dockIds()) {
                followDock(id);
            }
            ++dockRefreshes_;
            dockRebuilds_ += watcher_->refreshAll();
        },
        Qt::QueuedConnection);
}

void HiDpiShell::teardown() {
    tornDown_ = true;
    QObject::disconnect(stateChanged_);
    delete watcher_.data(); // removes its event filters before any window goes
}

} // namespace esses::widgets
