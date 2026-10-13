// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceGraph (04 sections 6.2-6.3 and 8; AC-M1-01, -02, -09): the layer pipeline, the mouse,
// like-units grouping and the scale edits.

#include <esses/base/rate.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/model/resolver.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>

#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <variant>

namespace esses::widgets {

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // the builders' "..." for a value still pending
constexpr double kWheelZoomIn = 0.8;                   // 04 section 8.6: x0.8 / x1.25 per notch
constexpr plot::Rgba kBandFallback = 0x1A73E8FFU;

plot::Rgba withAlpha(plot::Rgba rgba, std::uint8_t alpha) {
    return (rgba & 0xFFFFFF00U) | alpha;
}

std::shared_ptr<const project::Scheme> schemeOrDefault(std::shared_ptr<const project::Scheme> scheme) {
    return scheme != nullptr ? std::move(scheme) : std::make_shared<const project::Scheme>();
}

} // namespace

TimeDistanceGraph::TimeDistanceGraph(const project::ComponentSpec& spec, WorksheetContext& context,
                                     QWidget* parent)
    : ComponentWidget(spec, context, parent), pendingTimer_(new QTimer(this)), retryTimer_(new QTimer(this)),
      releaseTimer_(new QTimer(this)) {
    if (auto parsed = plot::TimeDistanceBody::parse(spec.body)) {
        body_ = std::move(parsed).value();
    }
    setAttribute(Qt::WA_OpaquePaintEvent); // the Static image covers the component (background Clear)
    setMouseTracking(false);
    setScheme(schemeOrDefault(context.scheme));
    pendingTimer_->setSingleShot(true);
    pendingTimer_->setInterval(kPendingStripeDelayMs);
    connect(pendingTimer_, &QTimer::timeout, this, [this] {
        if (buildInFlight_) {
            setPendingShown(true);
        }
    });
    retryTimer_->setSingleShot(true);
    retryTimer_->setInterval(kValueRetryMs);
    connect(retryTimer_, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            rebuildDynamic();
        }
    });
    releaseTimer_->setSingleShot(true);
    releaseTimer_->setInterval(kReleaseHiddenMs);
    connect(releaseTimer_, &QTimer::timeout, this, [this] {
        if (!isVisible()) {
            layers_.releaseStatic();
            staticDirty_ = true;
        }
    });
}

TimeDistanceGraph::~TimeDistanceGraph() {
    context_.render.cancelPending(context_.worksheetId, componentId());
}

QString TimeDistanceGraph::componentId() const {
    if (spec_.id.isNil()) { // never in a loaded workbook (ids are derived); keep keys apart anyway
        return QStringLiteral("anon-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
    }
    return QString::fromStdString(spec_.id.str());
}

void TimeDistanceGraph::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    scheme_ = std::move(scheme);
    ++schemeGeneration_;
    fonts_ = std::make_shared<const qt::SchemeFonts>(*scheme_, schemeGeneration_);
    metrics_ = qt::QtTextMetrics::create(*scheme_, schemeGeneration_);
    layers_.setFonts(fonts_);
}

// ---- component interface --------------------------------------------------------------------------------

void TimeDistanceGraph::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    if (auto parsed = plot::TimeDistanceBody::parse(spec.body)) {
        body_ = std::move(parsed).value();
    } else {
        body_ = plot::TimeDistanceBody{};
    }
    onViewChanged(context_.view.state(), plot::kFullRebuild);
}

void TimeDistanceGraph::onViewChanged(const plot::ViewState& view, plot::ChangeMask what) {
    inputs_.view = view;
    layers_.invalidate(what);
    if ((what & static_cast<plot::ChangeMask>(plot::kStaticRebuild | plot::kFullRebuild)) != 0) {
        // Static (and Dynamic/Chrome with it) after the inputs are prepared again.
        staticDirty_ = true;
        scheduleStaticBuild();
        return;
    }
    if ((what & plot::kDynamicOnly) != 0) {
        rebuildDynamic(); // Cursor/Datum: Dynamic only, never a Static build (04 section 2.2)
    }
}

void TimeDistanceGraph::onSelectionChanged(const plot::SelectionSnapshot&) {
    // The snapshot is read again when the build is prepared (the WorksheetViewModel's Selection bit, which
    // may arrive first, coalesces with this one).
    onViewChanged(context_.view.state(), plot::mask(plot::Change::Selection));
}

