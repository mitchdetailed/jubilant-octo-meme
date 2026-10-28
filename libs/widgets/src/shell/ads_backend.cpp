// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The only translation unit that sees the Qt Advanced Docking System (D-026): see ads_backend.hpp.

#include <esses/widgets/shell/ads_backend.hpp>

#include <AutoHideDockContainer.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <ads_globals.h>
#include <ads_version.h>
#include <utility>

namespace esses::widgets {

namespace {

// The worksheet area's dock widget; its name is part of every saved state (ADS refuses a state whose central
// widget has another name).
const char* const kCentralName = "esses.worksheetArea";

ads::DockWidgetArea toAdsArea(DockArea area) {
    switch (area) {
    case DockArea::Left:
        return ads::LeftDockWidgetArea;
    case DockArea::Right:
        return ads::RightDockWidgetArea;
    case DockArea::Top:
        return ads::TopDockWidgetArea;
    case DockArea::Bottom:
    case DockArea::Center: // never into the worksheet area: Center docks go below it, as in the fallback
        return ads::BottomDockWidgetArea;
    }
    return ads::LeftDockWidgetArea;
}

ads::SideBarLocation toSideBar(DockArea area) {
    switch (area) {
    case DockArea::Left:
        return ads::SideBarLeft;
    case DockArea::Right:
        return ads::SideBarRight;
    case DockArea::Top:
        return ads::SideBarTop;
    case DockArea::Bottom:
    case DockArea::Center:
        return ads::SideBarBottom;
    }
    return ads::SideBarLeft;
}

// The state of a dock, read from ADS. A dock without a dock area (one a restored perspective did not place)
// is closed until it is placed again.
DockState stateOf(const ads::CDockWidget* dock) {
    if (dock == nullptr || dock->isClosed() || dock->dockAreaWidget() == nullptr) {
        return DockState::Closed;
    }
    if (dock->isAutoHide()) {
        return DockState::AutoHide;
    }
    if (dock->isInFloatingContainer()) {
        return DockState::Floating;
    }
    return DockState::Docked;
}

// An AutoHide dock whose auto-hide container is shown (the fly-out of 06 section 3.3 item 5).
bool flownOut(const ads::CDockWidget* dock) {
    if (stateOf(dock) != DockState::AutoHide) {
        return false;
    }
    const ads::CAutoHideDockContainer* c = dock->autoHideDockContainer();
    return c != nullptr && !c->isHidden();
}

void collapse(ads::CDockWidget* dock) {
    if (flownOut(dock)) {
        dock->autoHideDockContainer()->collapseView(true);
    }
}

struct Applying {
    explicit Applying(int& counter) : n(counter) { ++n; }
    ~Applying() { --n; }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
    int& n;
};

} // namespace

struct AdsBackend::Impl {
    struct Entry {
        // Held as a QWidget and read as a dock with qobject_cast (adsDock()): QPointer<ads::CDockWidget>
        // static_casts from QObject on every read, undefined behaviour while the dock is being destroyed
        // (UBSan -fsanitize=vptr), and the content it deletes then moves the focus, which asks this backend
        // about its docks (dockIdOf, onFocusChanged). qobject_cast finds no CDockWidget in a dock being
        // destroyed.
        QPointer<QWidget> dock;
        DockArea area = DockArea::Left; // where this backend last placed it (registration, reset, re-dock)
        bool justPlaced = false;        // placed by the operation in progress (its geometry is not laid out)
        DockState defaultState = DockState::Docked; // for a perspective that does not know the dock
        DockState reported = DockState::Closed;     // last state the observer heard of
        bool reportedFlownOut = false;

        ads::CDockWidget* adsDock() const { return qobject_cast<ads::CDockWidget*>(dock.data()); }
    };

    explicit Impl(QMainWindow& h) : host(h) {}

    QMainWindow& host;
    QPointer<ads::CDockManager> manager;
    QPointer<ads::CDockWidget> central;
    QStringList order;
    QHash<QString, Entry> docks;
    int applying = 0; // > 0 while this backend changes the layout (ADS's signals are not user changes then)
    bool queued = false; // a settled sync() is queued
    StateObserver observer;

