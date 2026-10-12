// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetCanvas (06 section 4.3, 04 section 7.2 items 1 and 7): placement, focus, maximise, 30 Hz relayout
// coalescing and the M1 edit path.

#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace esses::widgets {

namespace {

QString worksheetKey(const project::WorksheetDef& ws) {
    return ws.id.isNil() ? QString::fromStdString(ws.name) : QString::fromStdString(ws.id.str());
}

QColor colourOf(project::Rgba rgba) {
    return QColor::fromRgb(static_cast<int>((rgba >> 24) & 0xFFU), static_cast<int>((rgba >> 16) & 0xFFU),
                           static_cast<int>((rgba >> 8) & 0xFFU), static_cast<int>(rgba & 0xFFU));
}

} // namespace

WorksheetCanvas::WorksheetCanvas(QWidget* parent) : QWidget(parent), layoutTimer_(new QTimer(this)) {
    setFocusPolicy(Qt::StrongFocus);
    layoutTimer_->setSingleShot(true);
    layoutTimer_->setInterval(kLayoutCoalesceMs);
    connect(layoutTimer_, &QTimer::timeout, this, [this] {
        if (layoutPending_) {
            layoutPending_ = false;
            layoutComponents();
            layoutTimer_->start(); // keep throttling while the burst lasts
        }
    });
    if (qApp != nullptr) {
        connect(qApp, &QApplication::focusChanged, this, &WorksheetCanvas::onApplicationFocusChanged);
    }
}

WorksheetCanvas::~WorksheetCanvas() = default;

QRect WorksheetCanvas::placement(const project::Rect& f, const QRect& r) {
    const int x0 = r.x() + static_cast<int>(std::lround(f.l * r.width()));
    const int y0 = r.y() + static_cast<int>(std::lround(f.t * r.height()));
    const int x1 = r.x() + static_cast<int>(std::lround(f.r * r.width()));
    const int y1 = r.y() + static_cast<int>(std::lround(f.b * r.height()));
    return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

void WorksheetCanvas::setWorksheet(const project::WorksheetDef& worksheet, WorksheetContext& context) {
    const QString key = worksheetKey(worksheet);
    if (context_ == &context && activeKey_ == key && worksheet_ == worksheet) {
        return; // already shown
    }
    // A worksheet switch from a dock (F11/F12, the tab bar) leaves the keyboard focus where it is.
    QWidget* keyboard = QApplication::focusWidget();
    const bool keyboardInCanvas = keyboard != nullptr && (keyboard == this || isAncestorOf(keyboard));
    if (keyboardInCanvas && keyboard != this) {
        setFocus(Qt::OtherFocusReason); // the canvas holds the keyboard while the components swap
    }
    keepActive();
    scheme_ = context.scheme;
    bool reused = false;
    if (const auto it = kept_.find(key); it != kept_.end()) {
        KeptWorksheet page = std::move(it->second);
        kept_.erase(it);
        if (page.context == &context && page.worksheet == worksheet) {
            components_ = std::move(page.components);
            maximised_ = page.maximised;
            focused_.clear(); // selectFocused below announces the worksheet's focused component again
            if (!page.focused.isNull()) {
                rememberedFocus_.insert(key, page.focused->spec().id);
            }
            reused = true;
        } else {
            for (const QPointer<ComponentWidget>& w : page.components) {
                delete w.data(); // the definition or the context changed: rebuilt below
            }
        }
    }
    worksheet_ = worksheet;
    context_ = &context;
    activeKey_ = key;
    if (!reused) {
        createComponents(context);
    }
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull()) {
            w->raise(); // ascending z: the last raised paints on top
        }
    }
    layoutComponents(); // shows them (only the maximised one when one is maximised)
    ComponentWidget* first = nullptr;
    if (const auto it = rememberedFocus_.constFind(key); it != rememberedFocus_.constEnd()) {
        first = componentById(it.value());
    }
    if (first == nullptr) {
        const QList<ComponentWidget*> tabOrder = focusOrder();
        first = tabOrder.isEmpty() ? nullptr : tabOrder.front();
    }
    if (first != nullptr) {
        selectFocused(first);
        if (keyboardInCanvas) {
            first->setFocus(Qt::OtherFocusReason);
        }
    }
    update();
}

