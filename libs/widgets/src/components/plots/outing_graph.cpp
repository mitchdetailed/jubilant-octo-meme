// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OutingGraph (04 section 9; D-196 (d), D-198): the layers through LayeredComponent and plot::OutingGeometry,
// the zoom rectangle's handles derived from OutingGeometry::zoomRect(), the cursor, the rubber band and the
// wheel on the worksheet zoom, the lap double-click, like-units grouping, the S and L toggles and the CSV
// export. (Moved from src/components/time_distance/ with its owner, the plots work.)

#include "plots_common.hpp"

#include <esses/model/source.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/outing_graph.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace esses::widgets {

namespace {

// Zoom, Offsets, the cursor and the datum only move the Dynamic layer (04 section 7.4: "Dynamic on
// Zoom/Cursor").
constexpr auto kOutingStatic =
    static_cast<plot::ChangeMask>(plot::Change::Size | plot::Change::Scale | plot::kFullRebuild);
constexpr plot::Rgba kBandFallback = 0x1A73E8FFU;

LayeredComponent::Builders outingBuilders() {
    LayeredComponent::Builders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return plot::OutingGeometry::buildStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        return plot::OutingGeometry::buildDynamic(in);
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return plot::OutingGeometry::buildChrome(in, pending);
    };
    return b;
}

plot::OutingBody parseBody(const JsonDoc& json) {
    Result<plot::OutingBody> body = plot::OutingBody::parse(json);
    return body ? std::move(*body) : plot::OutingBody{};
}

// The worksheet's Main mapper (alignment offsets excluded: the shared zoom does not fold them in).
plot::DomainMapper worksheetMainMapper(const plot::ComponentBuildInputs& in) {
    const plot::SlotData& main = in.slots.front();
    plot::RangeGeom range = main.range;
    range.offsetUs = 0;
    range.offsetM = 0.0;
    return {in.view.domain, range, main.outing, main.chain, Rate{}, 0};
}

struct Lap {
    Micros start = 0;
    Micros end = 0;
};

// The cells of the range band in their order (OutingGeometry's rule): the laps of the Main source's
// "Outings:Laps" group in the outing, else the distance chain's pieces, else the Main range.
std::vector<Lap> lapsOf(const plot::ComponentBuildInputs& in) {
    std::vector<Lap> laps;
    if (in.slots.empty()) {
        return laps;
    }
    const plot::SlotData& main = in.slots.front();
    const model::DataLayer* layer = main.source ? main.source->layer(main.layer) : nullptr;
    const model::RangeGroup* group = layer != nullptr ? layer->ranges.find("Outings:Laps") : nullptr;
    if (group != nullptr) {
        for (const model::Range& r : group->ranges) {
            if (r.end > main.outing.start && r.start < main.outing.end) {
                laps.push_back(Lap{r.start, r.end});
            }
        }
    }
    if (laps.empty() && main.chain != nullptr) {
        for (const plot::LapPiece& p : main.chain->pieces()) {
            laps.push_back(Lap{p.start, p.end});
        }
    }
    if (laps.empty()) {
        laps.push_back(Lap{main.range.start, main.range.end});
    }
    return laps;
}

} // namespace

