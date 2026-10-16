// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetCanvas (06 section 4.3, 04 section 7.2 items 1 and 7): placement, focus, maximise, 30 Hz relayout
// coalescing and the M1 edit path; M3: in-place definition updates, selection, move/resize/snap, splitters,
// nudge, z order and the layout lock.

#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace esses::widgets {

namespace {

QString worksheetKey(const project::WorksheetDef& ws) {
    return ws.id.isNil() ? QString::fromStdString(ws.name) : QString::fromStdString(ws.id.str());
}

QColor colourOf(project::Rgba rgba) {
    return QColor::fromRgb(static_cast<int>((rgba >> 24) & 0xFFU), static_cast<int>((rgba >> 16) & 0xFFU),
                           static_cast<int>((rgba >> 8) & 0xFFU), static_cast<int>(rgba & 0xFFU));
}

constexpr int kLeft = 1;
constexpr int kRight = 2;
constexpr int kTop = 4;
constexpr int kBottom = 8;
constexpr double kEdgeEps = 1e-6;

bool isAncestorOrSelf(const QWidget* ancestor, const QWidget* widget) {
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        if (w == ancestor) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- the selection overlay --------------------------------------------------------------------------------

class WorksheetCanvas::Overlay final : public QWidget {
public:
    explicit Overlay(WorksheetCanvas& canvas) : QWidget(&canvas), canvas_(canvas) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("selectionOverlay"));
        hide();
    }
    QRect band;

    void refresh() {
        const bool any = !band.isNull() || !canvas_.selection().isEmpty();
        setGeometry(canvas_.rect());
        setVisible(any);
        if (any) {
            raise();
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(1);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (ComponentWidget* c : canvas_.selection()) {
            if (c->isVisible()) {
                p.drawRect(c->geometry().adjusted(0, 0, -1, -1));
                const QRect g = c->geometry();
                for (const QPoint& corner : {g.topLeft(), g.topRight(), g.bottomLeft(), g.bottomRight()}) {
                    p.fillRect(QRect(corner - QPoint(2, 2), QSize(5, 5)), pen.color());
                }
            }
        }
        if (!band.isNull()) {
            pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            QColor fill = pen.color();
            fill.setAlpha(40);
            p.setBrush(fill);
            p.drawRect(band.normalized().adjusted(0, 0, -1, -1));
        }
    }

private:
    WorksheetCanvas& canvas_;
};

struct WorksheetCanvas::Gesture {
    enum class Kind : std::uint8_t { PendingMove, Move, Resize, Splitter, RubberBand };
    Kind kind = Kind::PendingMove;
    QPoint start; // canvas coordinates of the press
    QPointer<ComponentWidget> target;
    int edges = 0;         // Resize: kLeft | kRight | kTop | kBottom
    bool vertical = false; // Splitter: a vertical edge (x) or a horizontal one (y)
    bool noSnap = false;   // ALT held at the last move: the commit does not snap (06 section 4.3 item 2)
    double edgeFrom = 0.0; // Splitter: the edge's fraction at the press
    double edgeTo = 0.0;   // Splitter: where it is now
    bool additive = false; // RubberBand with CTRL: add to the selection
    std::vector<std::pair<Uuid, project::Rect>> startRects;
    std::vector<std::pair<Uuid, project::Rect>> current;
};

// ---- construction -----------------------------------------------------------------------------------------

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
    overlay_ = new Overlay(*this);
    if (qApp != nullptr) {
        connect(qApp, &QApplication::focusChanged, this, &WorksheetCanvas::onApplicationFocusChanged);
        qApp->installEventFilter(this); // mouse gestures over the components (M3)
    }
}

WorksheetCanvas::~WorksheetCanvas() {
    // ~QWidget clears the focus of a canvas that has it and QApplication::focusChanged follows: by then this
    // object is only a QWidget, so the slot must not run any more.
    if (qApp != nullptr) {
        disconnect(qApp, &QApplication::focusChanged, this, &WorksheetCanvas::onApplicationFocusChanged);
        qApp->removeEventFilter(this);
    }
    gesture_.reset();
}

