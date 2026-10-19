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
#pragma once

#include "launch_request.hpp"

#include <QString>
#include <QStringList>

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

} // namespace esses::app