void TimeDistanceGraph::onDataChanged(const model::SourceId& source, std::uint64_t) {
    if (showsSource(source)) {
        onViewChanged(context_.view.state(), plot::mask(plot::Change::Data));
    }
}

void TimeDistanceGraph::onSchemeChanged(const project::Scheme& scheme) {
    setScheme(std::make_shared<const project::Scheme>(scheme));
    onViewChanged(context_.view.state(), plot::mask(plot::Change::Scheme));
}

QStringList TimeDistanceGraph::channelNames() const {
    QStringList names;
    for (const std::string& n : body_.channelNames()) {
        names.append(QString::fromStdString(n));
    }
    return names;
}

std::optional<QString> TimeDistanceGraph::activeChannel() const {
    return ComponentWidget::activeChannel();
}

QStringList TimeDistanceGraph::contextCommands() const {
    return {QStringLiteral("zoom.in"),           QStringLiteral("zoom.out"),
            QStringLiteral("zoom.full"),         QStringLiteral("zoom.default_range"),
            QStringLiteral("pan.centre_cursor"), QStringLiteral("datum.place"),
            QStringLiteral("datum.toggle"),      QStringLiteral("data.toggle_domain")};
}

bool TimeDistanceGraph::showsSource(const model::SourceId& source) const {
    for (const plot::Slot& slot : context_.selection.snapshot().visibleSlots()) {
        if (slot.range.src == source) {
            return true;
        }
    }
    return false;
}

// ---- edits (specEditRequested; the canvas applies them) ------------------------------------------------

void TimeDistanceGraph::requestBody(const plot::TimeDistanceBody& body, const QString& undoText) {
    project::ComponentSpec next = spec_;
    next.body = body.toJson();
    Q_EMIT specEditRequested(next, undoText);
}

std::optional<units::QuantityId> TimeDistanceGraph::quantityOf(const std::string& channel) const {
    const plot::SelectionSnapshot& s = context_.selection.snapshot();
    if (!context_.selection.hasMain()) {
        return std::nullopt;
    }
    const auto src = context_.selection.source(s.main.src);
    if (src == nullptr) {
        return std::nullopt;
    }
    const std::string layer(model::canonicalLayerName(s.main.layer));
    if (context_.maths.isMathsChannel(src->id, layer, channel)) {
        if (const auto d = context_.maths.descriptor(maths::ChannelKey::make(src->id, layer, channel))) {
            return d->quantity;
        }
        return std::nullopt;
    }
    const model::DataLayer* l = src->layer(layer);
    if (l == nullptr) {
        return std::nullopt;
    }
    const model::ChannelResolver resolver({}, {}, context_.maths.units().get());
    const model::ChannelResolver::Resolution r = resolver.resolve(channel, *l, src->id);
    if (r.channel == nullptr) {
        return std::nullopt;
    }
    return r.alias ? r.alias->quantity : r.channel->desc.quantity;
}

void TimeDistanceGraph::addChannel(const QString& name, bool newPanel) {
    const std::string channel = name.toStdString();
    if (channel.empty()) {
        return;
    }
    plot::TimeDistanceBody body = body_;
    int target = -1;
    if (!newPanel) {
        // Like-units grouping (04 section 8.2 item 3): the group's quantity is that of its first channel
        // that resolves in the Main source (unit family compare, D-010).
        if (const std::optional<units::QuantityId> q = quantityOf(channel)) {
            for (std::size_t g = 0; g < body.groups.size() && target < 0; ++g) {
                for (const plot::TdTrace& t : body.groups[g].traces) {
                    if (const std::optional<units::QuantityId> tq = quantityOf(t.channel)) {
                        if (*tq == *q) {
                            target = static_cast<int>(g);
                        }
                        break;
                    }
                }
            }
        }
    }
    if (target >= 0) {
        plot::TdGroup& g = body.groups[static_cast<std::size_t>(target)];
        const bool present = std::any_of(g.traces.begin(), g.traces.end(),
                                         [&](const plot::TdTrace& t) { return t.channel == channel; });
        if (present) {
            return;
        }
        plot::TdTrace trace;
        trace.channel = channel;
        g.traces.push_back(std::move(trace));
    } else {
        // A new group at the bottom named after the channel. Explicit edges are compressed so the new panel
        // takes an equal share; groups that all keep the default [0, 1] share the height automatically.
        const bool allDefault =
            std::all_of(body.groups.begin(), body.groups.end(),
                        [](const plot::TdGroup& g) { return g.edgeTop == 0.0 && g.edgeBottom == 1.0; });
        const auto n = static_cast<double>(body.groups.size());
        plot::TdGroup g;
        g.name = channel;
        plot::TdTrace trace;
        trace.channel = channel;
        g.traces.push_back(std::move(trace));
        if (!allDefault && n > 0.0) {
            const double scale = n / (n + 1.0);
            for (plot::TdGroup& existing : body.groups) {
                existing.edgeTop *= scale;
                existing.edgeBottom *= scale;
            }
            g.edgeTop = scale;
            g.edgeBottom = 1.0;
        }
        body.groups.push_back(std::move(g));
    }
    requestBody(body, tr("Add %1").arg(name));
}