QRect WorksheetCanvas::placement(const project::Rect& f, const QRect& r) {
    const int x0 = r.x() + static_cast<int>(std::lround(f.l * r.width()));
    const int y0 = r.y() + static_cast<int>(std::lround(f.t * r.height()));
    const int x1 = r.x() + static_cast<int>(std::lround(f.r * r.width()));
    const int y1 = r.y() + static_cast<int>(std::lround(f.b * r.height()));
    return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

// ---- worksheets -------------------------------------------------------------------------------------------

void WorksheetCanvas::setWorksheet(const project::WorksheetDef& worksheet, WorksheetContext& context) {
    const QString key = worksheetKey(worksheet);
    if (context_ == &context && activeKey_ == key) {
        if (!(worksheet_ == worksheet)) {
            syncInPlace(worksheet, context); // undo/redo, paste, delete, an edit from elsewhere (M3)
        }
        return;
    }
    finishGesture(false);
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
        // A renamed worksheet keeps its components (D-203 (ac)): the name is not theirs.
        project::WorksheetDef renamed = page.worksheet;
        renamed.name = worksheet.name;
        if (page.context == &context && renamed == worksheet) {
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
    sortAndRaise();
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

void WorksheetCanvas::syncInPlace(const project::WorksheetDef& worksheet, WorksheetContext& context) {
    finishGesture(false);
    QWidget* keyboard = QApplication::focusWidget();
    const bool keyboardInCanvas = keyboard != nullptr && (keyboard == this || isAncestorOf(keyboard));
    std::map<Uuid, ComponentWidget*> existing;
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull()) {
            existing.emplace(w->spec().id, w.data());
        }
    }
    std::vector<QPointer<ComponentWidget>> next;
    std::vector<ComponentWidget*> applied;
    for (const project::ComponentSpec& spec : worksheet.components) {
        const auto it = existing.find(spec.id);
        if (it != existing.end() && it->second->spec().type == spec.type &&
            it->second->spec().known == spec.known && it->second->spec().version == spec.version) {
            ComponentWidget* w = it->second;
            existing.erase(it);
            if (!(w->spec() == spec)) {
                w->applySpec(spec);
                applied.push_back(w);
            }
            next.emplace_back(w);
        } else {
            next.emplace_back(createComponent(spec, context));
        }
    }
    const bool lostFocus = focused_.isNull() || existing.count(focused_->spec().id) > 0;
    for (auto& [id, w] : existing) {
        if (w == maximised_.data()) {
            maximised_.clear();
        }
        delete w; // removed from the worksheet (delete, cut, undo of a paste)
    }
    components_ = std::move(next);
    worksheet_ = worksheet;
    pruneSelection();
    sortAndRaise();
    layoutComponents();
    if (lostFocus || focused_.isNull()) {
        focused_.clear();
        const QList<ComponentWidget*> order = focusOrder();
        ComponentWidget* first = order.isEmpty() ? nullptr : order.front();
        selectFocused(first);
        if (first != nullptr && keyboardInCanvas) {
            first->setFocus(Qt::OtherFocusReason);
        } else if (keyboardInCanvas && QApplication::focusWidget() == nullptr) {
            setFocus(Qt::OtherFocusReason);
        }
    }
    for (ComponentWidget* w : applied) {
        Q_EMIT componentSpecApplied(w);
    }
    overlay_->refresh();
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
    if (!selection_.empty()) {
        selection_.clear();
        Q_EMIT selectionChanged();
    }
    overlay_->refresh();
}

ComponentWidget* WorksheetCanvas::createComponent(const project::ComponentSpec& spec,
                                                  WorksheetContext& context) {
    ComponentWidget* w = ComponentWidget::create(spec, context, this);
    connect(w, &ComponentWidget::focusRequested, this, [this, w] { focusComponent(w); });
    connect(w, &ComponentWidget::maximiseRequested, this, [this, w] { toggleMaximise(w); });
    connect(w, &ComponentWidget::specEditRequested, this,
            [this, w](const project::ComponentSpec& next, const QString& undoText) {
                applyEdit(w, next, undoText);
            });
    return w;
}

void WorksheetCanvas::createComponents(WorksheetContext& context) {
    for (const project::ComponentSpec& spec : worksheet_.components) {
        components_.emplace_back(createComponent(spec, context));
    }
}

void WorksheetCanvas::sortAndRaise() {
    // Ascending z; equal z keeps the definition's array order.
    std::vector<QPointer<ComponentWidget>> live;
    for (const QPointer<ComponentWidget>& w : components_) {
        if (!w.isNull()) {
            live.push_back(w);
        }
    }
    const auto indexOf = [this](const ComponentWidget* w) {
        for (std::size_t i = 0; i < worksheet_.components.size(); ++i) {
            if (worksheet_.components[i].id == w->spec().id) {
                return i;
            }
        }
        return worksheet_.components.size();
    };
    std::stable_sort(live.begin(), live.end(),
                     [&](const QPointer<ComponentWidget>& a, const QPointer<ComponentWidget>& b) {
                         if (a->spec().z != b->spec().z) {
                             return a->spec().z < b->spec().z;
                         }
                         return indexOf(a.data()) < indexOf(b.data());
                     });
    components_ = std::move(live);
    for (const QPointer<ComponentWidget>& w : components_) {
        w->raise(); // ascending z: the last raised paints on top
    }
    overlay_->raise();
}

void WorksheetCanvas::clear() {
    finishGesture(false);
    std::vector<QPointer<ComponentWidget>> doomed;
    doomed.swap(components_);
    for (auto& [key, page] : kept_) {
        doomed.insert(doomed.end(), page.components.begin(), page.components.end());
    }
    kept_.clear();
    focused_.clear();
    maximised_.clear();
    selection_.clear();
    worksheet_ = {};
    context_ = nullptr;
    activeKey_.clear();
    for (const QPointer<ComponentWidget>& w : doomed) {
        delete w.data();
    }
    overlay_->refresh();
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
        priorFocused_ = previous;
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

project::ComponentSpec* WorksheetCanvas::specOf(const Uuid& id) {
    for (project::ComponentSpec& c : worksheet_.components) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

void WorksheetCanvas::applyEdit(ComponentWidget* component, const project::ComponentSpec& spec,
                                const QString& undoText) {
    if (component == nullptr) {
        return;
    }
    const bool shown =
        std::any_of(components_.begin(), components_.end(),
                    [component](const QPointer<ComponentWidget>& w) { return w == component; });
    if (!shown) {
        // A component of a kept (hidden) worksheet: the edit goes to that worksheet's definition and the
        // shell stores it there, so the widget never shows what the session does not hold (D-203 (k)).
        for (auto& [key, page] : kept_) {
            const bool inPage =
                std::any_of(page.components.begin(), page.components.end(),
                            [component](const QPointer<ComponentWidget>& w) { return w == component; });
            if (!inPage) {
                continue;
            }
            for (project::ComponentSpec& c : page.worksheet.components) {
                if (c.id == component->spec().id) {
                    c = spec;
                }
            }
            component->applySpec(spec);
            Q_EMIT componentSpecApplied(component);
            Q_EMIT keptComponentEdited(page.worksheet, spec, undoText);
            return;
        }
        return; // not this canvas's: nothing to store it in
    }
    if (project::ComponentSpec* c = specOf(component->spec().id)) {
        *c = spec;
    }
    component->applySpec(spec);
    layoutComponents(); // rect or z may have changed
    Q_EMIT componentSpecApplied(component);
    Q_EMIT componentEdited(spec, undoText);
}

QList<std::pair<QString, QString>> WorksheetCanvas::addableTypes() {
    return {{QStringLiteral("time_distance"), tr("Time/Distance Graph")},
            {QStringLiteral("outing"), tr("Outing Graph")},
            {QStringLiteral("scatter"), tr("Scatter Plot")},
            {QStringLiteral("histogram"), tr("Histogram")},
            {QStringLiteral("channel_report"), tr("Channel Report")},
            {QStringLiteral("time_report"), tr("Time Report")},
            {QStringLiteral("track_position"), tr("Track Position")},
            {QStringLiteral("values_list"), tr("Values List")},
            {QStringLiteral("gauge_numeric"), tr("Numeric Gauge")},
            {QStringLiteral("gauge_bar"), tr("Bar Gauge")}};
}

project::Rect WorksheetCanvas::placeForAdd(const project::WorksheetDef& sheet, QSize grid) {
    const int w = std::max(1, grid.width());
    const int h = std::max(1, grid.height());
    std::vector<std::uint8_t> busy(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    for (const project::ComponentSpec& c : sheet.components) {
        const int x0 = std::clamp(static_cast<int>(std::floor(c.rect.l * w + 1e-9)), 0, w);
        const int x1 = std::clamp(static_cast<int>(std::ceil(c.rect.r * w - 1e-9)), 0, w);
        const int y0 = std::clamp(static_cast<int>(std::floor(c.rect.t * h + 1e-9)), 0, h);
        const int y1 = std::clamp(static_cast<int>(std::ceil(c.rect.b * h - 1e-9)), 0, h);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                busy[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(x)] = 1;
            }
        }
    }
    // The largest free rectangle at least 20 % x 20 %: every maximal free rectangle is the widest run of
    // free columns at least as tall as one column's free height above a row (the histogram method).
    const int minW = static_cast<int>(std::ceil(0.2 * w - 1e-9));
    const int minH = static_cast<int>(std::ceil(0.2 * h - 1e-9));
    std::vector<int> heights(static_cast<std::size_t>(w), 0);
    long best = 0;
    project::Rect out{0.0, 0.0, 1.0, 1.0};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool free = busy[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(x)] == 0;
            heights[static_cast<std::size_t>(x)] = free ? heights[static_cast<std::size_t>(x)] + 1 : 0;
        }
        std::vector<int> stack;
        for (int x = 0; x <= w; ++x) {
            const int current = x < w ? heights[static_cast<std::size_t>(x)] : 0;
            while (!stack.empty() && heights[static_cast<std::size_t>(stack.back())] >= current) {
                const int tall = heights[static_cast<std::size_t>(stack.back())];
                stack.pop_back();
                const int left = stack.empty() ? 0 : stack.back() + 1;
                const int wide = x - left;
                const long area = static_cast<long>(tall) * wide;
                if (tall >= minH && wide >= minW && area > best) {
                    best = area;
                    out = project::Rect{static_cast<double>(left) / w, static_cast<double>(y + 1 - tall) / h,
                                        static_cast<double>(x) / w, static_cast<double>(y + 1) / h};
                }
            }
            stack.push_back(x);
        }
    }
    return out;
}