OutingGraph::OutingGraph(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : LayeredComponent(spec, context, outingBuilders(), kOutingStatic, parent), body_(parseBody(spec.body)) {}

OutingGraph::~OutingGraph() = default;

ComponentWidget::OverlayPolicy OutingGraph::overlayPolicy() const {
    return body_.enableOverlays ? OverlayPolicy::AllOverlays : OverlayPolicy::MainOnly;
}

qt::CommandContext OutingGraph::commandContext() const {
    return qt::CommandContext::Graph;
}

void OutingGraph::onSpecApplied() {
    body_ = parseBody(spec_.body);
}

std::optional<QString> OutingGraph::activeChannel() const {
    if (!spec_.activeChannel.empty()) {
        return QString::fromStdString(spec_.activeChannel);
    }
    const std::vector<std::string> names = body_.channelNames();
    if (names.empty()) {
        return std::nullopt;
    }
    return QString::fromStdString(names.front());
}

QStringList OutingGraph::contextCommands() const {
    return {QStringLiteral("zoom.in"),
            QStringLiteral("zoom.out"),
            QStringLiteral("zoom.full"),
            QStringLiteral("zoom.default_range"),
            plots::kCycleStyle,
            plots::kLegend,
            QStringLiteral("component.export_data"),
            QStringLiteral("component.print_clipboard"),
            QStringLiteral("component.properties")};
}

// ---- edits

void OutingGraph::requestOutingBody(const plot::OutingBody& body, const QString& undoText) {
    requestBody(body.toJson(), undoText);
}

void OutingGraph::addChannel(const QString& name, bool newPanel) {
    const std::string channel = name.toStdString();
    if (channel.empty()) {
        return;
    }
    plot::OutingBody body = body_;
    std::vector<plot::TdGroup>& groups = body.graph.groups;
    int target = -1;
    if (!newPanel) {
        // Like-units grouping (04 section 8.2 item 3): a group's quantity is that of its first channel that
        // resolves in the Main source.
        if (const std::optional<units::QuantityId> q = plots::quantityOf(context_, channel)) {
            for (std::size_t g = 0; g < groups.size() && target < 0; ++g) {
                for (const plot::TdTrace& t : groups[g].traces) {
                    if (const std::optional<units::QuantityId> tq = plots::quantityOf(context_, t.channel)) {
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
        plot::TdGroup& g = groups[static_cast<std::size_t>(target)];
        if (std::any_of(g.traces.begin(), g.traces.end(),
                        [&](const plot::TdTrace& t) { return t.channel == channel; })) {
            return;
        }
        plot::TdTrace trace;
        trace.channel = channel;
        g.traces.push_back(std::move(trace));
    } else {
        // A new group at the bottom named after the channel; explicit edges are compressed so that the new
        // panel takes an equal share.
        const bool allDefault = std::all_of(groups.begin(), groups.end(), [](const plot::TdGroup& g) {
            return g.edgeTop == 0.0 && g.edgeBottom == 1.0;
        });
        const auto n = static_cast<double>(groups.size());
        plot::TdGroup g;
        g.name = channel;
        plot::TdTrace trace;
        trace.channel = channel;
        g.traces.push_back(std::move(trace));
        if (!allDefault && n > 0.0) {
            const double scale = n / (n + 1.0);
            for (plot::TdGroup& existing : groups) {
                existing.edgeTop *= scale;
                existing.edgeBottom *= scale;
            }
            g.edgeTop = scale;
            g.edgeBottom = 1.0;
        }
        groups.push_back(std::move(g));
    }
    requestOutingBody(body, tr("Add %1").arg(name));
}

void OutingGraph::toggleKey() {
    plot::OutingBody body = body_;
    body.graph.keyMode = body.graph.keyMode == "none" ? "labels" : "none";
    requestOutingBody(body, body.graph.keyMode == "none" ? tr("Hide key") : tr("Show key"));
}

void OutingGraph::cycleStyle() {
    plot::OutingBody body = body_;
    body.graph.traceStyle = body.graph.traceStyle == "points" ? "line" : "points";
    requestOutingBody(body, tr("Trace style %1").arg(QString::fromStdString(body.graph.traceStyle)));
}

bool OutingGraph::handleCommand(const QString& id, const QVariantList& args) {
    if (id == plots::kLegend) {
        toggleKey();
        return true;
    }
    // The programmatic forms of the mouse rows the shell offers here (D-200 (a); D-203 (w)): the wheel
    // [notches] zooms the worksheet about its cursor; the zoom rectangle [a, b] (domain units) sets it.
    if (id == QLatin1String("zoom.wheel")) {
        if (!prepared()) {
            return false;
        }
        const int notches = args.isEmpty() ? 1 : args.front().toInt();
        plots::zoomAboutCursor(*this, liveInputs(), std::pow(kWheelZoomIn, notches));
        return true;
    }
    if (id == QLatin1String("zoom.outing_rect")) {
        if (args.size() < 2) {
            return false;
        }
        const double a = args.at(0).toDouble();
        const double b = args.at(1).toDouble();
        if (!(b != a)) {
            return false;
        }
        setViewZoom(plot::XSpan{std::min(a, b), std::max(a, b)});
        return true;
    }
    if (id == plots::kCycleStyle) {
        cycleStyle();
        return true;
    }
    return ComponentWidget::handleCommand(id, args);
}

// ---- outing coordinates

const OutingGraph::Geometry& OutingGraph::geometry() const {
    const plot::ViewState view = viewState();
    if (geometry_.revision == inputsRevision() && geometry_.zoom == view.zoom &&
        geometry_.domain == view.domain) {
        return geometry_;
    }
    geometry_ = Geometry{inputsRevision(), view.zoom, view.domain, std::nullopt, plot::RectPx{}};
    if (!prepared() || inputs().slots.empty()) {
        return geometry_;
    }
    plot::ComponentBuildInputs in = liveInputs();
    geometry_.zoomRect = plot::OutingGeometry::zoomRect(in);
    const plot::DomainMapper map = worksheetMainMapper(in);
    if (in.view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return geometry_;
    }
    const plot::XSpan ext = map.outingExtent();
    if (!(ext.length() > 0.0)) {
        return geometry_;
    }
    in.view.zoom = ext; // the whole outing: zoomRect() is then the whole trace area
    const plot::RectPx r = plot::OutingGeometry::zoomRect(in);
    if (r.w > 0.0F) {
        geometry_.axis = Axis{static_cast<double>(r.x), static_cast<double>(r.w), ext.a, ext.length()};
    }
    return geometry_;
}

std::optional<OutingGraph::Axis> OutingGraph::axis() const {
    return geometry().axis;
}

std::optional<double> OutingGraph::worksheetXAt(double logicalX) const {
    const std::optional<Axis> a = axis();
    if (!a) {
        return std::nullopt;
    }
    const double dpr = static_cast<double>(inputs().layout.dpr > 0.0F ? inputs().layout.dpr : 1.0F);
    const double px = std::clamp(logicalX * dpr, a->x0, a->x0 + a->width);
    return a->origin + (px - a->x0) / a->width * a->length;
}

double OutingGraph::logicalXOfWorksheet(double x) const {
    const std::optional<Axis> a = axis();
    if (!a) {
        return std::nan("");
    }
    const double dpr = static_cast<double>(inputs().layout.dpr > 0.0F ? inputs().layout.dpr : 1.0F);
    return (a->x0 + (x - a->origin) / a->length * a->width) / dpr;
}

QRectF OutingGraph::zoomRectLogical() const {
    if (!prepared() || inputs().slots.empty()) {
        return {};
    }
    const plot::RectPx r = geometry().zoomRect;
    const double dpr = static_cast<double>(inputs().layout.dpr > 0.0F ? inputs().layout.dpr : 1.0F);
    return QRectF(static_cast<double>(r.x) / dpr, static_cast<double>(r.y) / dpr,
                  static_cast<double>(r.w) / dpr, static_cast<double>(r.h) / dpr);
}

plot::HitResult OutingGraph::hitAt(QPointF logical) const {
    // The zoom handles (D-196 (d): a Dynamic build carries no HitTester, so they come from zoomRect()).
    const QRectF z = zoomRectLogical();
    if (z.width() > 0.0 && z.height() > 0.0) {
        const double grab = kHandleLogical;
        const double x = logical.x();
        const double y = logical.y();
        plot::HitResult h;
        h.kind = plot::HitKind::ZoomSpan;
        const bool withinY = y >= z.top() - grab && y <= z.bottom() + grab;
        if (withinY && std::abs(x - z.left()) <= grab) {
            h.handleKind = plot::HandleKind::ZoomStart;
            return h;
        }
        if (withinY && std::abs(x - z.right()) <= grab) {
            h.handleKind = plot::HandleKind::ZoomEnd;
            return h;
        }
        if (x > z.left() && x < z.right() && std::abs(y - z.bottom()) <= grab) {
            h.handleKind = plot::HandleKind::ZoomPan;
            return h;
        }
    }
    return LayeredComponent::hitAt(logical);
}

bool OutingGraph::zoomToLap(int index) {
    if (!prepared() || inputs().slots.empty()) {
        return false;
    }
    const std::vector<Lap> laps = lapsOf(inputs());
    if (index < 0 || static_cast<std::size_t>(index) >= laps.size()) {
        return false;
    }
    const plot::DomainMapper map = worksheetMainMapper(liveInputs());
    if (viewState().domain == plot::Domain::Distance && !map.hasDistance()) {
        return false;
    }
    const Lap& lap = laps[static_cast<std::size_t>(index)];
    const double a = map.xOf(lap.start);
    const double b = map.xOf(lap.end);
    setViewZoom(plot::XSpan{std::min(a, b), std::max(a, b)});
    return true;
}

void OutingGraph::setCursorAtLogicalX(double logicalX) {
    const std::optional<double> x = worksheetXAt(logicalX);
    if (!x) {
        return;
    }
    const plot::ComponentBuildInputs in = liveInputs();
    const plot::DomainMapper map = worksheetMainMapper(in);
    setViewCursor(map.timeAt(*x) - in.slots.front().range.start, qt::CursorSource::Click);
}

// ---- rubber band (the worksheet zoom)

void OutingGraph::appendDynamic(plot::DrawList& list) const {
    if (!bandStart_) {
        return;
    }
    const plot::RectPx area = plot::OutingGeometry::zoomRect([&] {
        plot::ComponentBuildInputs in = liveInputs();
        in.view.zoom = worksheetMainMapper(in).outingExtent();
        return in;
    }());
    const double dpr = static_cast<double>(list.dpr > 0.0F ? list.dpr : 1.0F);
    const double x0 = logicalXOfWorksheet(std::min(*bandStart_, bandEnd_)) * dpr;
    const double x1 = logicalXOfWorksheet(std::max(*bandStart_, bandEnd_)) * dpr;
    if (!std::isfinite(x0) || !std::isfinite(x1) || !(area.h > 0.0F)) {
        return;
    }
    const plot::Rgba colour = scheme()->role("cursorZoom", kBandFallback);
    list.filledRect(
        plot::RectPx{static_cast<float>(x0), area.y, std::max(1.0F, static_cast<float>(x1 - x0)), area.h},
        (colour & 0xFFFFFF00U) | 0x40U);
}

bool OutingGraph::cancelTransient() {
    if (!bandStart_) {
        return false;
    }
    bandStart_.reset();
    setMouseTracking(false);
    rebuildDynamic();
    return true;
}

// ---- mouse

void OutingGraph::mousePressEvent(QMouseEvent* event) {
    ComponentWidget::mousePressEvent(event); // focusRequested
    event->accept();
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF p = event->position();
    if (bandStart_) {
        const std::optional<double> end = worksheetXAt(p.x());
        const double start = *bandStart_;
        bandStart_.reset();
        setMouseTracking(false);
        if (end && *end != start) {
            setViewZoom(plot::XSpan{std::min(start, *end), std::max(start, *end)});
        }
        rebuildDynamic(); // removes the band
        return;
    }
    const plot::HitResult hit = hitAt(p);
    switch (hit.kind) {
    case plot::HitKind::ZoomSpan:
        dragStartZoom_ = viewState().zoom;
        dragStartX_ = worksheetXAt(p.x()).value_or(0.0);
        drag_ = hit.handleKind == plot::HandleKind::ZoomStart ? Drag::ZoomStart
                : hit.handleKind == plot::HandleKind::ZoomEnd ? Drag::ZoomEnd
                                                              : Drag::ZoomPan;
        break;
    case plot::HitKind::Panel:
    case plot::HitKind::Trace:
        drag_ = Drag::Cursor;
        setCursorAtLogicalX(p.x());
        break;
    default:
        break;
    }
}

void OutingGraph::mouseMoveEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (bandStart_) {
        if (const std::optional<double> x = worksheetXAt(p.x())) {
            bandEnd_ = *x;
            rebuildDynamic();
        }
        event->accept();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton) || drag_ == Drag::None) {
        ComponentWidget::mouseMoveEvent(event);
        return;
    }
    event->accept();
    if (drag_ == Drag::Cursor) {
        setCursorAtLogicalX(p.x());
        return;
    }
    const std::optional<double> x = worksheetXAt(p.x());
    const std::optional<Axis> a = axis();
    if (!x || !a) {
        return;
    }
    // Never narrower than one device pixel of the outing.
    const double minSpan = a->length / std::max(1.0, a->width);
    const plot::XSpan z = dragStartZoom_;
    switch (drag_) {
    case Drag::ZoomStart:
        setViewZoom(plot::XSpan{std::min(*x, z.b - minSpan), z.b});
        break;
    case Drag::ZoomEnd:
        setViewZoom(plot::XSpan{z.a, std::max(*x, z.a + minSpan)});
        break;
    case Drag::ZoomPan: {
        const double d = *x - dragStartX_;
        setViewZoom(plot::XSpan{z.a + d, z.b + d}); // constant span; the view model clamps
        break;
    }
    default:
        break;
    }
}

void OutingGraph::mouseReleaseEvent(QMouseEvent* event) {
    drag_ = Drag::None;
    event->accept();
}

void OutingGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (event->button() == Qt::LeftButton) {
        const plot::HitResult hit = hitAt(p);
        if (hit.kind == plot::HitKind::Band && hit.band >= 0) {
            drag_ = Drag::None;
            zoomToLap(hit.band); // 04 section 9 item 5
            event->accept();
            return;
        }
        if (hit.kind == plot::HitKind::Panel || hit.kind == plot::HitKind::Trace) {
            drag_ = Drag::None;
            if (const std::optional<double> x = worksheetXAt(p.x())) {
                bandStart_ = *x;
                bandEnd_ = *x;
                setMouseTracking(true); // the band follows the pointer with no button held
                rebuildDynamic();
            }
            event->accept();
            return;
        }
    }
    ComponentWidget::mouseDoubleClickEvent(event); // caption: maximise
}

void OutingGraph::wheelEvent(QWheelEvent* event) {
    const QPointF p = event->position();
    const plot::HitResult hit = hitAt(p);
    if (hit.kind != plot::HitKind::Panel && hit.kind != plot::HitKind::Trace &&
        hit.kind != plot::HitKind::ZoomSpan) {
        ComponentWidget::wheelEvent(event);
        return;
    }
    event->accept();
    const int notches = plots::wheelNotches(wheelRemainder_, event->angleDelta().y());
    const std::optional<double> xp = worksheetXAt(p.x());
    if (notches == 0 || !xp) {
        return;
    }
    // The worksheet zoom x0.8 / x1.25 per notch about the pointer (the outing graph is not zoomable itself).
    const double f = std::pow(kWheelZoomIn, notches);
    const plot::XSpan z = viewState().zoom;
    setViewZoom(plot::XSpan{*xp - (*xp - z.a) * f, *xp + (z.b - *xp) * f});
}

void OutingGraph::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && cancelTransient()) {
        event->accept();
        return;
    }
    ComponentWidget::keyPressEvent(event);
}

// ---- export

ComponentWidget::ExportJob OutingGraph::exportJob(const ExportOptions& options) const {
    if (!prepared()) {
        return {};
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(liveInputs(), options.range);
    std::vector<std::size_t> channels;
    for (std::size_t i = 0; i < body_.channelNames().size(); ++i) {
        channels.push_back(i);
    }
    const plots::LegacyExportInfo info{context_.view.definition().name};
    return [in, channels, info](const CancelToken& cancel) {
        return plots::legacyCsv(in, channels, info, cancel);
    };
}

bool OutingGraph::exportData(ExportSink& sink, const ExportOptions& options) const {
    const ExportJob job = exportJob(options);
    if (!job) {
        return false;
    }
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"), job);
    return csv && sink.write(*csv);
}

} // namespace esses::widgets