void TimeDistanceGraph::setGroupScale(int group, plot::ScaleMode mode, std::optional<double> min,
                                      std::optional<double> max, const QString& unit) {
    if (group < 0 || static_cast<std::size_t>(group) >= body_.groups.size()) {
        return;
    }
    plot::TimeDistanceBody body = body_;
    plot::TdGroup& g = body.groups[static_cast<std::size_t>(group)];
    g.scaleMode = mode;
    if (mode == plot::ScaleMode::Manual) {
        if (min) {
            g.scaleMin = min;
        }
        if (max) {
            g.scaleMax = max;
        }
        if (min || max) {
            g.scaleUnit = unit.toStdString();
        }
    }
    requestBody(body, mode == plot::ScaleMode::Manual ? tr("Manual scale") : tr("Channel scale"));
}

void TimeDistanceGraph::setActiveChannel(const QString& name) {
    const std::string channel = name.toStdString();
    if (channel == spec_.activeChannel) {
        return;
    }
    project::ComponentSpec next = spec_;
    next.activeChannel = channel;
    Q_EMIT specEditRequested(next, tr("Active channel %1").arg(name));
}

// ---- layers ---------------------------------------------------------------------------------------------

bool TimeDistanceGraph::staticUpToDate() const noexcept {
    return prepared_ && !staticDirty_ && !buildScheduled_ && !buildInFlight_ &&
           !layers_.staticImage().isNull();
}

void TimeDistanceGraph::scheduleStaticBuild() {
    if (buildScheduled_) {
        return;
    }
    buildScheduled_ = true;
    // One build per event-loop turn: the WorksheetViewModel's Selection bit, the selection signal, a burst
    // of channelReady and a resize coalesce into one request (RenderService then keeps one running and one
    // pending build per component).
    QMetaObject::invokeMethod(this, &TimeDistanceGraph::requestStaticBuild, Qt::QueuedConnection);
}

void TimeDistanceGraph::prepare() {
    const qreal dpr = devicePixelRatioF();
    layers_.resize(size(), dpr);
    const QSize device = layers_.deviceSize();
    const plot::LayoutContext layout{device.width(), device.height(), static_cast<float>(dpr)};
    inputs_ = context_.render.prepareInputs(spec_, context_.view.state(), context_.selection.snapshot(),
                                            layout, scheme_, metrics_);
    inputs_.focused = isFocusedComponent();
    layout_ = plot::TimeDistanceBuilder::layoutOf(inputs_);
    prepared_ = true;
}

void TimeDistanceGraph::requestStaticBuild() {
    buildScheduled_ = false;
    if (!staticDirty_ || !isVisible() || size().isEmpty()) {
        return; // a hidden component builds when it is shown again
    }
    staticDirty_ = false;
    prepare();
    const std::uint64_t serial = ++submitSerial_;
    ++staticSubmitted_;
    buildInFlight_ = true;
    if (!pendingShown_) {
        pendingTimer_->start();
    }
    // 04 section 6.3 step 2 "worker build + raster": the worker also rasterises the Static list (in parallel
    // bands, RasterBackend::rasteriseStatic) into `raster`, which the GUI only stores
    // (LayerCache::storeStatic); the image travels beside the result and is read after the Dispatcher
    // delivered it (AC-M1-02).
    auto raster = std::make_shared<QImage>();
    context_.render.submitBuild(
        context_.worksheetId, componentId(), Priority::Visible,
        [in = inputs_, fonts = fonts_, raster](const CancelToken& cancel) {
            Result<plot::ComponentBuild> built = plot::TimeDistanceBuilder::buildStatic(in, cancel);
            if (built && fonts != nullptr && !cancel.checkpoint()) {
                *raster = qt::RasterBackend::rasteriseStatic(built->list, *fonts);
            }
            return built;
        },
        this,
        [this, serial, raster](Result<plot::ComponentBuild> result) {
            onStaticBuilt(serial, std::move(result), std::move(*raster));
        });
    // The inputs changed (slots, statuses, size): the GUI-built layers follow at once.
    rebuildDynamic();
    rebuildChrome();
    updateWorksheetHints();
}

