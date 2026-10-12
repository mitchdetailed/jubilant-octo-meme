// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// QDockWidgetBackend (06 section 3.1, D-026, D-153): the DockHost backend over QMainWindow::addDockWidget and
// saveState/restoreState. The fallback: selected with the environment variable
// ESSES_DOCK_BACKEND=qdockwidget, and the only backend of a build configured with
// -DESSES_DOCK_BACKEND=qdockwidget (an OS on which spike S-3 fails). AC-M1-13 runs on it as on the ADS
// backend.
//   Docked    a QDockWidget in its area (tabbed with the first dock registered in the same area)
//   Floating  QDockWidget::setFloating(true)
//   AutoHide  hidden, plus a checkable button on a side tool bar of the dock's area (objectName
//             "esses.autohide.<left|right|top|bottom>"); the button or DockHost::focus flies the dock out
//             (shown docked, raised, state still AutoHide) until the focus leaves it
//   Closed    hidden
// The perspective is a small JSON envelope {"backend": "qdockwidget", "version": 1, "mainWindow": <base64 of
// QMainWindow::saveState>, "docks": {"<id>": "<state>"}} so that AutoHide survives a restore and a
// perspective of another backend is refused (restoreState() false, the caller resets to the default layout).
#pragma once

#include <esses/widgets/shell/dock_host.hpp>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <utility>

class QAction;
class QDockWidget;
class QMainWindow;
class QToolBar;
class QWidget;

namespace esses::widgets {

class QDockWidgetBackend final : public QObject, public IDockBackend {
public:
    static constexpr int kStateVersion = 1; // QMainWindow::saveState/restoreState version

    explicit QDockWidgetBackend(QMainWindow& host);
    ~QDockWidgetBackend() override;

    void setCentral(QWidget* central) override; // QMainWindow::setCentralWidget
    void addDock(const QString& id, const QString& title, QWidget* content, DockArea area,
                 DockState state) override;
    void setState(const QString& id, DockState state) override;
    DockState state(const QString& id) const override;
    void resetDock(const QString& id, DockArea area, DockState state) override;
    void focus(const QString& id) override;
    bool isFlownOut(const QString& id) const override;
    QByteArray saveState() const override;
    bool restoreState(const QByteArray& state) override;
    QWidget* content(const QString& id) const override;
    QWidget* container(const QString& id) const override;
    QString dockIdOf(const QWidget* widget) const override;
    QWidget* focusedDock() const override;
    void setStateObserver(StateObserver observer) override { observer_ = std::move(observer); }
    const char* name() const override { return "qdockwidget"; }

    // The side tool bar of `area` (tests: the AutoHide button of a dock is its action, objectName
    // "esses.autohide.<id>").
    QToolBar* sideBar(DockArea area) const;
    QAction* autoHideButton(const QString& id) const;

private:
    struct Entry {
        QPointer<QDockWidget> dock;
        DockArea area = DockArea::Left;
        DockState state = DockState::Closed;
        QPointer<QAction> button; // AutoHide side button
    };
    void apply(const QString& id, Entry& e, DockState state, bool raise);
    void placeInArea(const QString& id, Entry& e, DockArea area);
    void addButton(const QString& id, Entry& e);
    void removeButton(Entry& e);
    void flyOut(const QString& id);
    void collapse();
    void updateSideBars();
    void notify(const QString& id, DockState state);
    void onClosed(const QString& id);
    void onTopLevelChanged(const QString& id, bool floating);
    void onFocusChanged(QWidget* now);

    QMainWindow& host_;
    QStringList order_;
    QHash<QString, Entry> docks_;
    QHash<int, QPointer<QToolBar>> sideBars_; // by DockArea
    QString flownOut_;
    int applying_ = 0; // > 0 while this backend changes a dock (its own signals are not user changes)
    StateObserver observer_;
};

} // namespace esses::widgets
