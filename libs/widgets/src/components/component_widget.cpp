// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentWidget (04 section 7.1, 06 section 4.4): the factory, the context wiring, the 30 Hz Size
// coalescing, DPR changes, focus and the caption double-click (04 section 7.2 items 1 and 7).

#include <esses/maths/channel_key.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/placeholder_component.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/shell/key_event_router.hpp>

#include <QContextMenuEvent>
#include <QEvent>
#include <QHash>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace esses::widgets {

namespace {

QHash<QString, ComponentWidget::Factory>& factories() {
    static QHash<QString, ComponentWidget::Factory> table; // GUI thread only
    return table;
}

constexpr int kCaptionLogicalPx = 18; // 04 section 8.1 (scheme default)

} // namespace

ComponentWidget* ComponentWidget::create(const project::ComponentSpec& spec, WorksheetContext& context,
                                         QWidget* parent) {
    const QString type = QString::fromStdString(spec.type);
    const auto it = factories().constFind(type);
    if (spec.known && it != factories().constEnd()) {
        return it.value()(spec, context, parent);
    }
    return new PlaceholderComponent(spec, context, parent);
}

void ComponentWidget::registerType(const QString& type, Factory factory) {
    factories().insert(type, std::move(factory));
}

bool ComponentWidget::isRegistered(const QString& type) {
    return factories().contains(type);
}

ComponentWidget::ComponentWidget(const project::ComponentSpec& spec, WorksheetContext& context,
                                 QWidget* parent)
    : QWidget(parent), spec_(spec), context_(context), sizeTimer_(new QTimer(this)) {
    setFocusPolicy(Qt::StrongFocus);
    sizeTimer_->setSingleShot(true);
    sizeTimer_->setInterval(kSizeCoalesceMs);
    connect(sizeTimer_, &QTimer::timeout, this, [this] {
        if (sizePending_ != 0) {
            flushSizeChange();
            sizeTimer_->start(); // keep throttling while the burst lasts
        }
    });

    takeOwnView(!zoomLinked(), !cursorLinked(), !datumLinked());
    connect(&context_.view, &qt::WorksheetViewModel::changed, this, [this](plot::ChangeMask what) {
        // The worksheet's Size bit is its cursor-step width (WorksheetViewModel::setSize); a component's own
        // size reaches it through resizeEvent.
        what = static_cast<plot::ChangeMask>(what & ~plot::mask(plot::Change::Size));
        // Unlinked parts keep their own values (D-211 (f)); F9 converted the worksheet's zoom into the new
        // domain, and an own zoom restarts from it.
        if (plot::has(what, plot::Change::Domain) && !zoomLinked()) {
            own_.zoom = context_.view.state().zoom;
            ++ownVersion_;
        } else if (!zoomLinked()) {
            what = static_cast<plot::ChangeMask>(what & ~plot::mask(plot::Change::Zoom));
        }
        if (!cursorLinked()) {
            what = static_cast<plot::ChangeMask>(what & ~plot::mask(plot::Change::Cursor));
        }
        if (!datumLinked()) {
            what = static_cast<plot::ChangeMask>(what & ~plot::mask(plot::Change::Datum));
        }
        if (what != 0) {
            onViewChanged(viewState(), what);
        }
    });
    connect(&context_.selection, &qt::DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot& snapshot, qt::DataSelectionModel::Changes) {
                onSelectionChanged(snapshot);
            });
    const auto dataFromKey = [this](const maths::ChannelKey& key) { onChannelDataChanged(key); };
    connect(&context_.maths, &qt::MathsService::channelReady, this, dataFromKey);
    connect(&context_.maths, &qt::MathsService::statusChanged, this, dataFromKey);
    const auto dataFromSource = [this](const model::SourceId& source) { onDataChanged(source, 0); };
    qt::DistanceService& distance = context_.view.distanceService();
    connect(&distance, &qt::DistanceService::distanceReady, this, dataFromSource);
    connect(&distance, &qt::DistanceService::noDistance, this, dataFromSource);
}

ComponentWidget::~ComponentWidget() = default;

namespace {

ComponentWidget::KeysProvider& keysProvider() {
    static ComponentWidget::KeysProvider provider; // GUI thread only
    return provider;
}

} // namespace