void WorksheetCanvas::contextMenuEvent(QContextMenuEvent* event) {
    const bool onComponent = std::any_of(components_.begin(), components_.end(), [event](const auto& w) {
        return !w.isNull() && w->isVisible() && w->geometry().contains(event->pos());
    });
    if (context_ == nullptr || onComponent) {
        event->ignore();
        return;
    }
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("canvasContextMenu"));
    QMenu* add = menu->addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("canvasAddMenu"));
    qt::CommandRegistry& registry = context_->commands;
    for (const auto& [type, title] : addableTypes()) {
        QAction* a = add->addAction(title);
        a->setObjectName(type);
        a->setEnabled(!locked_);
        connect(a, &QAction::triggered, this,
                [&registry, id = type] { registry.invoke(QStringLiteral("component.add"), {id}); });
    }
    menu->popup(event->globalPos());
    event->accept();
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
    overlay_->refresh();
}

void WorksheetCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const project::Rgba bg =
        scheme_ != nullptr ? scheme_->role("windowBackground", 0xFFFFFFFFU) : 0xFFFFFFFFU;
    painter.fillRect(event->rect(), colourOf(bg));
}

// ---- selection (06 section 4.3 item 5) --------------------------------------------------------------------

QList<ComponentWidget*> WorksheetCanvas::selection() const {
    QList<ComponentWidget*> out;
    for (ComponentWidget* c : components()) {
        if (isSelected(c)) {
            out.append(c);
        }
    }
    return out;
}

