// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AdsBackend (06 sections 3.1-3.5, D-026, D-153): the DockHost backend over the Qt Advanced Docking
// System, the default whenever the build has ADS (ESSES_DOCK_BACKEND=ads, cmake/qtads.cmake). Declared
// only for builds with ADS (ESSES_HAVE_ADS=1). No ADS type appears in this header: ADS (esses::qtads, a
// shared library under LGPL-2.1-or-later) is linked privately by esses_widgets and seen only by
// src/shell/ads_backend.cpp.
//   Host      a CDockManager becomes the QMainWindow's central widget; setCentral() puts the worksheet
//             area in ADS's central dock widget (objectName "esses.worksheetArea"; not movable,
//             floatable, closable or pinnable; the docks surround it)
//   Docked    a CDockWidget (objectName = the dock id, the saveState key) in a dock area of the main
//             container, tabbed with the first dock registered in the same area (06 section 3.2)
//   Floating  in a floating container (CDockWidget::setFloating; a tab of a floating window counts too)
//   AutoHide  pinned to a side bar (CDockWidget::setAutoHide): the side of the area this backend placed
//             it in, or, while the window is shown, the side ADS's own pin button would choose; the side
//             tab or DockHost::focus flies it out (its auto-hide container shown), and it collapses again
//             when the focus leaves it (06 section 3.3 item 5) or on a click outside it (ADS)
//   Closed    CDockWidget::toggleView(false); opening it again returns it where it was (docked, floating
//             or collapsed on its side bar)
// The state is read from ADS each time (closed, auto-hide, floating container, docked), so changes made
// with ADS's own controls (tab and title-bar buttons, drags, the side tab) are always seen; the
// StateObserver hears of them through the dock widgets' and the dock manager's signals (at once, and once
// more after the event loop has settled a compound operation such as a drop). Dock widgets are not
// deleted on close; the dock manager belongs to the host window and outlives this backend.
// The perspective is a JSON envelope {"backend": "ads", "version": 1, "adsVersion": "5.1.1", "state":
// <base64 of CDockManager::saveState(kStateVersion)>}. A perspective of another backend, a damaged one or
// one ADS rejects (its own dry run first) is refused (restoreState() false) and nothing changes; a dock
// the perspective does not know (registered after it was saved) goes to its registered area and state.
#pragma once

#include <esses/widgets/shell/dock_host.hpp>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

class QMainWindow;
class QWidget;

namespace esses::widgets {

class AdsBackend final : public QObject, public IDockBackend {
public:
    static constexpr int kStateVersion = 1; // CDockManager::saveState/restoreState user version

    explicit AdsBackend(QMainWindow& host);
    ~AdsBackend() override;
    AdsBackend(const AdsBackend&) = delete;
    AdsBackend& operator=(const AdsBackend&) = delete;

    void setCentral(QWidget* central) override;
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
    QWidget* container(const QString& id) const override; // the ads::CDockWidget
    QString dockIdOf(const QWidget* widget) const override;
    QWidget* focusedDock() const override;
    void setStateObserver(StateObserver observer) override;
    const char* name() const override { return "ads"; }
    QString libraryVersion() const override; // "5.1.1"

    // The ads::CDockManager (the host's central widget).
    QWidget* dockManager() const;

private:
    struct Impl;
    void apply(const QString& id, DockState target, bool raise);
    void open(const QString& id);
    void placeInArea(const QString& id, DockArea area);
    void collapseFlyOuts(const QString& except);
    void onDockSignal(bool settled);
    void sync(bool report);
    void onFocusChanged(QWidget* now);

    std::unique_ptr<Impl> d_;
};

} // namespace esses::widgets
