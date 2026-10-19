// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayeredComponent (04 sections 6.2-6.3 and 7; D-198): the layer pipeline of the M3 drawn components. The
// same steps as TimeDistanceGraph's (worker build and raster, Dynamic and Chrome on the GUI thread, the
// pending stripe after 100 ms, the 30 s release of a hidden component), with the builders and the Static
// change bits supplied by the subclass.

#include <esses/base/casefold.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <QHideEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace esses::widgets {

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // the builders' "..." for a value still pending

std::shared_ptr<const project::Scheme> schemeOrDefault(std::shared_ptr<const project::Scheme> scheme) {
    return scheme != nullptr ? std::move(scheme) : std::make_shared<const project::Scheme>();
}

QColor colourOf(plot::Rgba c) {
    return QColor::fromRgb(static_cast<int>((c >> 24) & 0xFFU), static_cast<int>((c >> 16) & 0xFFU),
                           static_cast<int>((c >> 8) & 0xFFU));
}

} // namespace

LayeredComponent::LayeredComponent(const project::ComponentSpec& spec, WorksheetContext& context,
                                   Builders builders, plot::ChangeMask staticMask, QWidget* parent)
    : ComponentWidget(spec, context, parent), builders_(std::move(builders)), staticMask_(staticMask),
      pendingTimer_(new QTimer(this)), retryTimer_(new QTimer(this)), releaseTimer_(new QTimer(this)) {
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
            releaseInputs();
        }
    });
}

// The build inputs hold the sources' snapshots (DataSource, columns, chunk-cache entries, the file mapping):
// a hidden component lets them go when the selection or the data changes (a closed log must not stay alive
// through a worksheet visited earlier, D-203 (j)); showing it prepares them again.
void LayeredComponent::releaseInputs() {
    const plot::ViewState view = inputs_.view;
    inputs_ = plot::ComponentBuildInputs{};
    inputs_.view = view;
    prepared_ = false;
    ++inputsRevision_;
}

LayeredComponent::~LayeredComponent() {
    context_.render.cancelPending(context_.worksheetId, componentId());
}

QString LayeredComponent::componentId() const {
    if (spec_.id.isNil()) { // never in a loaded workbook (ids are derived); keep keys apart anyway
        return QStringLiteral("anon-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
    }
    return QString::fromStdString(spec_.id.str());
}

void LayeredComponent::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    scheme_ = std::move(scheme);
    ++schemeGeneration_;
    fonts_ = std::make_shared<const qt::SchemeFonts>(*scheme_, schemeGeneration_);
    metrics_ = qt::QtTextMetrics::create(*scheme_, schemeGeneration_);
    layers_.setFonts(fonts_);
}

// ---- hooks (defaults)

void LayeredComponent::adjustInputs(plot::ComponentBuildInputs&) const {}
void LayeredComponent::onStaticDelivered(const plot::ComponentBuild&) {}
void LayeredComponent::appendDynamic(plot::DrawList&) const {}
void LayeredComponent::onSpecApplied() {}

bool LayeredComponent::showsSource(const model::SourceId& source) const {
    for (const plot::Slot& slot : context_.selection.snapshot().visibleSlots()) {
        if (slot.range.src == source) {
            return true;
        }
    }
    return false;
}

// ---- component interface

void LayeredComponent::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    onSpecApplied();
    onViewChanged(viewState(), plot::kFullRebuild);
}

void LayeredComponent::onViewChanged(const plot::ViewState& view, plot::ChangeMask what) {
    inputs_.view = view;
    layers_.invalidate(what);
    if (!isVisible()) {
        // A component of a hidden (kept) worksheet does no work per cursor move or animation frame: what
        // changed is rebuilt when it is shown (D-203 (j)).
        if ((what & (plot::mask(plot::Change::Selection) | plot::mask(plot::Change::Data))) != 0) {
            releaseInputs();
        }
        if ((what & staticMask_) != 0) {
            staticDirty_ = true;
        }
        dynamicDirty_ = dynamicDirty_ || what != 0;
        return;
    }
    if ((what & staticMask_) != 0) {
        // Static (and Dynamic/Chrome with it) after the inputs are prepared again.
        staticDirty_ = true;
        scheduleStaticBuild();
        return;
    }
    if (what != 0) {
        rebuildDynamic(); // e.g. Cursor/Datum: Dynamic only, never a Static build (04 section 2.2)
    }
}

