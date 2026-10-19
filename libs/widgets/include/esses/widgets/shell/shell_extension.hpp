// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ShellExtension (M4 contract, D-206; docs/milestones/M4.md sections 2-3): the one hook through which the M4
// packages wire the shell without editing main_window.cpp, main_window_m3.cpp, main_window.hpp or
// main_window_impl.hpp. MainWindow creates one extension per M4 package (src/shell/main_window_m4.cpp, in
// this order: budget, reload, hidpi, followups, about, docs) as QObject children named "m4.<key>", keeps them
// in its private state and calls their virtuals at fixed points of its life:
//   buildWidgets(central)    end of MainWindow::buildWidgets (after the M3 widgets; the docks are
//                            registered, the status-bar fields exist)
//   registerCommands()       after the M3 commands, before every keymap command still unregistered is
//                            added as planned (a command an extension registers is never "planned for M4")
//   buildMenus()             after the menus and the M3 menus exist (CommandActions::menu("Help") etc.)
//   windowReady()            end of the constructor: the layout is restored, the canvas has the focus
//   worksheetActivated(i)    after the M3 followers moved to worksheet i; -1 before the worksheets are
//                            rebuilt (also during the constructor, before windowReady)
//   teardown()               first thing in ~MainWindow, before any widget, dock or model goes:
//                            disconnect, stop timers, let go of the runtime
// An extension holds its own state (members of its class) and reaches the window's through window() (the
// public accessors) and impl() (MainWindow::Impl, src/shell/main_window_impl.hpp; write `auto& d = impl();`,
// the type is private to MainWindow): read it, and add to its containers only where the M4 note says so,
// e.g. impl().status for a status-bar field. Tests find an extension with shellExtension<T>(window).
#pragma once

#include <esses/widgets/shell/main_window.hpp>

#include <QObject>
#include <QString>

class QDialog;
class QWidget;

namespace esses::widgets {

class ShellExtension : public QObject {
    Q_OBJECT
public:
    ~ShellExtension() override;
    ShellExtension(const ShellExtension&) = delete;
    ShellExtension& operator=(const ShellExtension&) = delete;

    // The package key ("budget", "reload", "hidpi", "followups", "about", "docs"); the QObject name is
    // "m4." + key.
    const QString& key() const noexcept { return key_; }

    virtual void buildWidgets(QWidget& central);
    virtual void registerCommands();
    virtual void buildMenus();
    virtual void windowReady();
    virtual void worksheetActivated(int index);
    virtual void teardown();

protected:
    ShellExtension(MainWindow& window, const char* key);
    MainWindow& window() const noexcept { return window_; }
    // The window's private state (src/shell/main_window_impl.hpp; MainWindow befriends this class).
    MainWindow::Impl& impl() const;
    // MainWindow's private helpers the extensions share: the transient status-bar hint (lastHint() reads it)
    // and a modeless editor shown with its geometry kept in layout.json.
    void hint(const QString& text) const;
    void showEditor(QDialog& dialog, const char* geometryKey) const;

private:
    MainWindow& window_;
    QString key_;
};

// The extension of type T of a window (nullptr when it has none).
template <class T> T* shellExtension(const QObject& window) {
    return window.findChild<T*>(QString(), Qt::FindDirectChildrenOnly);
}

} // namespace esses::widgets