    ads::CDockWidget* dock(const QString& id) const { return docks.value(id).adsDock(); }
};

AdsBackend::AdsBackend(QMainWindow& host) : d_(std::make_unique<Impl>(host)) {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // ADS's Linux floating window takes its native window id each time it is shown, for window properties it
    // then sets only on X11 (CFloatingDockContainer::show). Outside Wayland that window is a child of the
    // dock manager, so by default Qt makes its siblings, and with them the dock manager's whole tree, native
    // windows. Off X11 that is never wanted: the offscreen platform cannot reparent native child windows, and
    // destroying a main window whose dock had floated crashed in its backing store. X11 keeps ADS's
    // behaviour; on Wayland ADS's floating windows have no parent, so the attribute changes nothing there.
    if (QGuiApplication::platformName() != QLatin1String("xcb")) {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings); // application-wide
    }
#endif
    // Global ADS configuration, read when the manager and its dock widgets are created: ADS's defaults with
    // opaque splitters, and auto-hide (pin button per dock area, minimise button, collapse on a click
    // outside).
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
    ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
    d_->manager = new ads::CDockManager(&host); // sets itself as the host's central widget
    d_->manager->setObjectName(QStringLiteral("esses.dockManager"));
    ads::CDockManager* m = d_->manager;
    connect(m, &ads::CDockContainerWidget::dockAreasAdded, this, [this] { onDockSignal(false); });
    connect(m, &ads::CDockContainerWidget::dockAreasRemoved, this, [this] { onDockSignal(false); });
    connect(m, &ads::CDockContainerWidget::autoHideWidgetCreated, this, [this] { onDockSignal(false); });
    connect(m, &ads::CDockManager::floatingWidgetCreated, this, [this] { onDockSignal(false); });
    connect(m, &ads::CDockManager::stateRestored, this, [this] { onDockSignal(false); });
    if (qApp != nullptr) {
        connect(qApp, &QApplication::focusChanged, this,
                [this](QWidget*, QWidget* now) { onFocusChanged(now); });
    }
}

AdsBackend::~AdsBackend() {
    // The dock manager and its dock widgets belong to the host window and may outlive this backend.
    d_->observer = nullptr;
    for (const Impl::Entry& e : std::as_const(d_->docks)) {
        if (e.dock != nullptr) {
            QObject::disconnect(e.dock.data(), nullptr, this, nullptr);
        }
    }
    if (d_->manager != nullptr) {
        QObject::disconnect(d_->manager.data(), nullptr, this, nullptr);
    }
}

QWidget* AdsBackend::dockManager() const {
    return d_->manager.data();
}

QString AdsBackend::libraryVersion() const {
    return QStringLiteral("%1.%2.%3").arg(ADS_VERSION_MAJOR).arg(ADS_VERSION_MINOR).arg(ADS_VERSION_PATCH);
}

void AdsBackend::setStateObserver(StateObserver observer) {
    d_->observer = std::move(observer);
}

void AdsBackend::setCentral(QWidget* central) {
    if (central == nullptr || d_->manager == nullptr || d_->central != nullptr) {
        return;
    }
    auto* dock = new ads::CDockWidget(d_->manager, QStringLiteral("Worksheet"));
    dock->setObjectName(QLatin1String(kCentralName));
    dock->setWidget(central, ads::CDockWidget::ForceNoScrollArea);
    // ADS takes a central widget only before the first dock (DockHost sets it in its constructor).
    if (d_->manager->setCentralWidget(dock) == nullptr) {
        delete dock;
        return;
    }
    d_->central = dock;
}

void AdsBackend::addDock(const QString& id, const QString& title, QWidget* content, DockArea area,
                         DockState state) {
    if (d_->docks.contains(id) || d_->manager == nullptr) {
        return;
    }
    const Applying guard(d_->applying);
    auto* dock = new ads::CDockWidget(d_->manager, title);
    dock->setObjectName(id); // the saveState/restoreState key
    dock->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
    // The dock's own controls (close and float buttons, the pin, drags) change the state without setState.
    connect(dock, &ads::CDockWidget::viewToggled, this, [this](bool) { onDockSignal(true); });
    connect(dock, &ads::CDockWidget::topLevelChanged, this, [this](bool) { onDockSignal(true); });
    connect(dock, &ads::CDockWidget::visibilityChanged, this, [this](bool) { onDockSignal(false); });
    Impl::Entry e;
    e.dock = dock;
    e.area = area;
    e.defaultState = state;
    d_->order.append(id);
    d_->docks.insert(id, e);
    placeInArea(id, area);
    apply(id, state, false);
}

// Docks `id` into `area` of the main container, tabbed with the first other dock registered in the same area
// while that one is docked (06 section 3.2: Channels under Data); that first dock stays the current tab.
void AdsBackend::placeInArea(const QString& id, DockArea area) {
    const auto it = d_->docks.find(id);
    if (it == d_->docks.end() || it->adsDock() == nullptr || d_->manager == nullptr) {
        return;
    }
    const Applying guard(d_->applying);
    ads::CDockWidget* dock = it->adsDock();
    it->area = area;
    it->justPlaced = true;
    if (dock->isAutoHide()) {
        dock->setAutoHide(false); // out of its auto-hide container first (that container is deleted)
    }
    // A closed dock (also one a restored perspective left without a dock area) comes out of ADS's insertion
    // marked open but with its tab hidden; it is closed and opened again so that both agree. Every caller
    // wants the dock open where it was placed (and closes it afterwards when that is its state).
    const bool wasClosed = dock->isClosed();
    ads::CDockWidget* first = nullptr;
    for (const QString& other : std::as_const(d_->order)) {
        if (other == id) {
            break;
        }
        const auto o = d_->docks.constFind(other);
        if (o != d_->docks.cend() && o->adsDock() != nullptr && o->area == area &&
            stateOf(o->adsDock()) == DockState::Docked) {
            first = o->adsDock();
            break;
        }
    }
    if (first != nullptr) {
        d_->manager->addDockWidgetTabToArea(dock, first->dockAreaWidget());
        first->setAsCurrentTab();
    } else {
        d_->manager->addDockWidget(toAdsArea(area), dock);
    }
    if (wasClosed || dock->isClosed()) {
        dock->toggleView(false);
        dock->toggleView(true);
    }
}

