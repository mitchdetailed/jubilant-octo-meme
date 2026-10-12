// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/widgets/shell/qdockwidget_backend.hpp>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QToolBar>
#include <functional>
#include <utility>

namespace esses::widgets {

namespace {

// A QDockWidget that reports its own close button (QDockWidget has no "closed by the user" signal).
class Dock final : public QDockWidget {
public:
    Dock(const QString& title, QWidget* parent) : QDockWidget(title, parent) {}
    std::function<void()> onClose;

protected:
    void closeEvent(QCloseEvent* event) override {
        QDockWidget::closeEvent(event);
        if (event->isAccepted() && onClose) {
            onClose();
        }
    }
};

Qt::DockWidgetArea toQtArea(DockArea area) {
    switch (area) {
    case DockArea::Left:
        return Qt::LeftDockWidgetArea;
    case DockArea::Right:
        return Qt::RightDockWidgetArea;
    case DockArea::Top:
        return Qt::TopDockWidgetArea;
    case DockArea::Bottom:
    case DockArea::Center:
        return Qt::BottomDockWidgetArea;
    }
    return Qt::LeftDockWidgetArea;
}

Qt::ToolBarArea toToolBarArea(DockArea area) {
    switch (area) {
    case DockArea::Left:
        return Qt::LeftToolBarArea;
    case DockArea::Right:
        return Qt::RightToolBarArea;
    case DockArea::Top:
        return Qt::TopToolBarArea;
    case DockArea::Bottom:
    case DockArea::Center:
        return Qt::BottomToolBarArea;
    }
    return Qt::LeftToolBarArea;
}

DockArea sideOf(DockArea area) {
    return area == DockArea::Center ? DockArea::Bottom : area;
}

const char* sideName(DockArea area) {
    switch (sideOf(area)) {
    case DockArea::Left:
        return "left";
    case DockArea::Right:
        return "right";
    case DockArea::Top:
        return "top";
    case DockArea::Bottom:
    case DockArea::Center:
        return "bottom";
    }
    return "left";
}

struct Applying {
    explicit Applying(int& counter) : n(counter) { ++n; }
    ~Applying() { --n; }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
    int& n;
};

} // namespace

QDockWidgetBackend::QDockWidgetBackend(QMainWindow& host) : host_(host) {
    if (qApp != nullptr) {
        connect(qApp, &QApplication::focusChanged, this,
                [this](QWidget*, QWidget* now) { onFocusChanged(now); });
    }
}

QDockWidgetBackend::~QDockWidgetBackend() {
    // The docks belong to the host window and may outlive this backend.
    for (Entry& e : docks_) {
        if (auto* d = dynamic_cast<Dock*>(e.dock.data())) {
            d->onClose = nullptr;
        }
    }
}

void QDockWidgetBackend::setCentral(QWidget* central) {
    host_.setCentralWidget(central);
}

void QDockWidgetBackend::addDock(const QString& id, const QString& title, QWidget* content, DockArea area,
                                 DockState state) {
    if (docks_.contains(id)) {
        return;
    }
    auto* dock = new Dock(title, &host_);
    dock->setObjectName(id); // the saveState/restoreState key
    dock->setWidget(content);
    dock->onClose = [this, id] { onClosed(id); };
    connect(dock, &QDockWidget::topLevelChanged, this,
            [this, id](bool floating) { onTopLevelChanged(id, floating); });
    Entry e;
    e.dock = dock;
    e.area = area;
    order_.append(id);
    Entry& stored = docks_.insert(id, e).value();
    placeInArea(id, stored, area);
    apply(id, stored, state, false);
}

// Docks `id` into `area`, tabbed with the first other dock registered in the same area (06 section 3.2:
// Channels under Data, Track Map under Values, Messages with Channel Status); that first dock stays on top.
void QDockWidgetBackend::placeInArea(const QString& id, Entry& e, DockArea area) {
    const Applying guard(applying_);
    e.area = area;
    QDockWidget* dock = e.dock;
    if (dock == nullptr) {
        return;
    }
    dock->setFloating(false);
    host_.addDockWidget(toQtArea(area), dock);
    for (const QString& other : order_) {
        if (other == id) {
            break;
        }
        const Entry& o = docks_[other];
        if (o.dock != nullptr && o.area == area) {
            host_.tabifyDockWidget(o.dock, dock);
            o.dock->raise();
            break;
        }
    }
}

void QDockWidgetBackend::setState(const QString& id, DockState state) {
    const auto it = docks_.find(id);
    if (it == docks_.end()) {
        return;
    }
    apply(id, it.value(), state, true);
}

// `raise`: make a tabbed dock the current tab (an explicit show/focus; not on add, reset or restore, which
// keep the tab order QMainWindow laid out).
void QDockWidgetBackend::apply(const QString& id, Entry& e, DockState state, bool raise) {
    const Applying guard(applying_);
    QDockWidget* dock = e.dock;
    if (dock == nullptr) {
        return;
    }
    if (flownOut_ == id) {
        flownOut_.clear();
    }
    e.state = state;
    switch (state) {
    case DockState::Docked:
        removeButton(e);
        dock->setFloating(false);
        if (dock->isHidden()) {
            dock->show();
        }
        if (raise) {
            dock->raise();
        }
        break;
    case DockState::Floating:
        removeButton(e);
        if (dock->isHidden()) {
            dock->show();
        }
        dock->setFloating(true);
        if (raise) {
            dock->raise();
        }
        break;
    case DockState::AutoHide:
        dock->setFloating(false);
        dock->hide();
        addButton(id, e);
        break;
    case DockState::Closed:
        removeButton(e);
        dock->hide();
        break;
    }
    updateSideBars();
}

DockState QDockWidgetBackend::state(const QString& id) const {
    const auto it = docks_.constFind(id);
    return it == docks_.constEnd() ? DockState::Closed : it->state;
}

void QDockWidgetBackend::resetDock(const QString& id, DockArea area, DockState state) {
    const auto it = docks_.find(id);
    if (it == docks_.end()) {
        return;
    }
    if (flownOut_ == id) {
        collapse();
    }
    removeButton(it.value());
    placeInArea(id, it.value(), area);
    apply(id, it.value(), state, false);
}

void QDockWidgetBackend::focus(const QString& id) {
    const auto it = docks_.find(id);
    if (it == docks_.end() || it->dock == nullptr) {
        return;
    }
    if (it->state == DockState::AutoHide) {
        flyOut(id);
    } else if (it->state == DockState::Closed) {
        apply(id, it.value(), DockState::Docked, true);
    }
    QDockWidget* dock = it->dock;
    dock->raise();
    // The keyboard focus needs the dock's window to be the active one: a floating dock's own window, or the
    // main window while a floating dock is active.
    if (QWidget* window = dock->window(); !window->isActiveWindow()) {
        window->activateWindow();
    }
    if (QWidget* w = dock->widget()) {
        w->setFocus(Qt::OtherFocusReason);
    }
}

bool QDockWidgetBackend::isFlownOut(const QString& id) const {
    return !flownOut_.isEmpty() && flownOut_ == id;
}

void QDockWidgetBackend::flyOut(const QString& id) {
    if (flownOut_ == id) {
        return;
    }
    collapse();
    const auto it = docks_.find(id);
    if (it == docks_.end() || it->dock == nullptr) {
        return;
    }
    const Applying guard(applying_);
    flownOut_ = id;
    it->dock->setFloating(false);
    it->dock->show();
    it->dock->raise();
    if (it->button != nullptr) {
        it->button->setChecked(true);
    }
    notify(id, it->state);
}

void QDockWidgetBackend::collapse() {
    if (flownOut_.isEmpty()) {
        return;
    }
    const QString id = std::exchange(flownOut_, QString());
    const auto it = docks_.find(id);
    if (it == docks_.end()) {
        return;
    }
    const Applying guard(applying_);
    if (it->state == DockState::AutoHide && it->dock != nullptr) {
        it->dock->hide();
    }
    if (it->button != nullptr) {
        it->button->setChecked(false);
    }
    notify(id, it->state);
}

QToolBar* QDockWidgetBackend::sideBar(DockArea area) const {
    return sideBars_.value(static_cast<int>(sideOf(area))).data();
}

QAction* QDockWidgetBackend::autoHideButton(const QString& id) const {
    const auto it = docks_.constFind(id);
    return it == docks_.constEnd() ? nullptr : it->button.data();
}

void QDockWidgetBackend::addButton(const QString& id, Entry& e) {
    if (e.button != nullptr) {
        return;
    }
    const DockArea side = sideOf(e.area);
    QPointer<QToolBar>& bar = sideBars_[static_cast<int>(side)];
    if (bar == nullptr) {
        bar = new QToolBar(QStringLiteral("Auto-hidden docks"), &host_);
        bar->setObjectName(QStringLiteral("esses.autohide.") + QLatin1String(sideName(side)));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
        host_.addToolBar(toToolBarArea(side), bar);
    }
    auto* button = new QAction(e.dock != nullptr ? e.dock->windowTitle() : id, bar);
    button->setObjectName(QStringLiteral("esses.autohide.") + id);
    button->setCheckable(true);
    button->setToolTip(button->text());
    connect(button, &QAction::triggered, this, [this, id](bool) {
        if (flownOut_ == id) {
            collapse();
        } else {
            focus(id);
        }
    });
    bar->addAction(button);
    e.button = button;
}

void QDockWidgetBackend::removeButton(Entry& e) {
    if (e.button != nullptr) {
        delete e.button.data();
        e.button = nullptr;
    }
}

void QDockWidgetBackend::updateSideBars() {
    for (const QPointer<QToolBar>& bar : std::as_const(sideBars_)) {
        if (bar != nullptr) {
            bar->setVisible(!bar->actions().isEmpty());
        }
    }
}

void QDockWidgetBackend::notify(const QString& id, DockState state) {
    if (observer_) {
        observer_(id, state);
    }
}

// The dock's own close button: a flown-out AutoHide dock collapses (it stays AutoHide), any other dock
// becomes Closed.
void QDockWidgetBackend::onClosed(const QString& id) {
    if (applying_ > 0) {
        return;
    }
    const auto it = docks_.find(id);
    if (it == docks_.end()) {
        return;
    }
    if (it->state == DockState::AutoHide) {
        if (flownOut_ == id) {
            collapse();
        }
        return;
    }
    it->state = DockState::Closed;
    notify(id, DockState::Closed);
}

// A title-bar drag or the float button: Docked <-> Floating.
void QDockWidgetBackend::onTopLevelChanged(const QString& id, bool floating) {
    if (applying_ > 0) {
        return;
    }
    const auto it = docks_.find(id);
    if (it == docks_.end()) {
        return;
    }
    if (it->state == DockState::Docked || it->state == DockState::Floating ||
        (it->state == DockState::AutoHide && flownOut_ == id)) {
        if (flownOut_ == id) {
            flownOut_.clear(); // dragging a fly-out off makes it a floating dock (ADS behaves alike)
        }
        removeButton(it.value());
        it->state = floating ? DockState::Floating : DockState::Docked;
        updateSideBars();
        notify(id, it->state);
    }
}

// A fly-out collapses when the focus moves outside it (06 section 3.3 item 5); losing the focus to another
// application (now == nullptr) keeps it out.
void QDockWidgetBackend::onFocusChanged(QWidget* now) {
    if (flownOut_.isEmpty() || now == nullptr) {
        return;
    }
    if (dockIdOf(now) != flownOut_) {
        collapse();
    }
}

QByteArray QDockWidgetBackend::saveState() const {
    QJsonObject states;
    for (const QString& id : order_) {
        states.insert(id, dockStateName(docks_.value(id).state));
    }
    QJsonObject root;
    root.insert(QStringLiteral("backend"), QString::fromLatin1(name()));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("mainWindow"), QString::fromLatin1(host_.saveState(kStateVersion).toBase64()));
    root.insert(QStringLiteral("docks"), states);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool QDockWidgetBackend::restoreState(const QByteArray& state) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(state, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("backend")).toString() != QLatin1String(name()) ||
        root.value(QStringLiteral("version")).toInt() != 1 ||
        !root.value(QStringLiteral("docks")).isObject()) {
        return false;
    }
    const QByteArray main =
        QByteArray::fromBase64(root.value(QStringLiteral("mainWindow")).toString().toLatin1());
    collapse();
    {
        const Applying guard(applying_);
        if (main.isEmpty() || !host_.restoreState(main, kStateVersion)) {
            return false;
        }
    }
    const QJsonObject states = root.value(QStringLiteral("docks")).toObject();
    for (const QString& id : order_) {
        const std::optional<DockState> s = parseDockState(states.value(id).toString());
        Entry& e = docks_[id];
        if (s) {
            // QMainWindow::restoreState moved the dock (area, tab, floating geometry); keep its area as the
            // auto-hide side.
            if (e.dock != nullptr && !e.dock->isFloating()) {
                const Qt::DockWidgetArea a = host_.dockWidgetArea(e.dock);
                e.area = a == Qt::RightDockWidgetArea    ? DockArea::Right
                         : a == Qt::TopDockWidgetArea    ? DockArea::Top
                         : a == Qt::BottomDockWidgetArea ? DockArea::Bottom
                                                         : DockArea::Left;
            }
            removeButton(e);
            apply(id, e, *s, false);
        }
    }
    updateSideBars();
    return true;
}

QWidget* QDockWidgetBackend::content(const QString& id) const {
    QDockWidget* dock = docks_.value(id).dock;
    return dock == nullptr ? nullptr : dock->widget();
}

QWidget* QDockWidgetBackend::container(const QString& id) const {
    return docks_.value(id).dock.data();
}

QString QDockWidgetBackend::dockIdOf(const QWidget* widget) const {
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        for (auto it = docks_.cbegin(); it != docks_.cend(); ++it) {
            if (it->dock.data() == w) {
                return it.key();
            }
        }
        if (w == &host_) {
            break;
        }
    }
    return {};
}

QWidget* QDockWidgetBackend::focusedDock() const {
    const QString id = dockIdOf(QApplication::focusWidget());
    return id.isEmpty() ? nullptr : docks_.value(id).dock.data();
}

} // namespace esses::widgets
