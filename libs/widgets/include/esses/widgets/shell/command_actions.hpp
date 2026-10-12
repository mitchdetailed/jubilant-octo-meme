// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandActions (06 section 14.1 CommandRegistry::action, 06 section 1 rule 5, 06 section 2.2): one shared
// QAction per command id for menus, toolbars and context menus, kept in sync with the registry (title,
// checkable/checked, enabled on menu show and on refresh()) and showing the active keymap's first key
// (KeymapBinder::keysFor) as display text only ("Title\tKey" in menus, "Title (Key)" in tool tips): key
// events are dispatched by KeyEventRouter, never by QAction shortcuts, so context resolution stays in one
// place. Lives here because QAction and QKeySequence are Qt Gui types (esses_qtmodel links Qt Core only).
// setNote() adds a line to a command's tool tip ("planned for M3": 06 section 14.4 registers planned rows
// bound and greyed, with a tool tip).
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QAction;
class QMenu;
class QMenuBar;
class QToolBar;

namespace esses::qt {
class CommandRegistry;
class KeymapBinder;
} // namespace esses::qt

namespace esses::widgets {

class CommandActions final : public QObject {
    Q_OBJECT
public:
    // The top-level menus of 06 section 2.2, in order.
    static const QStringList& menuOrder();

    CommandActions(qt::CommandRegistry& registry, qt::KeymapBinder& keys, QObject* parent = nullptr);
    ~CommandActions() override;

    // The shared action of a command (created on first use); nullptr for an unknown id.
    QAction* action(const QString& id);
    // Builds the menu bar from the registry's menuPath values ("Component/Zoom" = submenu Zoom of Component):
    // menus in menuOrder() (others after them), items in `itemOrder` (command ids; e.g. the keymap row order)
    // and then by id; commands in `skip` (those that need arguments, e.g. view.worksheet_n) are left out.
    // Every menu refreshes its actions on aboutToShow.
    void populateMenuBar(QMenuBar& bar, const QStringList& itemOrder = {}, const QStringList& skip = {});
    void populateToolBar(QToolBar& bar, const QStringList& ids);
    // Refreshes enabled/checked states and key texts of every action (menu aboutToShow, selection changes).
    void refresh();
    void refresh(const QString& id);
    // The menu built for a top-level title ("File", "Data", ...), or nullptr.
    QMenu* menu(const QString& title) const;
    // An extra tool-tip line for a command (shown after the title and key).
    void setNote(const QString& id, const QString& note);

private:
    void updateText(const QString& id, QAction& a);

    qt::CommandRegistry& registry_;
    qt::KeymapBinder& keys_;
    QHash<QString, QPointer<QAction>> actions_;
    QHash<QString, QPointer<QMenu>> menus_;
    QHash<QString, QString> notes_;
};

} // namespace esses::widgets
