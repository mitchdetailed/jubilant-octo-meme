// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/key_event_router.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QVariant>

namespace esses::widgets {

using qt::CommandContext;

namespace {

bool isAncestorOrSelf(const QWidget* ancestor, const QWidget* widget) {
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        if (w == ancestor) {
            return true;
        }
    }
    return false;
}

bool isTextEntry(const QWidget* widget) {
    return widget != nullptr && widget->property("esses.textEntry").toBool();
}

} // namespace

KeyEventRouter::KeyEventRouter(qt::KeymapBinder& binder, DockHost& docks, WorksheetCanvas& canvas,
                               QObject* parent)
    : QObject(parent), binder_(binder), docks_(&docks), canvas_(&canvas) {}

KeyEventRouter::~KeyEventRouter() = default;

QString KeyEventRouter::keyText(const QKeyEvent& event) {
    const int key = event.key();
    if (key == 0 || key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift ||
        key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr) {
        return {};
    }
    // Keypad Enter and Return stay distinct ("Enter", "Return"), as keymap rows list both.
    const QKeyCombination combo(event.modifiers() & ~Qt::KeypadModifier, static_cast<Qt::Key>(key));
    QString text = QKeySequence(combo).toString(QKeySequence::PortableText);
    // Qt writes DELETE as "Del"; keymap-1 rows spell it "Delete" (06 section 14.4).
    if (key == Qt::Key_Delete && text.endsWith(QLatin1String("Del"))) {
        text += QStringLiteral("ete");
    }
    return text;
}

bool KeyEventRouter::widgetFirst(const QKeyEvent& event) {
    const Qt::KeyboardModifiers mods = event.modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    if (mods != Qt::NoModifier) {
        return false;
    }
    const int k = event.key();
    if ((k >= Qt::Key_A && k <= Qt::Key_Z) || (k >= Qt::Key_0 && k <= Qt::Key_9)) {
        return true;
    }
    switch (k) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Backspace:
        return true;
    default:
        break;
    }
    // Other printable characters (punctuation typed into a search box). DELETE and ESC are not printable.
    const QString text = event.text();
    return !text.isEmpty() && text.front().isPrint() && k != Qt::Key_Delete && k != Qt::Key_Escape;
}

QString KeyEventRouter::dockOf(const QWidget* widget) const {
    return docks_.isNull() ? QString() : docks_->dockIdOf(widget);
}

bool KeyEventRouter::inScope(const QWidget* widget) const {
    if (scope_.isNull() || widget == nullptr) {
        return true;
    }
    return widget->window() == scope_.data() || !dockOf(widget).isEmpty();
}

QList<CommandContext> KeyEventRouter::chainFor(const QWidget* focus) const {
    if (focus == nullptr || (!scope_.isNull() && focus == scope_.data())) {
        return {CommandContext::Worksheet, CommandContext::Global};
    }
    const QString dock = dockOf(focus);
    if (!dock.isEmpty()) {
        if (dock == QLatin1String(DataWindow::kDockId)) {
            return {CommandContext::DataWindow, CommandContext::Global};
        }
        if (dock == QLatin1String(ChannelsWindow::kDockId)) {
            return {CommandContext::ChannelsWindow, CommandContext::Global};
        }
        if (dock == QLatin1String(ValuesWindow::kDockId)) {
            return {CommandContext::ValuesWindow, CommandContext::Global};
        }
        return {CommandContext::Global};
    }
    if (!canvas_.isNull() && isAncestorOrSelf(canvas_.data(), focus)) {
        for (const QWidget* w = focus; w != nullptr && w != canvas_.data(); w = w->parentWidget()) {
            if (const auto* c = qobject_cast<const ComponentWidget*>(w)) {
                // The component's type context (Graph for time_distance; Component for the others).
                QList<CommandContext> chain;
                if (const CommandContext own = c->commandContext(); own != CommandContext::Component) {
                    chain.append(own);
                }
                chain << CommandContext::Component << CommandContext::Worksheet << CommandContext::Global;
                return chain;
            }
        }
        return {CommandContext::Worksheet, CommandContext::Global};
    }
    return {CommandContext::Global};
}

bool KeyEventRouter::dispatch(const QString& keyText, const QWidget* focus) {
    return !keyText.isEmpty() && binder_.dispatch(keyText, chainFor(focus));
}

// One key event visits the receiver and then, while unaccepted, its ancestors (QApplication::notify); this
// filter sees every visit. A new event is resolved at its first visit, except a plain key in a dock, which is
// offered to the widget and resolved when it propagates to an ancestor (the widget ignored it).
bool KeyEventRouter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyRelease) {
        // A key press is fully delivered before its release: forget it, so a later event at the same address
        // (platform and QTest key events are stack objects) is never taken for its propagation.
        offered_ = nullptr;
        done_ = nullptr;
        return false;
    }
    if (event->type() != QEvent::KeyPress) {
        return false;
    }
    auto* receiver = qobject_cast<QWidget*>(watched);
    if (receiver == nullptr || QApplication::activeModalWidget() != nullptr ||
        QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    auto* key = static_cast<QKeyEvent*>(event);

    // The first visit of a key event is at the focus widget (or a window when nothing has the focus);
    // propagation only visits ancestors of it. A visit there is therefore always a new event, whatever its
    // address: a stale offered_/done_ (the dock widget accepted the offered key, so nothing propagated) must
    // not make it look like a propagation.
    QWidget* focus = QApplication::focusWidget();
    if (receiver == focus || (focus == nullptr && receiver->isWindow())) {
        offered_ = nullptr;
        done_ = nullptr;
    }

    if (event == offered_ && receiver != offeredTo_.data() && isAncestorOrSelf(receiver, offeredTo_.data())) {
        offered_ = nullptr;
        done_ = event;
        if (binder_.dispatch(keyText(*key), offeredChain_)) {
            key->accept();
            return true;
        }
        return false;
    }
    if (event == done_ && receiver != doneAt_.data() && isAncestorOrSelf(receiver, doneAt_.data())) {
        return false; // resolved at an earlier visit
    }

    // A new event; `receiver` is the focus widget (or the window when nothing has focus).
    offered_ = nullptr;
    done_ = event;
    doneAt_ = receiver;
    const QString text = keyText(*key);
    if (text.isEmpty()) {
        return false;
    }
    const bool plain = widgetFirst(*key);
    if (!inScope(receiver)) {
        // Another window (a modeless dialog): its own widgets keep plain keys; function keys and chords act
        // globally (06 section 14.2 item 6).
        if (plain || !binder_.dispatch(text, {CommandContext::Global})) {
            return false;
        }
        key->accept();
        return true;
    }
    const QList<CommandContext> chain = chainFor(receiver);
    const bool inDock = !dockOf(receiver).isEmpty();
    if (plain && (inDock || isTextEntry(receiver))) {
        if (inDock) {
            offered_ = event;
            offeredTo_ = receiver;
            offeredChain_ = chain;
            done_ = nullptr;
        }
        return false;
    }
    if (binder_.dispatch(text, chain)) {
        key->accept();
        return true;
    }
    return false;
}

} // namespace esses::widgets
