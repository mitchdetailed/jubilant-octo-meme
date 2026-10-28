// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HiDpiShell (M4 hidpi package; D-206, D-209; 07 section 7.1 row 3, AC-M4-07; 06 section 17 items 3-4):
// watches the main window and every dock window (DockHost::windowOf: a floating dock's own top-level window)
// with a DprWatcher, so a per-monitor DPR change re-lays out and re-renders the components there at the new
// DPR. A dock that floats, docks back or is restored moves its widgets into another window without any DPR
// event from Qt; on every DockHost::stateChanged the shell follows the dock's window and, one event-loop turn
// later (once the docking library has moved the widgets), re-checks every watched window
// (DprWatcher::refreshAll), so a component taken from a DPR 2 monitor to a DPR 1 one (or back) rebuilds at
// the DPR of the window it is now in. Everything is let go in teardown(). GUI thread only. Files of the
// package: docs/milestones/M4.md section 3.
#pragma once

#include <esses/widgets/shell/shell_extension.hpp>

#include <QMetaObject>
#include <QPointer>
#include <QString>

namespace esses::widgets {

class DprWatcher;

class HiDpiShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "hidpi";

    explicit HiDpiShell(MainWindow& window);
    ~HiDpiShell() override;

    // The watcher of the window and its dock windows (nullptr before windowReady and after teardown).
    DprWatcher* watcher() const;
    // Re-checks run after dock state changes so far, and the components they asked to rebuild (tests: the
    // path Qt does not notify; a docked-back component whose size changed has rebuilt at the new DPR through
    // its resize already, so the re-check finds it current).
    int dockRefreshes() const noexcept { return dockRefreshes_; }
    int dockRefreshRebuilds() const noexcept { return dockRebuilds_; }

    void windowReady() override;
    void teardown() override;

private:
    void followDock(const QString& id);
    void queueRefresh();

    QPointer<DprWatcher> watcher_;
    QMetaObject::Connection stateChanged_;
    bool refreshQueued_ = false;
    bool tornDown_ = false;
    int dockRefreshes_ = 0;
    int dockRebuilds_ = 0;
};

} // namespace esses::widgets