QList<ComponentWidget*> WorksheetCanvas::selectedOrFocused() const {
    QList<ComponentWidget*> out = selection();
    if (out.isEmpty() && !focused_.isNull()) {
        out.append(focused_.data());
    }
    return out;
}

bool WorksheetCanvas::isSelected(const ComponentWidget* component) const {
    return component != nullptr &&
           std::any_of(selection_.begin(), selection_.end(),
                       [component](const QPointer<ComponentWidget>& w) { return w.data() == component; });
}

void WorksheetCanvas::setSelection(const QList<ComponentWidget*>& list) {
    std::vector<QPointer<ComponentWidget>> next;
    for (ComponentWidget* c : list) {
        if (componentContaining(c) == c &&
            std::none_of(next.begin(), next.end(),
                         [c](const QPointer<ComponentWidget>& w) { return w.data() == c; })) {
            next.emplace_back(c);
        }
    }
    const bool same = next.size() == selection_.size() &&
                      std::equal(next.begin(), next.end(), selection_.begin(),
                                 [](const QPointer<ComponentWidget>& a, const QPointer<ComponentWidget>& b) {
                                     return a.data() == b.data();
                                 });
    selection_ = std::move(next);
    overlay_->refresh();
    if (!same) {
        Q_EMIT selectionChanged();
    }
}

void WorksheetCanvas::toggleSelected(ComponentWidget* component) {
    QList<ComponentWidget*> s = selection();
    if (s.isEmpty()) {
        // CTRL+click on a second component selects both: the focused one, or the one focused before the
        // click moved the keyboard focus here (a press focuses before the canvas sees it).
        ComponentWidget* implicit = focused_ != component ? focused_.data() : priorFocused_.data();
        if (implicit != nullptr && implicit != component && componentContaining(implicit) == implicit) {
            s.append(implicit);
        }
    }
    if (s.contains(component)) {
        s.removeAll(component);
    } else {
        s.append(component);
    }
    setSelection(s);
}

void WorksheetCanvas::selectAll() {
    setSelection(components());
}

void WorksheetCanvas::clearSelection() {
    setSelection({});
}

void WorksheetCanvas::selectIds(const std::vector<Uuid>& ids) {
    QList<ComponentWidget*> s;
    for (const Uuid& id : ids) {
        if (ComponentWidget* c = componentById(id)) {
            s.append(c);
        }
    }
    setSelection(s);
}