void TimeDistanceGraph::onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result,
                                      QImage raster) {
    const bool latest = serial == submitSerial_;
    if (latest) {
        buildInFlight_ = false;
        pendingTimer_->stop();
    }
    if (result) {
        plot::ComponentBuild build = std::move(result).value();
        if (observer_) {
            observer_(build);
        }
        builtViewVersion_ = build.viewVersion;
        hits_ = std::move(build.hits);
        hitsDpr_ = build.list.dpr;
        // Shown even when superseded: an interim frame until the newer build lands (04 section 6.3 step 3).
        if (!raster.isNull() && raster.width() == build.list.widthPx &&
            raster.height() == build.list.heightPx) {
            layers_.storeStatic(std::move(raster)); // rasterised by the worker
        } else {
            layers_.rasteriseStatic(build.list);
        }
        ++staticDelivered_;
        update();
    }
    // A failed build keeps the previous image (04 section 1 item 4); a newer request may follow.
    if (latest) {
        setPendingShown(false);
    }
}

void TimeDistanceGraph::setPendingShown(bool shown) {
    if (pendingShown_ == shown) {
        return;
    }
    pendingShown_ = shown;
    rebuildChrome();
}

void TimeDistanceGraph::rebuildDynamic() {
    if (!prepared_) {
        return;
    }
    inputs_.view = context_.view.state();
    plot::DrawList next = plot::TimeDistanceBuilder::buildDynamic(inputs_);
    if (bandStart_ && layout_.traceArea.w > 0.0F) { // rubber band (Dynamic, 04 section 5.2)
        const double width = static_cast<double>(layout_.traceWidthPx);
        const double traceX = static_cast<double>(layout_.traceArea.x);
        const double x0 = plot::DomainMapper::pxOf(*bandStart_, inputs_.view.zoom, traceX, width);
        const double x1 = plot::DomainMapper::pxOf(bandEnd_, inputs_.view.zoom, traceX, width);
        const double lo = std::clamp(std::min(x0, x1), traceX, traceX + width);
        const double hi = std::clamp(std::max(x0, x1), traceX, traceX + width);
        const plot::Rgba colour = scheme_->role("cursorZoom", kBandFallback);
        next.filledRect(plot::RectPx{static_cast<float>(lo), layout_.traceArea.y,
                                     std::max(1.0F, static_cast<float>(hi - lo)), layout_.traceArea.h},
                        withAlpha(colour, 0x40));
    }
    ++dynamicBuilds_;
    lastDamage_ = layers_.dynamicDamage(layers_.dynamicList(), next);
    const bool waiting = next.text.find(kEllipsis) != std::string::npos;
    layers_.setDynamic(std::move(next));
    if (!lastDamage_.isEmpty()) {
        update(lastDamage_);
    }
    if (waiting && !retryTimer_->isActive()) {
        retryTimer_->start(); // a key value waits for its chunk (requested by Resampler::sampleAt)
    }
}

void TimeDistanceGraph::rebuildChrome() {
    if (!prepared_) {
        return;
    }
    inputs_.focused = isFocusedComponent();
    plot::DrawList next = plot::TimeDistanceBuilder::buildChrome(inputs_, pendingShown_);
    ++chromeBuilds_;
    const QRegion damage = layers_.dynamicDamage(layers_.chromeList(), next);
    layers_.setChrome(std::move(next));
    if (!damage.isEmpty()) {
        update(damage);
    }
}

const plot::ResolvedChannel* TimeDistanceGraph::activeMainChannel() const {
    if (inputs_.slots.empty()) {
        return nullptr;
    }
    const plot::SlotData& main = inputs_.slots.front();
    const plot::ResolvedChannel* first = nullptr;
    for (const plot::ResolvedChannel& rc : main.channels) {
        if (!rc.descriptor || !rc.series) {
            continue;
        }
        if (rc.name == spec_.activeChannel) {
            return &rc;
        }
        if (first == nullptr) {
            first = &rc;
        }
    }
    return first;
}