QMenu* ComponentWidget::commandMenu(const QStringList& commandIds) {
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("componentContextMenu"));
    qt::CommandRegistry& registry = context_.commands;
    const QPointer<ComponentWidget> self(this);
    for (const QString& id : commandIds) {
        const qt::CommandSpec* spec = registry.find(id);
        if (spec == nullptr || !spec->invoke || (spec->enabled && !spec->enabled())) {
            continue; // not available on this component now: left out of its menu
        }
        QAction* a = menu->addAction(spec->title.isEmpty() ? id : spec->title);
        a->setObjectName(id);
        if (spec->checkable && spec->checked) {
            a->setCheckable(true);
            a->setChecked(spec->checked());
        }
        connect(a, &QAction::triggered, this, [self, id, &registry] {
            if (!self.isNull()) {
                Q_EMIT self->focusRequested(); // the shell's commands act on the focused component
                registry.invoke(id);
            }
        });
    }
    return menu;
}

void ComponentWidget::contextMenuEvent(QContextMenuEvent* event) {
    Q_EMIT focusRequested(); // a keyboard context menu as well as a right-click
    QStringList ids = contextCommands();
    if (!ids.contains(QStringLiteral("component.properties"))) {
        ids.append(QStringLiteral("component.properties")); // the Component menu's Properties... (F5)
    }
    QMenu* menu = commandMenu(ids);
    if (menu->isEmpty()) {
        delete menu;
        event->ignore();
        return;
    }
    menu->popup(event->globalPos()); // asynchronous: the component may go while the menu is open
    event->accept();
}

void ComponentWidget::setKeysProvider(KeysProvider provider) {
    keysProvider() = std::move(provider);
}

bool ComponentWidget::keyBound(const QString& commandId, const QKeyEvent& event,
                               const QStringList& defaults) {
    const QString text = KeyEventRouter::keyText(event);
    if (text.isEmpty()) {
        return false;
    }
    const QStringList keys = keysProvider() ? keysProvider()(commandId) : defaults;
    const QString key = qt::KeymapBinder::normalise(text);
    return std::any_of(keys.begin(), keys.end(),
                       [&key](const QString& k) { return qt::KeymapBinder::normalise(k) == key; });
}

void ComponentWidget::applySpec(const project::ComponentSpec& spec) {
    const bool zoomBefore = zoomLinked();
    const bool cursorBefore = cursorLinked();
    const bool datumBefore = datumLinked();
    const plot::ViewState before = viewState();
    spec_ = spec;
    // A part whose link is switched off starts from the worksheet's value; one switched on shows the
    // worksheet's again, delivered once the subclass has taken the new spec (D-211 (f)).
    takeOwnView(zoomBefore && !zoomLinked(), cursorBefore && !cursorLinked(), datumBefore && !datumLinked());
    const plot::ChangeMask changed = plot::viewChanges(before, viewState());
    if (changed != 0) {
        ++ownVersion_;
        QMetaObject::invokeMethod(
            this, [this, changed] { onViewChanged(viewState(), changed); }, Qt::QueuedConnection);
    }
    update();
}

// ---- links (M4, 04 section 2.4, D-211 (f)) ----------------------------------------------------------------

bool ComponentWidget::zoomLinked() const noexcept {
    // The outing graph is the zoom control (04 section 9 item 2): its zoom is always the worksheet's.
    return spec_.links.zoom || spec_.type == "outing";
}

plot::ViewState ComponentWidget::viewState() const {
    plot::ViewState v = context_.view.state();
    if (!zoomLinked()) {
        v.zoom = own_.zoom;
    }
    if (!cursorLinked()) {
        v.cursorT = own_.cursorT;
    }
    if (!datumLinked()) {
        v.datumT = own_.datumT;
    }
    v.version += ownVersion_;
    return v;
}

void ComponentWidget::takeOwnView(bool zoom, bool cursor, bool datum) {
    const plot::ViewState& ws = context_.view.state();
    if (zoom) {
        own_.zoom = ws.zoom;
    }
    if (cursor) {
        own_.cursorT = ws.cursorT;
    }
    if (datum) {
        own_.datumT = ws.datumT;
        own_.hiddenDatumT.reset();
    }
}

void ComponentWidget::ownViewChanged(plot::ChangeMask what) {
    ++ownVersion_;
    onViewChanged(viewState(), what);
}

std::optional<plot::DomainMapper> ComponentWidget::mainMapper() const {
    return std::nullopt;
}

