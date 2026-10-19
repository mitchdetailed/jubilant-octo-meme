// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// What a LaunchRequest does to the window (06 sections 18-19; D-113, D-212):
//   * openRequestedFiles(): 06 section 18 steps 3-6. With a project, its recovery prompt comes first,
//     then the -l logs (else its previous data) and the last selection; without one, or when it does not
//     open, the logs. A forwarded request (the running instance, section 19 item 6) whose project is the one
//     already open adds its logs to it; another project is opened as File > Open Project would (leaving the
//     open one may ask).
//   * applyStartOptions(): --scheme and --keymap at the start of an instance, each problem a Warning row in
//     Messages (origin System, code "launch.scheme" / "launch.keymap") and returned.
//       --scheme <name>: a scheme of the scheme selector by its name (case-insensitive), or "light"/"dark"
//       for the shipped ones, chosen as the toolbar's selector chooses it (it stays the active scheme).
//       --keymap <name|file>: a keymap-1 file (an existing path, else <config>/keymaps/<name>.json, else
//       <data>/keymaps/<name>.json) on top of the shipped default keymap, for this run only (the binder's
//       bindings; settings and the user's keymap.json are not touched).
//   * bringToFront(): a forwarded request shows the window (restored when minimised), raises and
//     activates it.
//   * RequestQueue (D-217): forwarded requests are applied only at the top of the event loop. One that
//   arrives
//     while a modal dialog, a menu or any other nested event loop runs (a file prompt holding a component,
//     "Save the changes?") waits until it has ended, and requests are applied one at a time (a request's own
//     prompt never runs the next one inside it).
//   * A forwarded project switch the user cancels ("Save the changes?" answered Cancel) opens none of its
//   logs
//     into the project that stays (an Info row says so; D-217).
//   * reportNoInstanceChannel(): a window that could not listen for later launches says so in Messages
//     (Warning, code "launch.instance"; D-217): those launches open windows of their own.
#pragma once

#include "launch_request.hpp"

#include <QString>
#include <QStringList>
#include <QTimer>
#include <deque>
#include <functional>

class QWidget;

namespace esses::widgets {
class MainWindow;
} // namespace esses::widgets

namespace esses::app {

enum class LaunchKind { Start, Forwarded };

void openRequestedFiles(widgets::MainWindow& window, const LaunchRequest& request, LaunchKind kind);

// Empty on success, else the problem (also posted to Messages).
QString applyScheme(widgets::MainWindow& window, const QString& name);
QString applyKeymap(widgets::MainWindow& window, const QString& nameOrFile);
QStringList applyStartOptions(widgets::MainWindow& window, const LaunchRequest& request);

void bringToFront(QWidget& window);

void reportNoInstanceChannel(widgets::MainWindow& window, const QString& detail);

class RequestQueue {
public:
    using Apply = std::function<void(const LaunchRequest&)>;
    using Busy = std::function<bool()>;
    static constexpr int kRetryMs = 200;

    // `topLevel`: the event-loop level of the application's own loop (1 inside QApplication::exec; a test
    // without exec runs at 0). `busy` replaces the check of a modal widget, a popup or a deeper loop (tests).
    explicit RequestQueue(Apply apply, int topLevel = 1, Busy busy = {});
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    void enqueue(LaunchRequest request);
    int pending() const { return static_cast<int>(queue_.size()); }
    int applied() const { return applied_; }
    // A modal widget, an open popup (a menu), or an event loop deeper than `topLevel`.
    static bool guiBusy(int topLevel);

private:
    void tryApply();

    Apply apply_;
    int topLevel_;
    Busy busy_;
    std::deque<LaunchRequest> queue_;
    bool applying_ = false;
    int applied_ = 0;
    QTimer retry_;
};

} // namespace esses::app