void TimeDistanceGraph::updateWorksheetHints() {
    if (!isFocusedComponent() || !prepared_) {
        return;
    }
    // Cursor steps and the zoom minimum follow the focused graph (04 section 8.5 item 2, 8.6).
    if (layout_.traceWidthPx > 0) {
        context_.view.setSize(layout_.traceWidthPx);
    }
    Micros period = 0;
    if (const plot::ResolvedChannel* rc = activeMainChannel()) {
        if (const auto* u = std::get_if<store::UniformSeries>(&*rc->series);
            u != nullptr && u->rate.valid()) {
            period = std::max<Micros>(1, u->rate.timeAt(1, 0));
        }
    }
    context_.view.setActiveSamplePeriod(period);
}

void TimeDistanceGraph::onFocusChanged(bool focused) {
    rebuildChrome(); // focus border (04 section 7.2 item 1)
    if (focused) {
        updateWorksheetHints();
    }
}

int TimeDistanceGraph::captionHeight() const {
    if (layout_.caption.h > 0.0F && layout_.dpr > 0.0F) {
        return static_cast<int>(std::lround(layout_.caption.h / layout_.dpr));
    }
    return ComponentWidget::captionHeight();
}

QImage TimeDistanceGraph::renderToImage(qreal dpr) const {
    const qreal current = layers_.devicePixelRatio();
    if (dpr <= 0.0 || std::abs(dpr - current) < 1e-9) {
        QImage img = layers_.composite();
        if (!img.isNull()) {
            return img; // the current Static image plus the Dynamic and Chrome lists (04 section 7.2 item 9)
        }
    }
    if (!prepared_) {
        return ComponentWidget::renderToImage(dpr);
    }
    // Another DPR: the Static image scaled (a rebuild would block), Dynamic and Chrome built at that DPR.
    qt::LayerCache cache(fonts_);
    cache.resize(size(), dpr);
    const QSize device = cache.deviceSize();
    plot::ComponentBuildInputs in = inputs_;
    in.layout = plot::LayoutContext{device.width(), device.height(), static_cast<float>(dpr)};
    if (!layers_.staticImage().isNull()) {
        QImage scaled = layers_.staticImage().scaled(device, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        cache.storeStatic(std::move(scaled));
    }
    cache.setDynamic(plot::TimeDistanceBuilder::buildDynamic(in));
    cache.setChrome(plot::TimeDistanceBuilder::buildChrome(in, false));
    return cache.composite();
}

void TimeDistanceGraph::paintEvent(QPaintEvent* event) {
    lastPaint_ = event->region();
    QPainter painter(this);
    // The component paints opaquely: what the Static image does not cover yet (nothing built, released, or
    // a resize ahead of its rebuild) gets the component background.
    QRegion uncovered = event->region();
    const QImage& image = layers_.staticImage();
    if (!image.isNull()) {
        const qreal dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
        uncovered -= QRect(QPoint(0, 0), (QSizeF(image.size()) / dpr).toSize());
    }
    if (!uncovered.isEmpty()) {
        const plot::Rgba bg = scheme_->role("cptBackground", 0xFFFFFFFFU);
        const QColor colour =
            QColor::fromRgb(static_cast<int>((bg >> 24) & 0xFFU), static_cast<int>((bg >> 16) & 0xFFU),
                            static_cast<int>((bg >> 8) & 0xFFU));
        for (const QRect& r : uncovered) {
            painter.fillRect(r, colour);
        }
    }
    layers_.paint(painter, event->region());
}

void TimeDistanceGraph::showEvent(QShowEvent* event) {
    ComponentWidget::showEvent(event);
    releaseTimer_->stop();
    if (staticDirty_ || layers_.staticImage().isNull()) {
        staticDirty_ = true;
        scheduleStaticBuild();
    }
}

void TimeDistanceGraph::hideEvent(QHideEvent* event) {
    ComponentWidget::hideEvent(event);
    releaseTimer_->start(); // 30 s, then the Static image is released if still hidden (D-058)
}

// ---- mouse (04 sections 8.5-8.6) ------------------------------------------------------------------------

std::optional<double> TimeDistanceGraph::domainXAt(double logicalX) const {
    if (!prepared_ || layout_.traceWidthPx <= 0) {
        return std::nullopt;
    }
    const double dpr = layout_.dpr > 0.0F ? static_cast<double>(layout_.dpr) : 1.0;
    const double x0 = static_cast<double>(layout_.traceArea.x);
    const auto width = static_cast<double>(layout_.traceWidthPx);
    const double px = std::clamp(logicalX * dpr, x0, x0 + width);
    return plot::DomainMapper::xOfPx(px, context_.view.state().zoom, x0, width);
}

double TimeDistanceGraph::logicalXOf(double domainX) const {
    const double dpr = layout_.dpr > 0.0F ? static_cast<double>(layout_.dpr) : 1.0;
    const double px = plot::DomainMapper::pxOf(domainX, context_.view.state().zoom,
                                               static_cast<double>(layout_.traceArea.x),
                                               static_cast<double>(layout_.traceWidthPx));
    return px / dpr;
}

std::optional<double> TimeDistanceGraph::cursorLogicalX() const {
    if (!prepared_ || inputs_.slots.empty()) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs_.slots.front();
    const plot::ViewState& view = context_.view.state();
    const plot::DomainMapper map(view.domain, main.range, main.outing, main.chain, Rate{}, 0);
    if (view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return std::nullopt;
    }
    return logicalXOf(map.xOf(main.range.start + view.cursorT));
}

plot::HitResult TimeDistanceGraph::hitAt(QPointF logical) const {
    // The hit regions travel with the Static layer (04 section 5.5): what the user sees is what is hit.
    if (hits_.empty()) {
        return {};
    }
    const float dpr = hitsDpr_ > 0.0F ? hitsDpr_ : 1.0F;
    return hits_.hit(static_cast<float>(logical.x()) * dpr, static_cast<float>(logical.y()) * dpr);
}

void TimeDistanceGraph::setCursorAtLogicalX(double logicalX) {
    if (inputs_.slots.empty()) {
        return;
    }
    const std::optional<double> x = domainXAt(logicalX);
    if (!x) {
        return;
    }
    const plot::SlotData& main = inputs_.slots.front();
    const plot::ViewState& view = context_.view.state();
    const plot::DomainMapper map(view.domain, main.range, main.outing, main.chain, Rate{}, 0);
    if (view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return; // no x axis to place a cursor on (banner)
    }
    Micros t = map.timeAt(*x);
    // Snap to the nearest Main sample of the active channel (04 section 8.5 item 1).
    if (const plot::ResolvedChannel* rc = activeMainChannel()) {
        if (const auto* u = std::get_if<store::UniformSeries>(&*rc->series);
            u != nullptr && u->rate.valid()) {
            const SampleIdx n = store::seriesSize(*rc->series);
            if (n > 0) {
                SampleIdx i = t <= u->t0 ? 0 : u->rate.indexAt(t, u->t0);
                i = std::min(i, n - 1);
                const Micros before = u->rate.timeAt(i, u->t0);
                const Micros after = i + 1 < n ? u->rate.timeAt(i + 1, u->t0) : before;
                t = (after - t) < (t - before) ? after : before;
            }
        }
    }
    context_.view.setCursor(t - main.range.start, qt::CursorSource::Click);
}

void TimeDistanceGraph::startRubberBand(double domainX) {
    bandStart_ = domainX;
    bandEnd_ = domainX;
    // The band follows the pointer with no button held (double-click -> move -> click): those moves reach
    // mouseMoveEvent only with mouse tracking on (QApplication drops button-less moves otherwise).
    setMouseTracking(true);
    rebuildDynamic();
}

void TimeDistanceGraph::endRubberBand() {
    bandStart_.reset();
    setMouseTracking(false);
}

void TimeDistanceGraph::finishRubberBand(double logicalX) {
    const std::optional<double> end = domainXAt(logicalX);
    const double start = *bandStart_;
    endRubberBand();
    if (end && *end != start) {
        context_.view.zoomTo(plot::XSpan{std::min(start, *end), std::max(start, *end)});
    }
    rebuildDynamic(); // removes the band
}

bool TimeDistanceGraph::cancelTransient() {
    if (!bandStart_) {
        return false;
    }
    endRubberBand();
    rebuildDynamic(); // removes the band; the zoom is unchanged
    return true;
}

void TimeDistanceGraph::mousePressEvent(QMouseEvent* event) {
    ComponentWidget::mousePressEvent(event); // focusRequested
    event->accept();
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF p = event->position();
    if (bandStart_) {
        finishRubberBand(p.x());
        return;
    }
    const plot::HitResult hit = hitAt(p);
    switch (hit.kind) {
    case plot::HitKind::Panel:
    case plot::HitKind::Trace:
        drag_ = Drag::Cursor;
        setCursorAtLogicalX(p.x());
        break;
    case plot::HitKind::Axis:
        if (hit.group < 0) { // the x axis (a group's axis strip is its vertical axis)
            drag_ = Drag::Pan;
            dragStartX_ = p.x();
            dragStartZoom_ = context_.view.state().zoom;
        }
        break;
    case plot::HitKind::KeyLabel: {
        const std::vector<std::string> names = body_.channelNames();
        if (hit.trace >= 0 && static_cast<std::size_t>(hit.trace) < names.size()) {
            setActiveChannel(QString::fromStdString(names[static_cast<std::size_t>(hit.trace)]));
        }
        break;
    }
    default:
        break;
    }
}

void TimeDistanceGraph::mouseMoveEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (bandStart_) {
        if (const std::optional<double> x = domainXAt(p.x())) {
            bandEnd_ = *x;
            rebuildDynamic();
        }
        event->accept();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
        ComponentWidget::mouseMoveEvent(event);
        return;
    }
    if (drag_ == Drag::Cursor) {
        setCursorAtLogicalX(p.x()); // Dynamic only
    } else if (drag_ == Drag::Pan && layout_.traceWidthPx > 0) {
        const double dpr = layout_.dpr > 0.0F ? static_cast<double>(layout_.dpr) : 1.0;
        const double widthLogical = static_cast<double>(layout_.traceWidthPx) / dpr;
        const double d = -(p.x() - dragStartX_) / widthLogical * dragStartZoom_.length();
        context_.view.zoomTo(plot::XSpan{dragStartZoom_.a + d, dragStartZoom_.b + d});
    }
    event->accept();
}