void WorksheetCanvas::pruneSelection() {
    const std::size_t before = selection_.size();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [this](const QPointer<ComponentWidget>& w) {
                                        return w.isNull() || componentContaining(w.data()) != w.data();
                                    }),
                     selection_.end());
    if (selection_.size() != before) {
        Q_EMIT selectionChanged();
    }
}

// ---- geometry edits (06 section 4.3 items 2, 4, 8, 9, 11) -------------------------------------------------

void WorksheetCanvas::setLayoutLocked(bool locked) {
    if (locked && gesture_ != nullptr && gesture_->kind != Gesture::Kind::RubberBand) {
        finishGesture(false);
    }
    locked_ = locked;
}

QSize WorksheetCanvas::grid() const {
    const int cols = worksheet_.grid[0] >= 1 && worksheet_.grid[0] <= 1000 ? worksheet_.grid[0] : 100;
    const int rows = worksheet_.grid[1] >= 1 && worksheet_.grid[1] <= 1000 ? worksheet_.grid[1] : 100;
    return {cols, rows};
}

project::Rect WorksheetCanvas::snapped(const project::Rect& rect, QSize g, bool keepSize) {
    const int cols = std::max(1, g.width());
    const int rows = std::max(1, g.height());
    const int minX = std::min(kMinCells, cols);
    const int minY = std::min(kMinCells, rows);
    const auto axis = [keepSize](double a, double b, int cells, int minCells) {
        auto lo = static_cast<int>(std::lround(a * cells));
        auto hi = static_cast<int>(std::lround(b * cells));
        if (keepSize) {
            const int size = std::clamp(static_cast<int>(std::lround((b - a) * cells)), minCells, cells);
            lo = std::clamp(lo, 0, cells - size);
            hi = lo + size;
        } else {
            lo = std::clamp(lo, 0, cells);
            hi = std::clamp(hi, 0, cells);
            if (hi - lo < minCells) {
                hi = std::min(cells, lo + minCells);
                lo = hi - minCells;
            }
        }
        return std::pair{static_cast<double>(lo) / cells, static_cast<double>(hi) / cells};
    };
    const auto [l, r] = axis(rect.l, rect.r, cols, minX);
    const auto [t, b] = axis(rect.t, rect.b, rows, minY);
    return project::Rect{l, t, r, b};
}

bool WorksheetCanvas::setRects(const std::vector<std::pair<Uuid, project::Rect>>& rects,
                               const QString& undoText) {
    if (locked_ || context_ == nullptr) {
        return false;
    }
    std::vector<ComponentWidget*> changed;
    for (const auto& [id, rect] : rects) {
        project::ComponentSpec* spec = specOf(id);
        ComponentWidget* w = componentById(id);
        if (spec == nullptr || w == nullptr || spec->rect == rect) {
            continue;
        }
        spec->rect = rect;
        w->applySpec(*spec);
        changed.push_back(w);
    }
    layoutComponents();
    if (changed.empty()) {
        return false;
    }
    for (ComponentWidget* w : changed) {
        Q_EMIT componentSpecApplied(w);
    }
    Q_EMIT layoutEdited(
        undoText, changed.size() == 1 ? QString::fromStdString(changed.front()->spec().id.str()) : QString());
    return true;
}

bool WorksheetCanvas::nudgeSelection(int dxCells, int dyCells) {
    if (locked_ || (dxCells == 0 && dyCells == 0)) {
        return false;
    }
    const QSize g = grid();
    const double dx = static_cast<double>(dxCells) / g.width();
    const double dy = static_cast<double>(dyCells) / g.height();
    std::vector<std::pair<Uuid, project::Rect>> rects;
    for (ComponentWidget* c : selectedOrFocused()) {
        project::Rect r = c->spec().rect;
        const double w = r.r - r.l;
        const double h = r.b - r.t;
        r.l = std::clamp(r.l + dx, 0.0, std::max(0.0, 1.0 - w));
        r.t = std::clamp(r.t + dy, 0.0, std::max(0.0, 1.0 - h));
        r.r = r.l + w;
        r.b = r.t + h;
        rects.emplace_back(c->spec().id, r);
    }
    return setRects(rects, tr("Move components"));
}