void LayeredComponent::onSelectionChanged(const plot::SelectionSnapshot&) {
    // The snapshot is read again when the build is prepared (the WorksheetViewModel's Selection bit, which
    // may arrive first, coalesces with this one).
    onViewChanged(viewState(), plot::mask(plot::Change::Selection));
}

void LayeredComponent::onDataChanged(const model::SourceId& source, std::uint64_t) {
    if (showsSource(source)) {
        onViewChanged(viewState(), plot::mask(plot::Change::Data));
    }
}

void LayeredComponent::onChannelDataChanged(const maths::ChannelKey& key) {
    // A maths output this component does not draw changes nothing on it (a log open readies dozens).
    for (const plot::ChannelUse& use : plot::componentChannelUses(spec_)) {
        if (caseFold(use.name) == key.nameCf) {
            onDataChanged(key.src, 0);
            return;
        }
    }
}

void LayeredComponent::onSchemeChanged(const project::Scheme& scheme) {
    setScheme(std::make_shared<const project::Scheme>(scheme));
    onViewChanged(viewState(), plot::mask(plot::Change::Scheme));
}

QStringList LayeredComponent::channelNames() const {
    QStringList names;
    for (const plot::ChannelUse& use : plot::componentChannelUses(spec_)) {
        const QString name = QString::fromStdString(use.name);
        if (!names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

void LayeredComponent::requestSpec(const project::ComponentSpec& next, const QString& undoText) {
    Q_EMIT specEditRequested(next, undoText);
}

void LayeredComponent::requestBody(const JsonDoc& body, const QString& undoText) {
    project::ComponentSpec next = spec_;
    next.body = body;
    requestSpec(next, undoText);
}

// ---- layers

bool LayeredComponent::staticUpToDate() const noexcept {
    return prepared_ && !staticDirty_ && !buildScheduled_ && !buildInFlight_ &&
           !layers_.staticImage().isNull();
}

void LayeredComponent::scheduleStaticBuild() {
    staticDirty_ = true;
    if (buildScheduled_) {
        return;
    }
    buildScheduled_ = true;
    // One build per event-loop turn: the WorksheetViewModel's Selection bit, the selection signal, a burst of
    // channelReady and a resize coalesce into one request (RenderService keeps one running and one pending
    // build per component).
    QMetaObject::invokeMethod(this, &LayeredComponent::requestStaticBuild, Qt::QueuedConnection);
}

void LayeredComponent::prepare() {
    const qreal dpr = devicePixelRatioF();
    layers_.resize(size(), dpr);
    const QSize device = layers_.deviceSize();
    const plot::LayoutContext layout{device.width(), device.height(), static_cast<float>(dpr)};
    inputs_ = context_.render.prepareInputs(spec_, viewState(), context_.selection.snapshot(), layout,
                                            scheme_, metrics_);
    inputs_.focused = isFocusedComponent();
    adjustInputs(inputs_);
    prepared_ = true;
    ++inputsRevision_;
}

plot::ComponentBuildInputs LayeredComponent::liveInputs() const {
    plot::ComponentBuildInputs in = inputs_;
    in.view = viewState();
    in.focused = isFocusedComponent();
    return in;
}

void LayeredComponent::requestStaticBuild() {
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
    // 04 section 6.3 step 2 "worker build + raster": the worker also rasterises the Static list into
    // `raster`, which the GUI only stores (LayerCache::storeStatic).
    auto raster = std::make_shared<QImage>();
    context_.render.submitBuild(
        context_.worksheetId, componentId(), Priority::Visible,
        [in = inputs_, build = builders_.buildStatic, fonts = fonts_, raster](const CancelToken& cancel) {
            Result<plot::ComponentBuild> built = build(in, cancel);
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
}

void LayeredComponent::onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result,
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
        onStaticDelivered(build);
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
        staticShown_ = static_cast<int>(serial); // serials count the submissions (staticSubmitted_)
        update();
    }
    // A failed build keeps the previous image (04 section 1 item 4); a newer request may follow.
    if (latest) {
        setPendingShown(false);
    }
}

void LayeredComponent::setPendingShown(bool shown) {
    if (pendingShown_ == shown) {
        return;
    }
    pendingShown_ = shown;
    rebuildChrome();
}

void LayeredComponent::rebuildDynamic() {
    if (!prepared_) {
        return;
    }
    dynamicDirty_ = false;
    inputs_.view = viewState();
    plot::DrawList next = builders_.buildDynamic ? builders_.buildDynamic(inputs_) : plot::DrawList{};
    next.widthPx = inputs_.layout.widthPx;
    next.heightPx = inputs_.layout.heightPx;
    next.dpr = inputs_.layout.dpr;
    appendDynamic(next);
    ++dynamicBuilds_;
    lastDamage_ = layers_.dynamicDamage(layers_.dynamicList(), next);
    const bool waiting = next.text.find(kEllipsis) != std::string::npos;
    layers_.setDynamic(std::move(next));
    if (!lastDamage_.isEmpty()) {
        update(lastDamage_);
    }
    if (waiting && !retryTimer_->isActive()) {
        retryTimer_->start(); // a value waits for its chunk (requested by Resampler::sampleAt)
    }
}

void LayeredComponent::rebuildChrome() {
    if (!prepared_) {
        return;
    }
    inputs_.focused = isFocusedComponent();
    plot::DrawList next =
        builders_.buildChrome ? builders_.buildChrome(inputs_, pendingShown_) : plot::DrawList{};
    ++chromeBuilds_;
    const QRegion damage = layers_.dynamicDamage(layers_.chromeList(), next);
    layers_.setChrome(std::move(next));
    if (!damage.isEmpty()) {
        update(damage);
    }
}

void LayeredComponent::onFocusChanged(bool) {
    rebuildChrome(); // focus border (04 section 7.2 item 1)
}

int LayeredComponent::captionHeight() const {
    return ComponentWidget::captionHeight();
}

QImage LayeredComponent::renderToImage(qreal dpr) const {
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
    plot::ComponentBuildInputs in = liveInputs();
    in.layout = plot::LayoutContext{device.width(), device.height(), static_cast<float>(dpr)};
    if (!layers_.staticImage().isNull()) {
        QImage scaled = layers_.staticImage().scaled(device, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        cache.storeStatic(std::move(scaled));
    }
    if (builders_.buildDynamic) {
        cache.setDynamic(builders_.buildDynamic(in));
    }
    if (builders_.buildChrome) {
        cache.setChrome(builders_.buildChrome(in, false));
    }
    return cache.composite();
}

void LayeredComponent::paintEvent(QPaintEvent* event) {
    lastPaint_ = event->region();
    QPainter painter(this);
    // The component paints opaquely: what the Static image does not cover yet (nothing built, released, or a
    // resize ahead of its rebuild) gets the component background.
    QRegion uncovered = event->region();
    const QImage& image = layers_.staticImage();
    if (!image.isNull()) {
        const qreal dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
        uncovered -= QRect(QPoint(0, 0), (QSizeF(image.size()) / dpr).toSize());
    }
    if (!uncovered.isEmpty()) {
        const QColor colour = colourOf(scheme_->role("cptBackground", 0xFFFFFFFFU));
        for (const QRect& r : uncovered) {
            painter.fillRect(r, colour);
        }
    }
    layers_.paint(painter, event->region());
}

void LayeredComponent::showEvent(QShowEvent* event) {
    ComponentWidget::showEvent(event);
    releaseTimer_->stop();
    if (staticDirty_ || !prepared_ || layers_.staticImage().isNull()) {
        scheduleStaticBuild(); // prepares the inputs and rebuilds Dynamic and Chrome with them
    } else if (dynamicDirty_) {
        rebuildDynamic();
    }
}

void LayeredComponent::hideEvent(QHideEvent* event) {
    ComponentWidget::hideEvent(event);
    releaseTimer_->start(); // 30 s, then the Static image is released if still hidden (D-058)
}

// ---- hits

QPointF LayeredComponent::devicePoint(QPointF logical) const {
    const auto dpr = static_cast<qreal>(hitsDpr());
    return logical * dpr;
}

plot::HitResult LayeredComponent::hitAt(QPointF logical) const {
    // The hit regions travel with the Static layer (04 section 5.5): what the user sees is what is hit.
    if (hits_.empty()) {
        return {};
    }
    const QPointF p = devicePoint(logical);
    return hits_.hit(static_cast<float>(p.x()), static_cast<float>(p.y()));
}

// M4 (D-211 (f)): an unlinked zoom, cursor or datum is mapped through the prepared inputs' Main slot.
std::optional<plot::DomainMapper> LayeredComponent::mainMapper() const {
    return mainMapperOf(inputs_, viewState().domain);
}

} // namespace esses::widgets