void WorksheetCanvas::keepActive() {
    if (context_ == nullptr) {
        return;
    }
    KeptWorksheet page;
    page.worksheet = std::move(worksheet_);
    page.context = context_;
    page.components = std::move(components_);
    page.focused = focused_;
    page.maximised = maximised_;
    components_.clear();
    focused_.clear();
    maximised_.clear();
    worksheet_ = {};
    context_ = nullptr;
    for (const QPointer<ComponentWidget>& w : page.components) {
        if (!w.isNull()) {
            w->hide(); // a hidden time_distance graph keeps its Static image for 30 s (D-058)
        }
    }
    kept_.insert_or_assign(activeKey_, std::move(page));
    activeKey_.clear();
}

void WorksheetCanvas::createComponents(WorksheetContext& context) {
    std::vector<const project::ComponentSpec*> order;
    order.reserve(worksheet_.components.size());
    for (const project::ComponentSpec& spec : worksheet_.components) {
        order.push_back(&spec);
    }
    std::stable_sort(
        order.begin(), order.end(),
        [](const project::ComponentSpec* a, const project::ComponentSpec* b) { return a->z < b->z; });
    for (const project::ComponentSpec* spec : order) {
        ComponentWidget* w = ComponentWidget::create(*spec, context, this);
        components_.emplace_back(w);
        connect(w, &ComponentWidget::focusRequested, this, [this, w] { focusComponent(w); });
        connect(w, &ComponentWidget::maximiseRequested, this, [this, w] { toggleMaximise(w); });
        connect(w, &ComponentWidget::specEditRequested, this,
                [this, w](const project::ComponentSpec& next, const QString& undoText) {
                    applyEdit(w, next, undoText);
                });
    }
}

void WorksheetCanvas::clear() {
    std::vector<QPointer<ComponentWidget>> doomed;
    doomed.swap(components_);
    for (auto& [key, page] : kept_) {
        doomed.insert(doomed.end(), page.components.begin(), page.components.end());
    }
    kept_.clear();
    focused_.clear();
    maximised_.clear();
    worksheet_ = {};
    context_ = nullptr;
    activeKey_.clear();
    for (const QPointer<ComponentWidget>& w : doomed) {
        delete w.data();
    }
}

bool WorksheetCanvas::layoutThrottleActive() const {
    return layoutTimer_->isActive();
}

QList<ComponentWidget*> WorksheetCanvas::components() const {
    QList<ComponentWidget*> out;
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull()) {
            out.append(w.data());
        }
    }
    return out;
}

QList<ComponentWidget*> WorksheetCanvas::focusOrder() const {
    QList<ComponentWidget*> out = components();
    std::stable_sort(out.begin(), out.end(), [](const ComponentWidget* a, const ComponentWidget* b) {
        const project::ComponentSpec& sa = a->spec();
        const project::ComponentSpec& sb = b->spec();
        if (sa.z != sb.z) {
            return sa.z < sb.z;
        }
        if (sa.rect.t != sb.rect.t) {
            return sa.rect.t < sb.rect.t;
        }
        return sa.rect.l < sb.rect.l;
    });
    return out;
}

ComponentWidget* WorksheetCanvas::focusedComponent() const {
    return focused_.data();
}

ComponentWidget* WorksheetCanvas::componentById(const Uuid& id) const {
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull() && w->spec().id == id) {
            return w.data();
        }
    }
    return nullptr;
}

ComponentWidget* WorksheetCanvas::componentContaining(QWidget* widget) const {
    for (QWidget* p = widget; p != nullptr && p != this; p = p->parentWidget()) {
        if (auto* c = qobject_cast<ComponentWidget*>(p); c != nullptr && c->parentWidget() == this) {
            // Only the shown worksheet's components (kept ones are children of the canvas too).
            const bool shown = std::any_of(components_.begin(), components_.end(),
                                           [c](const QPointer<ComponentWidget>& w) { return w.data() == c; });
            return shown ? c : nullptr;
        }
    }
    return nullptr;
}

void WorksheetCanvas::focusComponent(ComponentWidget* component) {
    if (component != nullptr && componentContaining(component) != component) {
        return; // not one of ours
    }
    selectFocused(component);
    if (component != nullptr && componentContaining(QApplication::focusWidget()) != component) {
        component->setFocus(Qt::OtherFocusReason);
    }
}

