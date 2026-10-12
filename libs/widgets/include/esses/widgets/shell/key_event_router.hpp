// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// KeyEventRouter (06 sections 3.3 and 14.2; AC-M1-06): the application-wide event filter (KeyPress on
// QApplication) that turns a key event into Qt portable key text and the focus chain, and asks
// qt::KeymapBinder to dispatch it:
//   modal dialog active                 -> no keymap handling
//   focus inside a dock                 -> [the dock's context, Global]; unmodified letters, digits, arrows,
//                                          ENTER, SPACE, HOME/END, PAGE keys (SHIFT allowed) go to the
//                                          focused widget first and reach the binder only when it ignores
//                                          them (the event propagating to the widget's ancestors); function
//                                          keys, ESC, DELETE and CTRL/ALT chords go to the binder first
//   focus inside the WorksheetCanvas    -> [the component's commandContext() (Graph | Summary | Track),
//                                          Component, Worksheet, Global]; keys go to the binder first,
//                                          except plain keys in a widget whose property "esses.textEntry" is
//                                          true
//   focus elsewhere in the scope window -> [Global] (the worksheet tab bar, the workbook selector, the tool
//                                          bar); no focus widget at all -> [Worksheet, Global]
//   focus in another window             -> function keys and chords only, [Global]
// The first context with a binding wins; when its command runs the event is accepted. Split from
// KeymapBinder because key events are Qt Gui types (see qt::KeymapBinder).
#pragma once

#include <esses/qtmodel/command_registry.hpp>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QEvent;
class QKeyEvent;
class QWidget;

namespace esses::qt {
class KeymapBinder;
} // namespace esses::qt

namespace esses::widgets {

class ComponentWidget;
class DockHost;
class WorksheetCanvas;

class KeyEventRouter final : public QObject {
    Q_OBJECT
public:
    KeyEventRouter(qt::KeymapBinder& binder, DockHost& docks, WorksheetCanvas& canvas,
                   QObject* parent = nullptr);
    ~KeyEventRouter() override;

    // The window whose focus chains this router computes (the MainWindow; floating docks belong to it).
    // Null: every window.
    void setScope(QWidget* window) { scope_ = window; }

    // Portable text of a key event ("Ctrl+Shift+C", "F9", "Return"); empty for a lone modifier.
    static QString keyText(const QKeyEvent& event);
    // An unmodified (SHIFT allowed) letter, digit, arrow, ENTER, SPACE, HOME/END, PAGE key or other
    // printable character: offered to a focused dock widget before the binder (06 section 14.2 item 3).
    static bool widgetFirst(const QKeyEvent& event);
    // The context chain for a focus widget (06 section 14.2 item 2).
    QList<qt::CommandContext> chainFor(const QWidget* focus) const;
    // Resolves and invokes a key for a focus widget as the filter does (tests, synthetic events).
    bool dispatch(const QString& keyText, const QWidget* focus);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool inScope(const QWidget* widget) const;
    QString dockOf(const QWidget* widget) const;

    qt::KeymapBinder& binder_;
    QPointer<DockHost> docks_;
    QPointer<WorksheetCanvas> canvas_;
    QPointer<QWidget> scope_;
    // The plain key event offered to a dock widget (resolved when it propagates to an ancestor of
    // offeredTo_), and the event already resolved at doneAt_ (later visits of its ancestors are skipped).
    const QEvent* offered_ = nullptr;
    QPointer<QWidget> offeredTo_;
    QList<qt::CommandContext> offeredChain_;
    const QEvent* done_ = nullptr;
    QPointer<QWidget> doneAt_;
};

} // namespace esses::widgets