void TimeDistanceGraph::mouseReleaseEvent(QMouseEvent* event) {
    drag_ = Drag::None;
    event->accept();
}

void TimeDistanceGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (event->button() == Qt::LeftButton && hitAt(p).kind == plot::HitKind::Panel) {
        // Rubber band: double-click -> move -> click (04 section 8.6).
        drag_ = Drag::None;
        if (const std::optional<double> x = domainXAt(p.x())) {
            startRubberBand(*x);
        }
        event->accept();
        return;
    }
    ComponentWidget::mouseDoubleClickEvent(event); // caption: maximise
}

void TimeDistanceGraph::wheelEvent(QWheelEvent* event) {
    const QPointF p = event->position();
    const int delta = event->angleDelta().y();
    if (delta == 0 || hitAt(p).kind != plot::HitKind::Panel) {
        ComponentWidget::wheelEvent(event);
        return;
    }
    // High-resolution wheels and touchpads send fractions of a notch (< 120): they add up to whole notches; a
    // change of direction starts again from zero.
    if ((delta > 0) != (wheelRemainder_ > 0) && wheelRemainder_ != 0) {
        wheelRemainder_ = 0;
    }
    wheelRemainder_ += delta;
    const int notches = wheelRemainder_ / 120;
    wheelRemainder_ -= notches * 120;
    event->accept();
    if (notches == 0) {
        return;
    }
    const std::optional<double> xp = domainXAt(p.x());
    if (!xp) {
        return;
    }
    // Horizontal zoom x0.8 / x1.25 per notch about the pointer (04 section 8.6).
    const double f = std::pow(kWheelZoomIn, notches);
    const plot::XSpan z = context_.view.state().zoom;
    context_.view.zoomTo(plot::XSpan{*xp - (*xp - z.a) * f, *xp + (z.b - *xp) * f});
    event->accept();
}

void TimeDistanceGraph::keyPressEvent(QKeyEvent* event) {
    // Without the shell's key router (the router resolves ESC to view.return_focus, which calls
    // cancelTransient first).
    if (event->key() == Qt::Key_Escape && cancelTransient()) {
        event->accept();
        return;
    }
    ComponentWidget::keyPressEvent(event);
}

} // namespace esses::widgets