std::optional<plot::DomainMapper> ComponentWidget::mainMapperOf(const plot::ComponentBuildInputs& inputs,
                                                                plot::Domain domain) {
    if (inputs.slots.empty()) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs.slots.front();
    plot::RangeGeom geom = main.range;
    geom.offsetUs = 0;
    geom.offsetM = 0.0;
    return plot::DomainMapper(domain, geom, main.outing, main.chain, Rate{}, 0);
}

plot::XSpan ComponentWidget::clampToOuting(plot::XSpan span,
                                           const std::optional<plot::DomainMapper>& map) const {
    if (!(span.b > span.a) || !std::isfinite(span.a) || !std::isfinite(span.b)) {
        return own_.zoom;
    }
    if (!map) {
        return span;
    }
    const plot::XSpan extent = map->outingExtent();
    if (!(extent.b > extent.a)) {
        return span;
    }
    const double length = std::min(span.length(), extent.length());
    double a = std::clamp(span.a, extent.a, extent.b - length);
    if (span.length() > extent.length()) {
        a = extent.a;
    }
    return plot::XSpan{a, a + length};
}

std::optional<double> ComponentWidget::ownCursorX(const std::optional<plot::DomainMapper>& map) const {
    const Micros t = viewState().cursorT;
    if (context_.view.domain() == plot::Domain::Time) {
        return static_cast<double>(t);
    }
    if (map && map->hasDistance()) {
        return map->xOf(map->range().start + t);
    }
    return std::nullopt;
}

std::optional<Micros> ComponentWidget::ownTimeAt(const std::optional<plot::DomainMapper>& map,
                                                 double x) const {
    if (!std::isfinite(x)) {
        return std::nullopt;
    }
    if (context_.view.domain() == plot::Domain::Time) {
        // x is microseconds from the Main range start: range-check before the conversion (UBSan).
        constexpr double kLimit = 9.0e18;
        return x > -kLimit && x < kLimit ? std::optional<Micros>(static_cast<Micros>(std::llround(x)))
                                         : std::nullopt;
    }
    if (map && map->hasDistance()) {
        return map->timeAt(x) - map->range().start;
    }
    return std::nullopt;
}

void ComponentWidget::setViewCursor(Micros tMainLap, qt::CursorSource source) {
    if (cursorLinked()) {
        context_.view.setCursor(tMainLap, source);
        return;
    }
    if (own_.cursorT != tMainLap) {
        own_.cursorT = tMainLap;
        ownViewChanged(plot::mask(plot::Change::Cursor));
    }
}

void ComponentWidget::setViewZoom(plot::XSpan span) {
    if (zoomLinked()) {
        context_.view.zoomTo(span);
        return;
    }
    const plot::XSpan next = clampToOuting(span, mainMapper());
    if (!(next == own_.zoom)) {
        own_.zoom = next;
        ownViewChanged(plot::mask(plot::Change::Zoom));
    }
}

void ComponentWidget::viewPanBy(double fractionOfSpan) {
    if (zoomLinked()) {
        context_.view.panBy(fractionOfSpan);
        return;
    }
    const double d = fractionOfSpan * own_.zoom.length();
    setViewZoom(plot::XSpan{own_.zoom.a + d, own_.zoom.b + d});
}

void ComponentWidget::viewZoomFull() {
    if (zoomLinked()) {
        context_.view.zoomFull();
        return;
    }
    if (const std::optional<plot::DomainMapper> map = mainMapper()) {
        setViewZoom(map->outingExtent());
    }
}