void WorksheetCanvas::selectFocused(ComponentWidget* component) {
    if (focused_ != component) {
        ComponentWidget* previous = focused_.data();
        focused_ = component;
        if (previous != nullptr) {
            previous->setFocusedComponent(false);
        }
        if (component != nullptr) {
            component->setFocusedComponent(true);
            rememberedFocus_.insert(worksheetKey(worksheet_), component->spec().id);
        }
        Q_EMIT focusedComponentChanged(component);
    }
}

void WorksheetCanvas::onApplicationFocusChanged(QWidget*, QWidget* now) {
    if (ComponentWidget* c = componentContaining(now); c != nullptr && c != focused_) {
        focusComponent(c); // keyboard focus moved into another component (click, TAB, programmatic)
    }
}

void WorksheetCanvas::focusNext(bool backwards) {
    QList<ComponentWidget*> order = focusOrder();
    if (!maximised_.isNull()) {
        order = {maximised_.data()}; // only the maximised component is visible
    }
    if (order.isEmpty()) {
        return;
    }
    const qsizetype n = order.size();
    const qsizetype current = order.indexOf(focused_.data());
    qsizetype next = 0;
    if (current < 0) {
        next = backwards ? n - 1 : 0;
    } else {
        next = ((current + (backwards ? -1 : 1)) % n + n) % n;
    }
    focusComponent(order[next]);
}

bool WorksheetCanvas::focusNextPrevChild(bool next) {
    // TAB / SHIFT+TAB from inside a component cycle the components (04 section 7.2 item 1).
    if (!components_.empty()) {
        focusNext(!next);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

void WorksheetCanvas::returnFocus() {
    if (!focused_.isNull()) {
        focused_->setFocus(Qt::OtherFocusReason);
    } else {
        focusNext(false);
    }
}

void WorksheetCanvas::toggleMaximise(ComponentWidget* component) {
    if (component == nullptr) {
        return;
    }
    maximised_ = maximised_ == component ? nullptr : component;
    layoutComponents();
    if (!maximised_.isNull()) {
        focusComponent(maximised_.data());
    }
}

ComponentWidget* WorksheetCanvas::maximisedComponent() const {
    return maximised_.data();
}

bool WorksheetCanvas::restoreMaximised() {
    if (maximised_.isNull()) {
        return false;
    }
    maximised_.clear();
    layoutComponents();
    return true;
}

void WorksheetCanvas::applyScheme(const project::Scheme& scheme) {
    scheme_ = std::make_shared<const project::Scheme>(scheme);
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull()) {
            w->onSchemeChanged(scheme);
        }
    }
    for (auto& [key, page] : kept_) { // hidden: they rebuild when shown again
        for (const QPointer<ComponentWidget>& w : page.components) {
            if (!w.isNull()) {
                w->onSchemeChanged(scheme);
            }
        }
    }
    update();
}

void WorksheetCanvas::applyEdit(ComponentWidget* component, const project::ComponentSpec& spec,
                                const QString& undoText) {
    if (component == nullptr) {
        return;
    }
    for (project::ComponentSpec& c : worksheet_.components) {
        if (c.id == component->spec().id) {
            c = spec;
            break;
        }
    }
    component->applySpec(spec);
    layoutComponents(); // rect or z may have changed
    Q_EMIT componentEdited(spec, undoText);
}

bool WorksheetCanvas::event(QEvent* event) {
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal) {
        queueLayout(); // the components rebuild through their own DPR handling
        update();
    }
    return QWidget::event(event);
}

void WorksheetCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    queueLayout();
}

void WorksheetCanvas::queueLayout() {
    if (layoutTimer_->isActive()) {
        layoutPending_ = true; // coalesced: the timer lays out the latest size
        return;
    }
    layoutComponents(); // leading edge
    layoutTimer_->start();
}

void WorksheetCanvas::layoutComponents() {
    ++layoutPasses_;
    const QRect r = contentsRect();
    for (const QPointer<ComponentWidget>& p : components_) {
        ComponentWidget* w = p.data();
        if (w == nullptr) {
            continue;
        }
        if (!maximised_.isNull()) {
            w->setVisible(w == maximised_.data());
            w->setGeometry(r);
            continue;
        }
        w->setGeometry(placement(w->spec().rect, r));
        w->setVisible(true);
    }
}

void WorksheetCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const project::Rgba bg =
        scheme_ != nullptr ? scheme_->role("windowBackground", 0xFFFFFFFFU) : 0xFFFFFFFFU;
    painter.fillRect(event->rect(), colourOf(bg));
}

} // namespace esses::widgets
