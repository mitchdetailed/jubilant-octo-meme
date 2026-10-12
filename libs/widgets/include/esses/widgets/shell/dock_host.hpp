// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DockHost (06 sections 3.1-3.5, D-026; AC-M1-13: save/restore perspective, auto-hide, float/dock pass
// offscreen; for any OS on which spike S-3 failed the QDockWidget fallback passes the same test). A facade:
// no docking library type appears outside the backend implementation. IDockBackend is the seam (D-153):
//   AdsBackend          the Qt Advanced Docking System (CDockManager, CDockWidget, CAutoHideDockContainer;
//                       ads_backend.hpp), the default whenever the build has ADS (CMake ESSES_DOCK_BACKEND)
//   QDockWidgetBackend  QMainWindow::addDockWidget/saveState, AutoHide emulated with a side tool bar button
//                       (qdockwidget_backend.hpp), the fallback
// makeDefaultBackend() takes the build's default; the environment variable ESSES_DOCK_BACKEND ("ads" or
// "qdockwidget") selects another backend the build has at run time. Dock registry (06 section 3.2): data
// (F8, Left), channels (C, Left tabbed under Data), values (V, Right), trackmap (M3), channelstatus (M2),
// messages (Bottom, AutoHide); a dock registered in the same default area as an earlier one is tabbed with
// it. Focus model: 06 section 3.3 (an AutoHide dock focused by command flies out and stays out until focus
// leaves it).
//
// Additions to the 06 section 3.1 sketch: IDockBackend::setCentral/resetDock/isFlownOut/container/dockIdOf/
// setStateObserver/libraryVersion (the worksheet area, Reset Dock Windows, the fly-out of item 5, the focus
// chain of the KeyEventRouter, state changes made with the dock's own buttons, layout-1 "adsVersion"),
// DockHost::isVisible/dockIds/defaultArea/defaultState/focusedDockId/dockIdOf/content, the stateChanged()
// signal and the backend choice (availableBackends/defaultBackendName/makeBackend); dockStateName/
// parseDockState give the layout-1 spelling ("Docked", "Floating", "AutoHide", "Closed").
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QMainWindow;
class QWidget;

namespace esses::widgets {

enum class DockArea : std::uint8_t { Left, Right, Top, Bottom, Center };
enum class DockState : std::uint8_t { Docked, Floating, AutoHide, Closed };

// layout-1 spelling (02 section 6.7): "Docked", "Floating", "AutoHide", "Closed".
QString dockStateName(DockState state);
std::optional<DockState> parseDockState(const QString& name);

class IDockBackend {
public:
    // Called after a state change the backend did not get through setState (the dock's own close and float
    // buttons, a title-bar drag, a fly-out collapsing).
    using StateObserver = std::function<void(const QString& id, DockState state)>;

    virtual ~IDockBackend() = default;
    // The worksheet area (06 section 2.1), set once before the first addDock: the host's central widget, or
    // the docking library's central area that docks surround and never cover.
    virtual void setCentral(QWidget* central) = 0;
    virtual void addDock(const QString& id, const QString& title, QWidget* content, DockArea area,
                         DockState state) = 0;
    virtual void setState(const QString& id, DockState state) = 0;
    virtual DockState state(const QString& id) const = 0;
    // Back to `area` in `state`: floating windows re-docked, auto-hide buttons rebuilt (06 section 3.5).
    virtual void resetDock(const QString& id, DockArea area, DockState state) = 0;
    // Show + keyboard focus to the dock's primary widget; an AutoHide dock flies out (state unchanged) and
    // collapses when focus leaves it (06 section 3.3 item 5).
    virtual void focus(const QString& id) = 0;
    virtual bool isFlownOut(const QString& id) const = 0;
    virtual QByteArray saveState() const = 0; // opaque perspective
    virtual bool restoreState(const QByteArray& state) = 0;
    virtual QWidget* content(const QString& id) const = 0;
    virtual QWidget* container(const QString& id) const = 0;   // the dock frame around the content
    virtual QString dockIdOf(const QWidget* widget) const = 0; // the dock containing `widget`, or empty
    virtual QWidget* focusedDock() const = 0;                  // nullptr when no dock has focus
    virtual void setStateObserver(StateObserver observer) = 0;
    virtual const char* name() const = 0; // "qdockwidget", "ads"
    // The docking library's version ("5.1.1" for ADS), empty for the QDockWidget backend (layout-1
    // "adsVersion").
    virtual QString libraryVersion() const { return {}; }

protected:
    IDockBackend() = default;
    IDockBackend(const IDockBackend&) = default;
    IDockBackend& operator=(const IDockBackend&) = default;
};

class DockHost final : public QObject {
    Q_OBJECT
public:
    // `backend` null: the build's default backend for `host` (makeDefaultBackend).
    DockHost(QMainWindow& host, QWidget* central, std::unique_ptr<IDockBackend> backend = {});
    ~DockHost() override;

    void addDock(const QString& id, const QString& title, QWidget* content, DockArea defaultArea,
                 DockState defaultState, const QString& iconName = {});
    void show(const QString& id);   // visible (restores from AutoHide/Closed to Docked), no focus change
    void focus(const QString& id);  // show + keyboard focus (Closed -> Docked; AutoHide flies out)
    void toggle(const QString& id); // visible <-> Closed (V); back to the last visible state
    void setState(const QString& id, DockState state);
    DockState state(const QString& id) const;
    bool isVisible(const QString& id) const; // Docked, Floating, or an AutoHide dock that is flown out
    QStringList dockIds() const;             // registration order
    DockArea defaultArea(const QString& id) const;
    DockState defaultState(const QString& id) const;
    QByteArray savePerspective() const;
    bool restorePerspective(const QByteArray& perspective); // false: caller falls back to resetToDefault()
    void resetToDefault();                                  // every dock to its registered default (06 3.5)
    QWidget* focusedDock() const;
    QString focusedDockId() const;
    QString dockIdOf(const QWidget* widget) const;
    QWidget* content(const QString& id) const;
    IDockBackend& backend() noexcept { return *backend_; }
    const IDockBackend& backend() const noexcept { return *backend_; }

    // Backend choice (D-153): the names this build has ("ads" first when ADS is built, then "qdockwidget"),
    // the default (the environment variable ESSES_DOCK_BACKEND when it names one of them, else the build's
    // ESSES_DOCK_BACKEND), and a backend by name (nullptr for a name this build does not have).
    static QStringList availableBackends();
    static QString defaultBackendName();
    static std::unique_ptr<IDockBackend> makeBackend(const QString& name, QMainWindow& host);
    static std::unique_ptr<IDockBackend> makeDefaultBackend(QMainWindow& host);

Q_SIGNALS:
    void dockFocusChanged(const QString& id, bool focused);
    void visibilityChanged(const QString& id, bool visible);
    void stateChanged(const QString& id);

private:
    struct Registered {
        QString title;
        DockArea area = DockArea::Left;
        DockState state = DockState::Docked;
        QString iconName;
        DockState lastVisible = DockState::Docked; // toggle() target
        bool visible = false;                      // last announced visibility
    };
    void announce(const QString& id);
    void onFocusChanged(QWidget* old, QWidget* now);

    QMainWindow& host_;
    std::unique_ptr<IDockBackend> backend_;
    QStringList ids_;
    QHash<QString, Registered> registered_;
};

} // namespace esses::widgets
