// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackPosition (04 section 14). Owner: src/components/reports_track/.

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/widgets/components/track_position.hpp>

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
#include <utility>

namespace esses::widgets {

namespace {

constexpr float kCaptionLogical = 18.0F; // plotcore's caption bar (geometry_common.hpp)
constexpr float kInsetLogical = 6.0F;    // plot::TrackGeometry's map inset

std::shared_ptr<const project::Scheme> schemeOrDefault(std::shared_ptr<const project::Scheme> scheme) {
    return scheme != nullptr ? std::move(scheme) : std::make_shared<const project::Scheme>();
}

plot::TrackPositionBody parseBody(const project::ComponentSpec& spec) {
    Result<plot::TrackPositionBody> body = plot::TrackPositionBody::parse(spec.body);
    return body ? std::move(body).value() : plot::TrackPositionBody{};
}

} // namespace

TrackPosition::TrackPosition(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : ComponentWidget(spec, context, parent), commitTimer_(new QTimer(this)),
      releaseTimer_(new QTimer(this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(false);
    setScheme(schemeOrDefault(context.scheme));
    body_ = parseBody(spec_);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(kCommitDelayMs);
    connect(commitTimer_, &QTimer::timeout, this, &TrackPosition::commitView);
    releaseTimer_->setSingleShot(true);
    releaseTimer_->setInterval(kReleaseHiddenMs);
    connect(releaseTimer_, &QTimer::timeout, this, [this] {
        if (!isVisible()) {
            layers_.releaseStatic();
            staticDirty_ = true;
        }
    });
}

TrackPosition::~TrackPosition() {
    context_.render.cancelPending(context_.worksheetId, componentId());
}

ComponentWidget::OverlayPolicy TrackPosition::overlayPolicy() const {
    return OverlayPolicy::AllOverlays;
}

qt::CommandContext TrackPosition::commandContext() const {
    return qt::CommandContext::Track;
}

QString TrackPosition::componentId() const {
    if (spec_.id.isNil()) {
        return QStringLiteral("anon-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
    }
    return QString::fromStdString(spec_.id.str());
}

void TrackPosition::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    scheme_ = std::move(scheme);
    ++schemeGeneration_;
    fonts_ = std::make_shared<const qt::SchemeFonts>(*scheme_, schemeGeneration_);
    metrics_ = qt::QtTextMetrics::create(*scheme_, schemeGeneration_);
    layers_.setFonts(fonts_);
}

// ---- component interface --------------------------------------------------------------------------------

void TrackPosition::applySpec(const project::ComponentSpec& spec) {
    if (spec == spec_) {
        return; // a local zoom/pan edit coming back from the canvas: already shown
    }
    commitTimer_->stop(); // the spec from outside (undo, Properties) wins over a gesture not committed yet
    ComponentWidget::applySpec(spec);
    body_ = parseBody(spec_);
    staticDirty_ = true;
    scheduleStaticBuild();
}

void TrackPosition::onViewChanged(const plot::ViewState& view, plot::ChangeMask what) {
    inputs_.view = view;
    // 04 section 7.4: Static on Data/Scheme/Size (and Selection, Offsets, Scale); Dynamic on Cursor, Datum,
    // Zoom and Domain.
    const plot::ChangeMask staticBits = plot::Change::Size | plot::Change::Scheme | plot::Change::Selection |
                                        plot::Change::Data | plot::Change::Offsets | plot::Change::Scale;
    if ((what & staticBits) != 0) {
        layers_.invalidate(static_cast<plot::ChangeMask>(what & staticBits));
        staticDirty_ = true;
        scheduleStaticBuild();
        return;
    }
    rebuildDynamic();
}

void TrackPosition::onSelectionChanged(const plot::SelectionSnapshot&) {
    onViewChanged(context_.view.state(), plot::mask(plot::Change::Selection));
}

void TrackPosition::onDataChanged(const model::SourceId& source, std::uint64_t) {
    for (const plot::Slot& slot : context_.selection.snapshot().visibleSlots()) {
        if (slot.range.src == source) {
            onViewChanged(context_.view.state(), plot::mask(plot::Change::Data));
            return;
        }
    }
}

void TrackPosition::onSchemeChanged(const project::Scheme& scheme) {
    setScheme(std::make_shared<const project::Scheme>(scheme));
    onViewChanged(context_.view.state(), plot::mask(plot::Change::Scheme));
}

QStringList TrackPosition::contextCommands() const {
    return {QStringLiteral("track.zoom_in"), QStringLiteral("track.zoom_out"), QStringLiteral("track.fit"),
            QStringLiteral("component.send_to_background"), QStringLiteral("component.properties")};
}

// ---- layers ---------------------------------------------------------------------------------------------

bool TrackPosition::staticUpToDate() const noexcept {
    return prepared_ && !staticDirty_ && !buildScheduled_ && !buildInFlight_ &&
           !layers_.staticImage().isNull();
}

void TrackPosition::scheduleStaticBuild() {
    if (buildScheduled_) {
        return;
    }
    buildScheduled_ = true;
    QMetaObject::invokeMethod(this, &TrackPosition::requestStaticBuild, Qt::QueuedConnection);
}

void TrackPosition::prepare() {
    if (qt::TrackMapService* maps = context_.render.trackMaps(); maps != mapsConnected_.data()) {
        if (!mapsConnected_.isNull()) {
            disconnect(mapsConnected_.data(), nullptr, this, nullptr);
        }
        mapsConnected_ = maps;
        if (maps != nullptr) {
            connect(maps, &qt::TrackMapService::mapChanged, this, [this](const QString&) {
                onViewChanged(context_.view.state(), plot::mask(plot::Change::Data));
            });
        }
    }
    const qreal dpr = devicePixelRatioF();
    layers_.resize(size(), dpr);
    const QSize device = layers_.deviceSize();
    const plot::LayoutContext layout{device.width(), device.height(), static_cast<float>(dpr)};
    inputs_ = context_.render.prepareInputs(spec_, context_.view.state(), context_.selection.snapshot(),
                                            layout, scheme_, metrics_);
    inputs_.focused = isFocusedComponent();
    prepared_ = true;
}

void TrackPosition::requestStaticBuild() {
    buildScheduled_ = false;
    if (!staticDirty_ || !isVisible() || size().isEmpty()) {
        return; // a hidden component builds when it is shown again
    }
    staticDirty_ = false;
    prepare();
    const std::uint64_t serial = ++submitSerial_;
    ++staticSubmitted_;
    buildInFlight_ = true;
    auto raster = std::make_shared<QImage>();
    context_.render.submitBuild(
        context_.worksheetId, componentId(), Priority::Visible,
        [in = inputs_, fonts = fonts_, raster](const CancelToken& cancel) {
            Result<plot::ComponentBuild> built = plot::TrackGeometry::buildStatic(in, cancel);
            if (built && fonts != nullptr && !cancel.checkpoint()) {
                *raster = qt::RasterBackend::rasteriseStatic(built->list, *fonts);
            }
            return built;
        },
        this,
        [this, serial, raster](Result<plot::ComponentBuild> result) {
            if (serial == submitSerial_) {
                buildInFlight_ = false;
            }
            if (!result) {
                return; // the previous image stays (04 section 1 item 4)
            }
            plot::ComponentBuild build = std::move(result).value();
            hits_ = std::move(build.hits);
            hitsDpr_ = build.list.dpr;
            if (!raster->isNull() && raster->width() == build.list.widthPx &&
                raster->height() == build.list.heightPx) {
                layers_.storeStatic(std::move(*raster));
            } else {
                layers_.rasteriseStatic(build.list);
            }
            ++staticDelivered_;
            update();
        });
    rebuildDynamic();
    rebuildChrome();
}

void TrackPosition::rebuildDynamic() {
    if (!prepared_) {
        return;
    }
    inputs_.view = context_.view.state();
    plot::DrawList next = plot::TrackGeometry::buildDynamic(inputs_);
    ++dynamicBuilds_;
    const QRegion damage = layers_.dynamicDamage(layers_.dynamicList(), next);
    layers_.setDynamic(std::move(next));
    if (!damage.isEmpty()) {
        update(damage);
    }
}

void TrackPosition::rebuildChrome() {
    if (!prepared_) {
        return;
    }
    inputs_.focused = isFocusedComponent();
    plot::DrawList next = plot::TrackGeometry::buildChrome(inputs_, false);
    const QRegion damage = layers_.dynamicDamage(layers_.chromeList(), next);
    layers_.setChrome(std::move(next));
    if (!damage.isEmpty()) {
        update(damage);
    }
}

void TrackPosition::onFocusChanged(bool) {
    rebuildChrome(); // focus border (04 section 7.2 item 1)
}

int TrackPosition::captionHeight() const {
    return spec_.caption.show ? static_cast<int>(kCaptionLogical) : 0;
}

QImage TrackPosition::renderToImage(qreal dpr) const {
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
    cache.setDynamic(plot::TrackGeometry::buildDynamic(in));
    cache.setChrome(plot::TrackGeometry::buildChrome(in, false));
    return cache.composite();
}

plot::HitResult TrackPosition::hitAt(QPointF logical) const {
    if (hits_.empty()) {
        return {};
    }
    const float dpr = hitsDpr_ > 0.0F ? hitsDpr_ : 1.0F;
    return hits_.hit(static_cast<float>(logical.x()) * dpr, static_cast<float>(logical.y()) * dpr);
}

void TrackPosition::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
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

void TrackPosition::showEvent(QShowEvent* event) {
    ComponentWidget::showEvent(event);
    releaseTimer_->stop();
    if (staticDirty_ || layers_.staticImage().isNull()) {
        staticDirty_ = true;
        scheduleStaticBuild();
    }
}

void TrackPosition::hideEvent(QHideEvent* event) {
    ComponentWidget::hideEvent(event);
    releaseTimer_->start();
}

// ---- the view gestures ----------------------------------------------------------------------------------

plot::RectPx TrackPosition::mapAreaPx() const {
    const qreal dpr = devicePixelRatioF();
    const auto snapPx = [](double v) { return static_cast<float>(std::round(v)); };
    const float caption = spec_.caption.show ? snapPx(static_cast<double>(kCaptionLogical) * dpr) : 0.0F;
    const float inset = snapPx(static_cast<double>(kInsetLogical) * dpr);
    const auto w = static_cast<float>(std::lround(width() * dpr));
    const auto h = static_cast<float>(std::lround(height() * dpr));
    const float top = std::min(h, caption) + inset;
    return plot::RectPx{inset, top, std::max(0.0F, w - 2.0F * inset), std::max(0.0F, h - top - inset)};
}

bool TrackPosition::interactive() const {
    return isFocusedComponent() || hasFocus();
}

bool TrackPosition::inMapArea(QPointF logical) const {
    return logical.y() >= captionHeight();
}

void TrackPosition::setLocalBody(const plot::TrackPositionBody& body) {
    body_ = body;
    spec_.body = body_.toJson();
    layers_.invalidate(plot::mask(plot::Change::Scale));
    staticDirty_ = true;
    scheduleStaticBuild();
    commitTimer_->start();
}

void TrackPosition::zoomBy(double factor, std::optional<QPointF> about) {
    const std::shared_ptr<const analysis::TrackMap> map = inputs_.trackMap;
    if (map == nullptr || !(factor > 0.0)) {
        return;
    }
    plot::TrackPositionBody b = body_;
    if (b.scaleMode != "manual") {
        b.zoom = 1.0;
        b.panX = 0.0;
        b.panY = 0.0;
    }
    const plot::RectPx area = mapAreaPx();
    b.scaleMode = "manual";
    const plot::TrackFit before = plot::TrackGeometry::fit(*map, area, b);
    b.zoom = std::clamp(b.zoom * factor, kMinZoom, kMaxZoom);
    if (about) {
        // Keep the map point under the pointer where it is (04 section 14 item 4).
        const double dpr = devicePixelRatioF();
        const double px = about->x() * dpr;
        const double py = about->y() * dpr;
        const double mx = (px - before.offsetX) / before.scale;
        const double my = (before.offsetY - py) / before.scale;
        const plot::TrackFit after = plot::TrackGeometry::fit(*map, area, b);
        if (after.scale > 0.0) {
            b.panX += (px - after.scale * mx - after.offsetX) / after.scale;
            b.panY += (after.offsetY - py - after.scale * my) / after.scale;
        }
    }
    setLocalBody(b);
}

void TrackPosition::panByLogical(QPointF delta) {
    const std::shared_ptr<const analysis::TrackMap> map = inputs_.trackMap;
    if (map == nullptr) {
        return;
    }
    plot::TrackPositionBody b = body_;
    if (b.scaleMode != "manual") {
        b.scaleMode = "manual";
        b.zoom = 1.0;
        b.panX = 0.0;
        b.panY = 0.0;
    }
    const plot::TrackFit f = plot::TrackGeometry::fit(*map, mapAreaPx(), b);
    if (!(f.scale > 0.0)) {
        return;
    }
    const double dpr = devicePixelRatioF();
    b.panX += delta.x() * dpr / f.scale; // metres, right
    b.panY -= delta.y() * dpr / f.scale; // metres, up
    setLocalBody(b);
}

void TrackPosition::fit() {
    plot::TrackPositionBody b = body_;
    if (b.scaleMode == "fit" && b.zoom == 1.0 && b.panX == 0.0 && b.panY == 0.0) {
        return;
    }
    b.scaleMode = "fit";
    b.zoom = 1.0;
    b.panX = 0.0;
    b.panY = 0.0;
    setLocalBody(b);
    commitView(); // a fit is one step, not a gesture
}

void TrackPosition::commitView() {
    commitTimer_->stop();
    Q_EMIT specEditRequested(spec_, tr("Zoom track view"));
}

bool TrackPosition::handleCommand(const QString& id, const QVariantList& args) {
    // The shell offers the Track rows to the focused component (06 section 14.4;
    // ComponentWidget::handleCommand).
    if (id == QLatin1String("track.zoom_in")) {
        zoomBy(kZoomStep);
        return true;
    }
    if (id == QLatin1String("track.zoom_out")) {
        zoomBy(1.0 / kZoomStep);
        return true;
    }
    if (id == QLatin1String("track.fit")) {
        fit();
        return true;
    }
    return ComponentWidget::handleCommand(id, args);
}

// ---- keys and mouse -------------------------------------------------------------------------------------

void TrackPosition::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers mods = event->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    if (mods == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomBy(kZoomStep);
            event->accept();
            return;
        case Qt::Key_Minus:
            zoomBy(1.0 / kZoomStep);
            event->accept();
            return;
        default:
            break;
        }
    }
    ComponentWidget::keyPressEvent(event);
}

void TrackPosition::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y();
    if (!interactive() || steps == 0 || !inMapArea(event->position())) {
        event->ignore();
        return;
    }
    zoomBy(steps > 0 ? kZoomStep : 1.0 / kZoomStep, event->position());
    event->accept();
}