// The worksheet keyboard rows on an unlinked part of the focused component (the shell offers them to the
// component first): the WorksheetViewModel's rules applied to the component's own state.
bool ComponentWidget::handleOwnViewCommand(const QString& id) {
    const bool cursorRow = id.startsWith(QLatin1String("cursor."));
    const bool zoomRow = id.startsWith(QLatin1String("zoom.")) || id.startsWith(QLatin1String("pan."));
    const bool datumRow = id.startsWith(QLatin1String("datum."));
    if ((cursorRow && cursorLinked()) || (zoomRow && zoomLinked()) || (datumRow && datumLinked()) ||
        (!cursorRow && !zoomRow && !datumRow)) {
        return false;
    }
    const std::optional<plot::DomainMapper> map = mainMapper();
    const plot::ViewState v = viewState();
    const std::optional<double> cx = ownCursorX(map);
    // Cursor steps: one device pixel of the component per step (CTRL: ten), at least one sample period.
    const auto step = [&](int pixels) {
        if (!cx) {
            return false;
        }
        const double widthPx = std::max(1.0, static_cast<double>(width()) * devicePixelRatioF());
        const std::optional<Micros> t = ownTimeAt(map, *cx + v.zoom.length() / widthPx * pixels);
        if (!t) {
            return false;
        }
        Micros next = *t;
        if (const Micros period = context_.view.activeSamplePeriod(); period > 0) {
            const Micros moved = next - v.cursorT;
            const Micros steps = std::max<Micros>(1, (std::abs(moved) + period / 2) / period);
            next = v.cursorT + (moved < 0 ? -steps : steps) * period;
        }
        setViewCursor(next, qt::CursorSource::Keyboard);
        return true;
    };
    const auto cursorTo = [&](double x) {
        const std::optional<Micros> t = ownTimeAt(map, x);
        if (t) {
            setViewCursor(*t, qt::CursorSource::Keyboard);
        }
        return t.has_value();
    };
    const auto zoomAbout = [&](double factor) {
        if (!cx) {
            return false;
        }
        const double c = std::clamp(*cx, std::min(v.zoom.a, v.zoom.b), std::max(v.zoom.a, v.zoom.b));
        setViewZoom(plot::XSpan{c - (c - v.zoom.a) * factor, c + (v.zoom.b - c) * factor});
        return true;
    };
    if (id == QLatin1String("cursor.left") || id == QLatin1String("cursor.right")) {
        return step(id.endsWith(QLatin1String("left")) ? -1 : 1);
    }
    if (id == QLatin1String("cursor.left_fast") || id == QLatin1String("cursor.right_fast")) {
        return step(id.endsWith(QLatin1String("left_fast")) ? -10 : 10);
    }
    if (id == QLatin1String("cursor.home")) {
        return cursorTo(v.zoom.a);
    }
    if (id == QLatin1String("cursor.end")) {
        return cursorTo(std::nextafter(v.zoom.b, v.zoom.a));
    }
    if (id == QLatin1String("zoom.in")) {
        return zoomAbout(0.5);
    }
    if (id == QLatin1String("zoom.out")) {
        return zoomAbout(2.0);
    }
    if (id == QLatin1String("zoom.default_range")) {
        if (!map) {
            return false;
        }
        setViewZoom(plot::XSpan{0.0, map->extent()});
        return true;
    }
    if (id == QLatin1String("zoom.datum_main")) {
        if (!cx || !v.datumT) {
            return false;
        }
        const std::optional<double> dx = context_.view.domain() == plot::Domain::Time
                                             ? std::optional<double>(static_cast<double>(*v.datumT))
                                         : map && map->hasDistance()
                                             ? std::optional<double>(map->xOf(map->range().start + *v.datumT))
                                             : std::nullopt;
        if (!dx || *dx == *cx) {
            return false;
        }
        setViewZoom(plot::XSpan{std::min(*cx, *dx), std::max(*cx, *dx)});
        return true;
    }
    if (id == QLatin1String("pan.left") || id == QLatin1String("pan.right")) {
        viewPanBy(id.endsWith(QLatin1String("left")) ? -0.1 : 0.1);
        return true;
    }
    if (id == QLatin1String("pan.back") || id == QLatin1String("pan.forward")) {
        viewPanBy(id.endsWith(QLatin1String("back")) ? -0.9 : 0.9);
        return true;
    }
    if (id == QLatin1String("pan.centre_cursor")) {
        if (!cx) {
            return false;
        }
        const double half = 0.5 * v.zoom.length();
        setViewZoom(plot::XSpan{*cx - half, *cx + half});
        return true;
    }
    if (id == QLatin1String("datum.place")) {
        own_.datumT = v.cursorT;
        own_.hiddenDatumT.reset();
        ownViewChanged(plot::mask(plot::Change::Datum));
        return true;
    }
    if (id == QLatin1String("datum.toggle")) {
        if (own_.datumT) {
            own_.hiddenDatumT = own_.datumT;
            own_.datumT.reset();
        } else {
            own_.datumT = own_.hiddenDatumT.value_or(v.cursorT);
            own_.hiddenDatumT.reset();
        }
        ownViewChanged(plot::mask(plot::Change::Datum));
        return true;
    }
    if (id == QLatin1String("datum.swap")) {
        if (cursorLinked() || !own_.datumT) {
            return false; // the swap moves both: only when both are the component's own
        }
        std::swap(own_.cursorT, *own_.datumT);
        ownViewChanged(plot::Change::Cursor | plot::Change::Datum);
        return true;
    }
    return false;
}