// Shows a Closed dock where it was; an auto-hidden one stays collapsed on its side bar.
void AdsBackend::open(const QString& id) {
    const auto it = d_->docks.find(id);
    if (it == d_->docks.end() || it->adsDock() == nullptr) {
        return;
    }
    const Applying guard(d_->applying);
    ads::CDockWidget* dock = it->adsDock();
    if (dock->dockAreaWidget() == nullptr) {
        placeInArea(id, it->area);
    }
    if (dock->isClosed()) {
        dock->toggleView(true); // flies an auto-hidden dock out
        collapse(dock);
    }
}

void AdsBackend::setState(const QString& id, DockState state) {
    apply(id, state, true);
}

// `raise`: make a tabbed dock the current tab (an explicit show/focus; not on add, reset or restore).
void AdsBackend::apply(const QString& id, DockState target, bool raise) {
    const auto it = d_->docks.find(id);
    if (it == d_->docks.end() || it->adsDock() == nullptr) {
        return;
    }
    ads::CDockWidget* dock = it->adsDock();
    {
        const Applying guard(d_->applying);
        switch (target) {
        case DockState::Docked:
            open(id);
            if (dock->isAutoHide()) {
                dock->setAutoHide(false); // unpinned: docked on its side bar's side (ADS)
            } else if (dock->isInFloatingContainer()) {
                placeInArea(id, it->area);
            }
            if (raise) {
                dock->raise();
            }
            break;
        case DockState::Floating:
            open(id);
            if (dock->isAutoHide()) {
                dock->setAutoHide(false);
            }
            if (!dock->isInFloatingContainer()) {
                dock->setFloating();
            }
            if (raise) {
                dock->raise();
            }
            break;
        case DockState::AutoHide:
            open(id);
            if (dock->isInFloatingContainer()) {
                placeInArea(id, it->area);
            }
            if (!dock->isAutoHide()) {
                // A dock this operation placed goes to the side of that area. Otherwise, while the window is
                // shown, ADS picks the side from where the dock sits (as its own pin button does) once the
                // pending layout is applied; with the window hidden, the side of the area it was placed in.
                const bool laidOut = !it->justPlaced && d_->manager != nullptr && d_->manager->isVisible();
                if (laidOut) {
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                }
                dock->setAutoHide(true, laidOut ? ads::SideBarNone : toSideBar(it->area));
            }
            collapse(dock);
            break;
        case DockState::Closed:
            if (!dock->isClosed()) {
                dock->toggleView(false);
            }
            break;
        }
    }
    it->justPlaced = false;
    sync(false);
}

DockState AdsBackend::state(const QString& id) const {
    return stateOf(d_->dock(id));
}

void AdsBackend::resetDock(const QString& id, DockArea area, DockState state) {
    ads::CDockWidget* dock = d_->dock(id);
    if (dock == nullptr) {
        return;
    }
    {
        const Applying guard(d_->applying);
        collapse(dock);
        placeInArea(id, area); // unpins, re-docks a floating dock, places a dock without an area, opens it
    }
    apply(id, state, false);
}

void AdsBackend::focus(const QString& id) {
    ads::CDockWidget* dock = d_->dock(id);
    if (dock == nullptr) {
        return;
    }
    // Another fly-out collapses (reported as a change the observer must hear of; DockHost announces `id`).
    {
        const Applying guard(d_->applying);
        collapseFlyOuts(id);
    }
    sync(true);
    {
        const Applying guard(d_->applying);
        const DockState s = stateOf(dock);
        if (s == DockState::AutoHide) {
            if (!flownOut(dock)) {
                dock->autoHideDockContainer()->collapseView(false); // fly out (06 section 3.3 item 5)
            }
        } else {
            if (s == DockState::Closed) {
                apply(id, DockState::Docked, true);
            }
            dock->raise(); // current tab; a floating window is raised and activated
        }
    }
    sync(false);
    // Outside the guard: the focus moving in collapses another fly-out, a change the observer must hear of.
    if (QWidget* w = dock->widget()) {
        // The keyboard focus needs the dock's window to be the active one (a floating dock may be).
        if (QWidget* window = w->window(); !window->isActiveWindow()) {
            window->activateWindow();
        }
        w->setFocus(Qt::OtherFocusReason);
    }
}