void TrackPosition::mousePressEvent(QMouseEvent* event) {
    if (body_.background) {
        event->ignore(); // a backdrop: the click is not consumed (04 section 14 item 4)
        return;
    }
    ComponentWidget::mousePressEvent(event); // focusRequested
    if (event->button() == Qt::LeftButton && inMapArea(event->position()) && interactive()) {
        dragFrom_ = event->position();
    }
    event->accept();
}

void TrackPosition::mouseMoveEvent(QMouseEvent* event) {
    if (!dragFrom_ || (event->buttons() & Qt::LeftButton) == 0) {
        ComponentWidget::mouseMoveEvent(event);
        return;
    }
    const QPointF delta = event->position() - *dragFrom_;
    dragFrom_ = event->position();
    panByLogical(delta);
    event->accept();
}

void TrackPosition::mouseReleaseEvent(QMouseEvent* event) {
    if (dragFrom_ && event->button() == Qt::LeftButton) {
        dragFrom_.reset();
        event->accept();
        return;
    }
    ComponentWidget::mouseReleaseEvent(event);
}

void TrackPosition::mouseDoubleClickEvent(QMouseEvent* event) {
    if (body_.background) {
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton && inMapArea(event->position())) {
        fit(); // 04 section 14 item 4
        event->accept();
        return;
    }
    ComponentWidget::mouseDoubleClickEvent(event); // the caption maximises
}

} // namespace esses::widgets