bool WorksheetCanvas::moveSharedEdge(bool vertical, double from, double to, bool snap) {
    if (locked_) {
        return false;
    }
    const QSize g = grid();
    const int cells = vertical ? g.width() : g.height();
    if (snap) {
        to = std::round(to * cells) / cells;
    }
    const double minSize = static_cast<double>(std::min(kMinCells, cells)) / cells;
    std::vector<std::pair<Uuid, project::Rect>> rects;
    // The limits that keep every touching component at least the minimum size.
    double lo = 0.0;
    double hi = 1.0;
    for (ComponentWidget* c : components()) {
        const project::Rect& r = c->spec().rect;
        const double a = vertical ? r.l : r.t;
        const double b = vertical ? r.r : r.b;
        if (std::abs(b - from) < kEdgeEps) {
            lo = std::max(lo, a + minSize); // its far edge moves: keep the minimum width
        }
        if (std::abs(a - from) < kEdgeEps) {
            hi = std::min(hi, b - minSize);
        }
    }
    if (lo > hi) {
        return false;
    }
    to = std::clamp(to, lo, hi);
    for (ComponentWidget* c : components()) {
        project::Rect r = c->spec().rect;
        double& a = vertical ? r.l : r.t;
        double& b = vertical ? r.r : r.b;
        bool touched = false;
        if (std::abs(b - from) < kEdgeEps) {
            b = to;
            touched = true;
        }
        if (std::abs(a - from) < kEdgeEps) {
            a = to;
            touched = true;
        }
        if (touched) {
            rects.emplace_back(c->spec().id, r);
        }
    }
    return setRects(rects, tr("Move splitter"));
}

namespace {

// z values 0..n-1 in paint order after `moved` was placed first (background) or last (front).
std::vector<std::pair<Uuid, int>> renumbered(const project::WorksheetDef& ws, const Uuid& moved,
                                             bool toBack) {
    std::vector<std::pair<int, std::size_t>> order; // (z, array index)
    for (std::size_t i = 0; i < ws.components.size(); ++i) {
        int z = ws.components[i].z;
        if (ws.components[i].id == moved) {
            z = toBack ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
        }
        order.emplace_back(z, i);
    }
    std::stable_sort(order.begin(), order.end());
    std::vector<std::pair<Uuid, int>> out;
    for (std::size_t k = 0; k < order.size(); ++k) {
        out.emplace_back(ws.components[order[k].second].id, static_cast<int>(k));
    }
    return out;
}

} // namespace

bool WorksheetCanvas::sendToBackground(ComponentWidget* component) {
    if (locked_ || component == nullptr || componentContaining(component) != component) {
        return false;
    }
    bool changed = false;
    for (const auto& [id, z] : renumbered(worksheet_, component->spec().id, true)) {
        project::ComponentSpec* spec = specOf(id);
        if (spec != nullptr && spec->z != z) {
            spec->z = z;
            changed = true;
            if (ComponentWidget* w = componentById(id)) {
                w->applySpec(*spec);
            }
        }
    }
    if (!changed) {
        return false;
    }
    sortAndRaise();
    layoutComponents();
    Q_EMIT layoutEdited(tr("Send to background"), QString());
    return true;
}

bool WorksheetCanvas::bringToFront(ComponentWidget* component) {
    if (locked_ || component == nullptr || componentContaining(component) != component) {
        return false;
    }
    bool changed = false;
    for (const auto& [id, z] : renumbered(worksheet_, component->spec().id, false)) {
        project::ComponentSpec* spec = specOf(id);
        if (spec != nullptr && spec->z != z) {
            spec->z = z;
            changed = true;
            if (ComponentWidget* w = componentById(id)) {
                w->applySpec(*spec);
            }
        }
    }
    if (!changed) {
        return false;
    }
    sortAndRaise();
    layoutComponents();
    Q_EMIT layoutEdited(tr("Bring to front"), QString());
    return true;
}

