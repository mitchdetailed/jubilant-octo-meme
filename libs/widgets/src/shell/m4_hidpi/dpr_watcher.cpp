// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DprWatcher (dpr_watcher.hpp; M4 hidpi package, D-209). Root of the package in src/shell/
// (docs/milestones/M4.md section 3).

#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/dpr_watcher.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QEvent>
#include <QMetaObject>
#include <QWidget>
#include <algorithm>

namespace esses::widgets {

DprWatcher::DprWatcher(QObject* parent) : QObject(parent) {}

DprWatcher::~DprWatcher() {
    for (const Entry& e : entries_) {
        if (e.window != nullptr) {
            e.window->removeEventFilter(this);
            QObject::disconnect(e.window.data(), nullptr, this, nullptr);
        }
    }
}

DprWatcher::Entry* DprWatcher::find(const QObject* window) {
    const auto it =
        std::find_if(entries_.begin(), entries_.end(), [window](const Entry& e) { return e.key == window; });
    return it == entries_.end() ? nullptr : &*it;
}

void DprWatcher::watch(QWidget* topLevel) {
    if (topLevel == nullptr) {
        return;
    }
    QWidget* window = topLevel->window();
    if (find(window) != nullptr) {
        return;
    }
    Entry e;
    e.window = window;
    e.key = window;
    e.dpr = window->devicePixelRatioF();
    entries_.push_back(e);
    window->installEventFilter(this);
    connect(window, &QObject::destroyed, this, [this](QObject* gone) { forget(gone); });
}

void DprWatcher::unwatch(QWidget* topLevel) {
    if (topLevel == nullptr) {
        return;
    }
    QWidget* window = topLevel->window();
    if (find(window) == nullptr) {
        return;
    }
    window->removeEventFilter(this);
    QObject::disconnect(window, nullptr, this, nullptr);
    forget(window);
}

void DprWatcher::forget(const QObject* window) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [window](const Entry& e) { return e.key == window; }),
                   entries_.end());
}

QList<QWidget*> DprWatcher::watched() const {
    QList<QWidget*> out;
    for (const Entry& e : entries_) {
        if (e.window != nullptr) {
            out.append(e.window.data());
        }
    }
    return out;
}

qreal DprWatcher::lastDpr(const QWidget* topLevel) const {
    if (topLevel == nullptr) {
        return 0.0;
    }
    const QWidget* window = topLevel->window();
    for (const Entry& e : entries_) {
        if (e.key == window) {
            return e.dpr;
        }
    }
    return 0.0;
}

int DprWatcher::changes() const {
    return changes_;
}

bool DprWatcher::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
    case QEvent::Show:
        if (auto* window = qobject_cast<QWidget*>(watched); window != nullptr && find(window) != nullptr) {
            queueCheck(window);
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// One check per window and event-loop turn: Qt's own recursive DPR event (and the screen change that may
// precede the DPR update) has been delivered by then.
void DprWatcher::queueCheck(QWidget* window) {
    Entry* e = find(window);
    if (e == nullptr || e->queued) {
        return;
    }
    e->queued = true;
    QMetaObject::invokeMethod(
        this,
        [this, target = QPointer<QWidget>(window)] {
            if (target != nullptr) {
                check(target.data());
            }
        },
        Qt::QueuedConnection);
}

int DprWatcher::check(QWidget* window) {
    Entry* e = find(window);
    if (e == nullptr) {
        return 0;
    }
    e->queued = false;
    const qreal from = e->dpr;
    const qreal to = window->devicePixelRatioF();
    const int rebuilt = refreshComponents(*window);
    if (to != from) {
        e->dpr = to;
        ++changes_;
        Q_EMIT dprChanged(window, from, to); // e may dangle once a receiver changed the watch list
    }
    return rebuilt;
}

int DprWatcher::refreshAll() {
    int rebuilt = 0;
    for (QWidget* window : watched()) {
        rebuilt += check(window);
    }
    return rebuilt;
}

int DprWatcher::refreshComponents(QWidget& root) {
    int rebuilt = 0;
    if (auto* self = qobject_cast<ComponentWidget*>(&root);
        self != nullptr && self->recheckDevicePixelRatio()) {
        ++rebuilt;
    }
    for (ComponentWidget* c : root.findChildren<ComponentWidget*>()) {
        if (c->recheckDevicePixelRatio()) {
            ++rebuilt;
        }
    }
    if (auto* self = qobject_cast<WorksheetCanvas*>(&root); self != nullptr) {
        self->refreshGeometry();
    }
    for (WorksheetCanvas* canvas : root.findChildren<WorksheetCanvas*>()) {
        canvas->refreshGeometry();
    }
    return rebuilt;
}

} // namespace esses::widgets