bool AdsBackend::isFlownOut(const QString& id) const {
    return flownOut(d_->dock(id));
}

// Collapses every flown-out dock but `except`.
void AdsBackend::collapseFlyOuts(const QString& except) {
    for (const QString& id : std::as_const(d_->order)) {
        if (id != except) {
            collapse(d_->dock(id));
        }
    }
}

// A signal of ADS: `settled` ones (a view toggled, a top level changed) come at the end of an operation and
// are reported at once; every one also queues a sync after the event loop has finished the operation in
// progress.
void AdsBackend::onDockSignal(bool settled) {
    if (!d_->queued) {
        d_->queued = true;
        QTimer::singleShot(0, this, [this] {
            d_->queued = false;
            sync(true);
        });
    }
    if (settled && d_->applying == 0) {
        sync(true);
    }
}

// Compares every dock's state and fly-out with what the observer last heard and reports the changes
// (`report`), or only records them (after this backend's own changes, which DockHost announces itself).
void AdsBackend::sync(bool report) {
    if (report && d_->applying > 0) {
        return;
    }
    const QStringList ids = d_->order;
    for (const QString& id : ids) {
        const auto it = d_->docks.find(id);
        if (it == d_->docks.end()) {
            continue;
        }
        const DockState s = stateOf(it->adsDock());
        const bool out = flownOut(it->adsDock());
        if (s == it->reported && out == it->reportedFlownOut) {
            continue;
        }
        it->reported = s;
        it->reportedFlownOut = out;
        if (report && d_->observer) {
            d_->observer(id, s);
        }
    }
}

// A fly-out collapses when the focus moves outside it (06 section 3.3 item 5); losing the focus to another
// application (now == nullptr) keeps it out.
void AdsBackend::onFocusChanged(QWidget* now) {
    if (now == nullptr || d_->applying > 0) {
        return;
    }
    const QString inside = dockIdOf(now);
    bool changed = false;
    for (const QString& id : std::as_const(d_->order)) {
        ads::CDockWidget* dock = d_->dock(id);
        if (id != inside && flownOut(dock)) {
            const Applying guard(d_->applying);
            collapse(dock);
            changed = true;
        }
    }
    if (changed) {
        sync(true);
    }
}

QByteArray AdsBackend::saveState() const {
    QJsonObject root;
    root.insert(QStringLiteral("backend"), QString::fromLatin1(name()));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("adsVersion"), libraryVersion());
    const QByteArray state = d_->manager != nullptr ? d_->manager->saveState(kStateVersion) : QByteArray();
    root.insert(QStringLiteral("state"), QString::fromLatin1(state.toBase64()));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool AdsBackend::restoreState(const QByteArray& state) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(state, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || d_->manager == nullptr) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("backend")).toString() != QLatin1String(name()) ||
        root.value(QStringLiteral("version")).toInt() != 1 ||
        !root.value(QStringLiteral("state")).isString()) {
        return false;
    }
    const QByteArray layout =
        QByteArray::fromBase64(root.value(QStringLiteral("state")).toString().toLatin1());
    if (layout.isEmpty()) {
        return false;
    }
    {
        const Applying guard(d_->applying);
        collapseFlyOuts(QString());
        // ADS checks the whole state in a dry run first and changes nothing when it fails.
        if (!d_->manager->restoreState(layout, kStateVersion)) {
            return false;
        }
        for (const QString& id : std::as_const(d_->order)) {
            const Impl::Entry e = d_->docks.value(id);
            if (e.adsDock() != nullptr && e.adsDock()->dockAreaWidget() == nullptr) {
                resetDock(id, e.area, e.defaultState);
            }
        }
    }
    sync(false);
    return true;
}

QWidget* AdsBackend::content(const QString& id) const {
    ads::CDockWidget* dock = d_->dock(id);
    return dock == nullptr ? nullptr : dock->widget();
}

QWidget* AdsBackend::container(const QString& id) const {
    return d_->dock(id);
}

QString AdsBackend::dockIdOf(const QWidget* widget) const {
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        for (auto it = d_->docks.cbegin(); it != d_->docks.cend(); ++it) {
            if (it->dock.data() == w) {
                return it.key();
            }
        }
        if (w == &d_->host) {
            break;
        }
    }
    return {};
}

QWidget* AdsBackend::focusedDock() const {
    const QString id = dockIdOf(QApplication::focusWidget());
    return id.isEmpty() ? nullptr : d_->dock(id);
}

} // namespace esses::widgets