// Base-class behaviours (04 section 7.1): components override what applies to them.
void ComponentWidget::onViewChanged(const plot::ViewState&, plot::ChangeMask) {}
void ComponentWidget::onSelectionChanged(const plot::SelectionSnapshot&) {}
void ComponentWidget::onDataChanged(const model::SourceId&, std::uint64_t) {}
void ComponentWidget::onChannelDataChanged(const maths::ChannelKey& key) {
    onDataChanged(key.src, 0);
}
void ComponentWidget::onSchemeChanged(const project::Scheme&) {}

ComponentWidget::OverlayPolicy ComponentWidget::overlayPolicy() const {
    return OverlayPolicy::MainOnly;
}

QStringList ComponentWidget::channelNames() const {
    return {};
}

std::optional<QString> ComponentWidget::activeChannel() const {
    if (spec_.activeChannel.empty()) {
        return std::nullopt;
    }
    return QString::fromStdString(spec_.activeChannel);
}

bool ComponentWidget::acceptsChannelDrop() const {
    return false;
}

void ComponentWidget::addChannel(const QString&, bool) {}

QStringList ComponentWidget::contextCommands() const {
    return {};
}

ComponentWidget::ExportJob ComponentWidget::exportJob(const ExportOptions&) const {
    return {};
}

bool ComponentWidget::exportData(ExportSink&, const ExportOptions&) const {
    return false; // CSV export arrives with the Component menu (M3)
}

QImage ComponentWidget::renderToImage(qreal dpr) const {
    dpr = dpr > 0.0 ? dpr : devicePixelRatioF();
    QImage img(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    const_cast<ComponentWidget*>(this)->render(&img); // QWidget::render is non-const
    return img;
}

void ComponentWidget::openProperties(int tabIndex) { // F5 / CTRL+N (06 section 12, D-044)
    PropertiesDialog::openFor(*this, tabIndex);
}

qt::CommandContext ComponentWidget::commandContext() const {
    return qt::CommandContext::Component;
}

plot::HitResult ComponentWidget::hitAt(QPointF) const {
    return {};
}

void ComponentWidget::setFocusedComponent(bool focused) {
    if (focusedComponent_ == focused) {
        return;
    }
    focusedComponent_ = focused;
    onFocusChanged(focused);
}

void ComponentWidget::onFocusChanged(bool) {
    update();
}

int ComponentWidget::captionHeight() const {
    return spec_.caption.show ? kCaptionLogicalPx : 0;
}

bool ComponentWidget::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        // A new DPR invalidates every layer and re-lays out the key column (06 section 17 item 3). Qt sends
        // the event to every widget of the window; one rebuild per real change.
        if (dpr_ > 0.0 && devicePixelRatioF() != dpr_) {
            queueSizeChange(plot::Change::Size | plot::Change::Scheme);
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void ComponentWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    queueSizeChange(plot::mask(plot::Change::Size));
}

void ComponentWidget::queueSizeChange(plot::ChangeMask what) {
    sizePending_ = static_cast<plot::ChangeMask>(sizePending_ | what);
    if (!sizeTimer_->isActive()) {
        flushSizeChange(); // leading edge: the first change of a burst at once
        sizeTimer_->start();
    }
}

bool ComponentWidget::sizeThrottleActive() const {
    return sizeTimer_->isActive();
}

bool ComponentWidget::handleCommand(const QString& id, const QVariantList&) {
    // M4 (D-211 (f)): the worksheet rows of an unlinked part act on the component's own view; otherwise the
    // shell's generic behaviour applies (M3).
    return handleOwnViewCommand(id);
}

bool ComponentWidget::cancelTransient() {
    return false;
}

void ComponentWidget::flushSizeChange() {
    const plot::ChangeMask what = sizePending_;
    sizePending_ = 0;
    if (what == 0) {
        return;
    }
    dpr_ = devicePixelRatioF();
    ++sizeDeliveries_;
    onViewChanged(context_.view.state(), what);
}

void ComponentWidget::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    Q_EMIT focusRequested();
}

void ComponentWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->position().y() < captionHeight()) {
        Q_EMIT maximiseRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace esses::widgets
