// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/qdockwidget_backend.hpp>

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <utility>

namespace esses::widgets {

QString dockStateName(DockState state) {
    switch (state) {
    case DockState::Docked:
        return QStringLiteral("Docked");
    case DockState::Floating:
        return QStringLiteral("Floating");
    case DockState::AutoHide:
        return QStringLiteral("AutoHide");
    case DockState::Closed:
        return QStringLiteral("Closed");
    }
    return QStringLiteral("Docked");
}

std::optional<DockState> parseDockState(const QString& name) {
    for (const DockState s :
         {DockState::Docked, DockState::Floating, DockState::AutoHide, DockState::Closed}) {
        if (name == dockStateName(s)) {
            return s;
        }
    }
    return std::nullopt;
}

DockHost::DockHost(QMainWindow& host, QWidget* central, std::unique_ptr<IDockBackend> backend)
    : QObject(&host), host_(host), backend_(backend ? std::move(backend) : makeDefaultBackend(host)) {
    if (central != nullptr) {
        host_.setCentralWidget(central);
    }
    backend_->setStateObserver([this](const QString& id, DockState) { announce(id); });
    if (qApp != nullptr) {
        connect(qApp, &QApplication::focusChanged, this, &DockHost::onFocusChanged);
    }
}

DockHost::~DockHost() {
    backend_->setStateObserver({});
}

// The ADS backend joins here once ADS is vendored (D-119): makeDefaultBackend then returns it unless
// ESSES_DOCK_BACKEND=qdockwidget. Until then every OS uses the QDockWidget backend (AC-M1-13 runs on it).
std::unique_ptr<IDockBackend> DockHost::makeDefaultBackend(QMainWindow& host) {
    return std::make_unique<QDockWidgetBackend>(host);
}

void DockHost::addDock(const QString& id, const QString& title, QWidget* content, DockArea defaultArea,
                       DockState defaultState, const QString& iconName) {
    Registered r;
    r.title = title;
    r.area = defaultArea;
    r.state = defaultState;
    r.iconName = iconName;
    if (!ids_.contains(id)) {
        ids_.append(id);
    }
    registered_.insert(id, r);
    backend_->addDock(id, title, content, defaultArea, defaultState);
    announce(id);
}

void DockHost::show(const QString& id) {
    const DockState s = backend_->state(id);
    if (s == DockState::Closed || s == DockState::AutoHide) {
        backend_->setState(id, DockState::Docked);
    }
    announce(id);
}

void DockHost::focus(const QString& id) {
    if (!registered_.contains(id)) {
        return;
    }
    if (backend_->state(id) == DockState::Closed) {
        const DockState target = registered_.value(id).lastVisible;
        backend_->setState(id, target == DockState::Floating ? DockState::Floating : DockState::Docked);
    }
    backend_->focus(id); // an AutoHide dock flies out
    announce(id);
}

void DockHost::toggle(const QString& id) {
    if (!registered_.contains(id)) {
        return;
    }
    if (isVisible(id)) {
        backend_->setState(id, DockState::Closed);
    } else {
        const DockState target = registered_.value(id).lastVisible;
        backend_->setState(id, target == DockState::Floating ? DockState::Floating : DockState::Docked);
    }
    announce(id);
}

void DockHost::setState(const QString& id, DockState state) {
    backend_->setState(id, state);
    announce(id);
}

DockState DockHost::state(const QString& id) const {
    return backend_->state(id);
}

bool DockHost::isVisible(const QString& id) const {
    const DockState s = backend_->state(id);
    return s == DockState::Docked || s == DockState::Floating ||
           (s == DockState::AutoHide && backend_->isFlownOut(id));
}

QStringList DockHost::dockIds() const {
    return ids_;
}

DockArea DockHost::defaultArea(const QString& id) const {
    return registered_.value(id).area;
}

DockState DockHost::defaultState(const QString& id) const {
    return registered_.value(id).state;
}

QByteArray DockHost::savePerspective() const {
    return backend_->saveState();
}

bool DockHost::restorePerspective(const QByteArray& perspective) {
    const bool ok = !perspective.isEmpty() && backend_->restoreState(perspective);
    for (const QString& id : ids_) {
        announce(id);
    }
    return ok;
}

void DockHost::resetToDefault() {
    for (const QString& id : ids_) {
        const Registered& r = registered_[id];
        backend_->resetDock(id, r.area, r.state);
    }
    for (const QString& id : ids_) {
        announce(id);
    }
}

QWidget* DockHost::focusedDock() const {
    return backend_->focusedDock();
}

QString DockHost::focusedDockId() const {
    return backend_->dockIdOf(QApplication::focusWidget());
}

QString DockHost::dockIdOf(const QWidget* widget) const {
    return backend_->dockIdOf(widget);
}

QWidget* DockHost::content(const QString& id) const {
    return backend_->content(id);
}

// Records the last visible state (the target of toggle/focus from Closed) and announces visibility changes.
void DockHost::announce(const QString& id) {
    const auto it = registered_.find(id);
    if (it == registered_.end()) {
        return;
    }
    const DockState s = backend_->state(id);
    if (s == DockState::Docked || s == DockState::Floating) {
        it->lastVisible = s;
    }
    const bool visible = isVisible(id);
    const bool changed = visible != it->visible;
    it->visible = visible;
    Q_EMIT stateChanged(id);
    if (changed) {
        Q_EMIT visibilityChanged(id, visible);
    }
}

void DockHost::onFocusChanged(QWidget* old, QWidget* now) {
    const QString before = backend_->dockIdOf(old);
    const QString after = backend_->dockIdOf(now);
    if (before == after) {
        return;
    }
    if (!before.isEmpty()) {
        Q_EMIT dockFocusChanged(before, false);
    }
    if (!after.isEmpty()) {
        Q_EMIT dockFocusChanged(after, true);
    }
}

} // namespace esses::widgets