void WorksheetCanvas::keyPressEvent(QKeyEvent* event) {
    // ALT+arrows nudge the selection one grid cell (06 section 4.3 item 4) when no keymap row took the key.
    if (event->modifiers() == Qt::AltModifier) {
        int dx = 0;
        int dy = 0;
        switch (event->key()) {
        case Qt::Key_Left:
            dx = -1;
            break;
        case Qt::Key_Right:
            dx = 1;
            break;
        case Qt::Key_Up:
            dy = -1;
            break;
        case Qt::Key_Down:
            dy = 1;
            break;
        default:
            break;
        }
        if ((dx != 0 || dy != 0) && nudgeSelection(dx, dy)) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

// ---- mouse gestures ---------------------------------------------------------------------------------------

QPointF WorksheetCanvas::fraction(QPointF canvasPos) const {
    const QRect r = contentsRect();
    return {(canvasPos.x() - r.x()) / std::max(1, r.width()),
            (canvasPos.y() - r.y()) / std::max(1, r.height())};
}

bool WorksheetCanvas::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type t = event->type();
    if (t != QEvent::MouseButtonPress && t != QEvent::MouseMove && t != QEvent::MouseButtonRelease) {
        return false;
    }
    auto* receiver = qobject_cast<QWidget*>(watched);
    if (receiver == nullptr || !isAncestorOrSelf(this, receiver) || !isVisible()) {
        return false;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    switch (t) {
    case QEvent::MouseButtonPress:
        return mousePress(receiver, mouse);
    case QEvent::MouseMove:
        return mouseMove(mouse);
    case QEvent::MouseButtonRelease:
        return mouseRelease(mouse);
    default:
        break;
    }
    return false;
}

bool WorksheetCanvas::mousePress(QWidget* receiver, QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || gesture_ != nullptr) {
        return gesture_ != nullptr;
    }
    const QPoint at = mapFromGlobal(event->globalPosition().toPoint());
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    if (receiver == this) { // empty canvas: a rubber band selects by intersection
        if (!maximised_.isNull() || components_.empty()) {
            return false;
        }
        gesture_ = std::make_unique<Gesture>();
        gesture_->kind = Gesture::Kind::RubberBand;
        gesture_->start = at;
        gesture_->additive = ctrl;
        if (!ctrl) {
            clearSelection();
        }
        setFocus(Qt::MouseFocusReason);
        return true;
    }
    ComponentWidget* c = componentContaining(receiver);
    if (c == nullptr || !maximised_.isNull()) {
        return false;
    }
    const QPoint local = c->mapFromGlobal(event->globalPosition().toPoint());
    int edges = 0;
    if (local.x() < kEdgeZonePx) {
        edges |= kLeft;
    } else if (local.x() >= c->width() - kEdgeZonePx) {
        edges |= kRight;
    }
    if (local.y() < kEdgeZonePx) {
        edges |= kTop;
    } else if (local.y() >= c->height() - kEdgeZonePx) {
        edges |= kBottom;
    }
    // A plain press on a control the component draws along its edge (a scrollbar, a handle, the offset
    // axis, the outing's zoom rectangle) is the component's; CTRL (the splitter) and the rest of the edge
    // zone are the canvas's.
    if (edges != 0 && !ctrl) {
        switch (c->hitAt(QPointF(local)).kind) {
        case plot::HitKind::ScrollBar:
        case plot::HitKind::Handle:
        case plot::HitKind::OffsetAxis:
        case plot::HitKind::ZoomSpan:
            edges = 0;
            break;
        default:
            break;
        }
    }
    if (edges != 0 && !locked_) {
        focusComponent(c);
        gesture_ = std::make_unique<Gesture>();
        gesture_->start = at;
        gesture_->target = c;
        const project::Rect& r = c->spec().rect;
        if (ctrl) { // splitter: the vertical edge when one was hit, else the horizontal one
            gesture_->kind = Gesture::Kind::Splitter;
            gesture_->vertical = (edges & (kLeft | kRight)) != 0;
            gesture_->edgeFrom =
                gesture_->vertical ? ((edges & kLeft) != 0 ? r.l : r.r) : ((edges & kTop) != 0 ? r.t : r.b);
            gesture_->edgeTo = gesture_->edgeFrom;
        } else {
            gesture_->kind = Gesture::Kind::Resize;
            gesture_->edges = edges;
            gesture_->startRects = {{c->spec().id, r}};
        }
        return true;
    }
    if (ctrl && !shift) { // CTRL+click toggles membership (06 section 4.3 item 5)
        toggleSelected(c);
        focusComponent(c);
        return true;
    }
    if (!locked_ && (shift || local.y() < c->captionBarHeight())) {
        if (!isSelected(c)) {
            setSelection({});
        }
        focusComponent(c);
        gesture_ = std::make_unique<Gesture>();
        gesture_->kind = Gesture::Kind::PendingMove;
        gesture_->start = at;
        gesture_->target = c;
        for (ComponentWidget* m : selectedOrFocused()) {
            gesture_->startRects.emplace_back(m->spec().id, m->spec().rect);
        }
        return true;
    }
    if (!isSelected(c)) {
        setSelection({}); // a plain click: the focused component alone
    }
    return false; // the component handles its own clicks
}

bool WorksheetCanvas::mouseMove(QMouseEvent* event) {
    if (gesture_ == nullptr) {
        return false;
    }
    Gesture& g = *gesture_;
    const QPoint at = mapFromGlobal(event->globalPosition().toPoint());
    const bool snap = !event->modifiers().testFlag(Qt::AltModifier);
    const QSize cells = grid();
    if (g.kind == Gesture::Kind::RubberBand) {
        overlay_->band = QRect(g.start, at).normalized();
        overlay_->refresh();
        return true;
    }
    if (g.kind == Gesture::Kind::PendingMove) {
        if ((at - g.start).manhattanLength() < kDragThresholdPx) {
            return true;
        }
        g.kind = Gesture::Kind::Move;
    }
    const QPointF d = fraction(at) - fraction(g.start);
    g.current.clear();
    if (g.kind == Gesture::Kind::Move) {
        for (const auto& [id, r0] : g.startRects) {
            const double w = r0.r - r0.l;
            const double h = r0.b - r0.t;
            project::Rect r = r0;
            r.l = std::clamp(r0.l + d.x(), 0.0, std::max(0.0, 1.0 - w));
            r.t = std::clamp(r0.t + d.y(), 0.0, std::max(0.0, 1.0 - h));
            r.r = r.l + w;
            r.b = r.t + h;
            g.current.emplace_back(id, snap ? snapped(r, cells, true) : r);
        }
    } else if (g.kind == Gesture::Kind::Resize && !g.startRects.empty()) {
        const project::Rect r0 = g.startRects.front().second;
        const double minW = static_cast<double>(std::min(kMinCells, cells.width())) / cells.width();
        const double minH = static_cast<double>(std::min(kMinCells, cells.height())) / cells.height();
        const auto snapX = [&](double v) { return snap ? std::round(v * cells.width()) / cells.width() : v; };
        const auto snapY = [&](double v) {
            return snap ? std::round(v * cells.height()) / cells.height() : v;
        };
        project::Rect r = r0;
        if ((g.edges & kLeft) != 0) {
            r.l = std::clamp(snapX(r0.l + d.x()), 0.0, r0.r - minW);
        }
        if ((g.edges & kRight) != 0) {
            r.r = std::clamp(snapX(r0.r + d.x()), r0.l + minW, 1.0);
        }
        if ((g.edges & kTop) != 0) {
            r.t = std::clamp(snapY(r0.t + d.y()), 0.0, r0.b - minH);
        }
        if ((g.edges & kBottom) != 0) {
            r.b = std::clamp(snapY(r0.b + d.y()), r0.t + minH, 1.0);
        }
        g.current.emplace_back(g.startRects.front().first, r);
    } else if (g.kind == Gesture::Kind::Splitter) {
        g.noSnap = !snap;
        g.edgeTo = std::clamp(g.edgeFrom + (g.vertical ? d.x() : d.y()), 0.0, 1.0);
        // Preview: place every touching component as the commit will.
        for (ComponentWidget* c : components()) {
            project::Rect r = c->spec().rect;
            double& a = g.vertical ? r.l : r.t;
            double& b = g.vertical ? r.r : r.b;
            bool touched = false;
            if (std::abs(b - g.edgeFrom) < kEdgeEps && g.edgeTo > a) {
                b = g.edgeTo;
                touched = true;
            }
            if (std::abs(a - g.edgeFrom) < kEdgeEps && g.edgeTo < b) {
                a = g.edgeTo;
                touched = true;
            }
            if (touched) {
                g.current.emplace_back(c->spec().id, r);
            }
        }
    }
    // Live preview: the widgets move, the definition changes on release.
    const QRect content = contentsRect();
    for (const auto& [id, r] : g.current) {
        if (ComponentWidget* c = componentById(id)) {
            c->setGeometry(placement(r, content));
        }
    }
    overlay_->refresh();
    return true;
}

bool WorksheetCanvas::mouseRelease(QMouseEvent* event) {
    if (gesture_ == nullptr || event->button() != Qt::LeftButton) {
        return gesture_ != nullptr;
    }
    finishGesture(true);
    return true;
}

void WorksheetCanvas::finishGesture(bool commit) {
    if (gesture_ == nullptr) {
        return;
    }
    const std::unique_ptr<Gesture> g = std::move(gesture_);
    if (g->kind == Gesture::Kind::RubberBand) {
        const QRect band = overlay_->band;
        overlay_->band = QRect();
        if (commit && !band.isNull()) {
            QList<ComponentWidget*> s = g->additive ? selection() : QList<ComponentWidget*>{};
            for (ComponentWidget* c : components()) {
                if (c->geometry().intersects(band) && !s.contains(c)) {
                    s.append(c);
                }
            }
            setSelection(s);
            if (!s.isEmpty()) {
                focusComponent(s.front());
            }
        }
        overlay_->refresh();
        return;
    }
    if (!commit || g->kind == Gesture::Kind::PendingMove) {
        layoutComponents(); // back to the definition
        return;
    }
    if (g->kind == Gesture::Kind::Splitter) {
        layoutComponents();
        moveSharedEdge(g->vertical, g->edgeFrom, g->edgeTo, !g->noSnap);
        return;
    }
    const QString text = g->kind == Gesture::Kind::Move ? tr("Move components") : tr("Resize component");
    if (!setRects(g->current, text)) {
        layoutComponents();
    }
}

} // namespace esses::widgets
