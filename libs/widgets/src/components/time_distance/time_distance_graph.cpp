// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceGraph (04 sections 6.2-6.3 and 8; parity-first section 5.4; AC-M1-01, -02, -09; AC-M3-02, -05,
// -09): the layer pipeline, the mouse, the graph's commands, like-units grouping and the body edits.

#include "../plots/plots_common.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/uuid.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/store/resampler.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>

#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QHideEvent>
#include <QHoverEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <utility>
#include <variant>

namespace esses::widgets {

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // the builders' "..." for a value still pending
constexpr double kWheelZoomIn = 0.8;                   // 04 section 8.6: x0.8 / x1.25 per notch
constexpr plot::Rgba kBandFallback = 0x1A73E8FFU;
constexpr double kMinVerticalFraction = 1e-4; // the smallest shown fraction of a group's scale

plot::Rgba withAlpha(plot::Rgba rgba, std::uint8_t alpha) {
    return (rgba & 0xFFFFFF00U) | alpha;
}

std::shared_ptr<const project::Scheme> schemeOrDefault(std::shared_ptr<const project::Scheme> scheme) {
    return scheme != nullptr ? std::move(scheme) : std::make_shared<const project::Scheme>();
}

// The value of a channel at an absolute time as the key shows it: "" invalid, "..." not resident yet.
QString valueText(const plot::ResolvedChannel& rc, Micros t, int dps, double* out = nullptr) {
    if (!rc.series || rc.status == model::ChannelStatus::Pending) {
        return QString::fromUtf8(kEllipsis.data(), static_cast<qsizetype>(kEllipsis.size()));
    }
    const store::ValueSample v = store::Resampler::sampleAt(
        *rc.series, t, rc.descriptor ? rc.descriptor->interp : store::Interp::Linear);
    if (v.state == store::SampleState::Absent) {
        return QString::fromUtf8(kEllipsis.data(), static_cast<qsizetype>(kEllipsis.size()));
    }
    if (v.state != store::SampleState::Valid) {
        return {};
    }
    const double display = rc.toDisplay(v.value);
    if (out != nullptr) {
        *out = display;
    }
    return QString::number(display, 'f', std::clamp(dps, 0, 9));
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
    setMouseTracking(body_.keyStats);      // the measurement hover line needs button-less moves
    setAttribute(Qt::WA_Hover);            // HoverMove: the group headings' pointer (D-211 (e))
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
            releaseSources();
        }
    });
}

// A hidden graph lets its sources' snapshots go (D-203 (j)); the layout and the hits stay for the next show.
void TimeDistanceGraph::releaseSources() {
    inputs_.slots.clear();
    inputs_.trackMap.reset();
    prepared_ = false;
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

double TimeDistanceGraph::dprOrOne() const {
    return layout_.dpr > 0.0F ? static_cast<double>(layout_.dpr) : 1.0;
}

// ---- component interface --------------------------------------------------------------------------------

void TimeDistanceGraph::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    if (auto parsed = plot::TimeDistanceBody::parse(spec.body)) {
        body_ = std::move(parsed).value();
    } else {
        body_ = plot::TimeDistanceBody{};
    }
    // Vertical zoom is per group: a group that is gone loses it.
    std::erase_if(local_.vZoom, [this](const auto& z) {
        return z.first < 0 || static_cast<std::size_t>(z.first) >= body_.groups.size();
    });
    setMouseTracking(body_.keyStats || bandStart_.has_value());
    hoverLineX_.reset();
    onViewChanged(viewState(), plot::kFullRebuild);
}

void TimeDistanceGraph::onViewChanged(const plot::ViewState& view, plot::ChangeMask what) {
    // The datum appearing or disappearing changes the key columns (value@datum, delta): a Static rebuild.
    if (plot::has(what, plot::Change::Datum) && prepared_ && body_.keyValues &&
        view.datumT.has_value() != inputs_.view.datumT.has_value()) {
        what = what | plot::Change::Size;
    }
    inputs_.view = view;
    layers_.invalidate(what);
    if (!isVisible()) {
        // A graph of a hidden (kept) worksheet does no work per cursor move or animation frame (it would
        // read key values for nobody): what changed is rebuilt when it is shown (D-203 (j)).
        if ((what & (plot::mask(plot::Change::Selection) | plot::mask(plot::Change::Data))) != 0) {
            releaseSources();
        }
        if ((what & static_cast<plot::ChangeMask>(plot::kStaticRebuild | plot::kFullRebuild)) != 0) {
            staticDirty_ = true;
        }
        dynamicDirty_ = dynamicDirty_ || what != 0;
        return;
    }
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
    onViewChanged(viewState(), plot::mask(plot::Change::Selection));
}

void TimeDistanceGraph::onDataChanged(const model::SourceId& source, std::uint64_t) {
    if (showsSource(source)) {
        onViewChanged(viewState(), plot::mask(plot::Change::Data));
    }
}

void TimeDistanceGraph::onSchemeChanged(const project::Scheme& scheme) {
    setScheme(std::make_shared<const project::Scheme>(scheme));
    onViewChanged(viewState(), plot::mask(plot::Change::Scheme));
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
    return {QStringLiteral("zoom.in"),
            QStringLiteral("zoom.out"),
            QStringLiteral("zoom.full"),
            QStringLiteral("zoom.default_range"),
            QStringLiteral("pan.centre_cursor"),
            QStringLiteral("datum.place"),
            QStringLiteral("datum.toggle"),
            QStringLiteral("data.toggle_domain"),
            QStringLiteral("graph.variance"),
            QStringLiteral("graph.measurements"),
            QStringLiteral("graph.status_errors"),
            QStringLiteral("graph.device_events"),
            QStringLiteral("cursor.style"),
            QStringLiteral("component.cycle_layout"),
            QStringLiteral("component.cycle_style"),
            QStringLiteral("offset.axis_toggle"),
            QStringLiteral("cursor.add_annotation"),
            QStringLiteral("graph.copy_cursor_values"),
            QStringLiteral("component.export_data")};
}

bool TimeDistanceGraph::showsSource(const model::SourceId& source) const {
    for (const plot::Slot& slot : context_.selection.snapshot().visibleSlots()) {
        if (slot.range.src == source) {
            return true;
        }
    }
    return false;
}

// ---- commands (06 section 14.4: the graph rows; the shell offers them here first) -----------------------

bool TimeDistanceGraph::handleCommand(const QString& id, const QVariantList& args) {
    const auto arg = [&args](qsizetype i) { return i < args.size() ? args[i] : QVariant(); };
    if (id == QLatin1String("graph.variance")) {
        toggleVariance();
    } else if (id == QLatin1String("component.legend")) {
        toggleKey();
    } else if (id == QLatin1String("graph.measurements")) {
        toggleMeasurements();
    } else if (id == QLatin1String("graph.status_errors")) {
        toggleStatusStrip();
    } else if (id == QLatin1String("graph.device_events")) {
        toggleDeviceEvents();
    } else if (id == QLatin1String("cursor.style")) {
        cycleCursorStyle();
    } else if (id == QLatin1String("component.cycle_layout")) {
        cycleLayout();
    } else if (id == QLatin1String("component.cycle_style")) {
        cycleStyle();
    } else if (id == QLatin1String("graph.copy_cursor_values")) {
        copyCursorValues();
    } else if (id == QLatin1String("zoom.v_in")) {
        verticalZoom(kVerticalZoomStep);
    } else if (id == QLatin1String("zoom.v_out")) {
        verticalZoom(1.0 / kVerticalZoomStep);
    } else if (id == QLatin1String("zoom.v_full")) {
        verticalZoomFull();
    } else if (id == QLatin1String("pan.up")) {
        verticalPan(kVerticalPanStep);
    } else if (id == QLatin1String("pan.down")) {
        verticalPan(-kVerticalPanStep);
    } else if (id == QLatin1String("offset.drag")) {
        // [reference (bool), amount (domain units)]: the programmatic form of dragging a sub-axis.
        if (args.size() >= 2) {
            const bool reference = arg(0).toBool();
            const int slot = reference ? layout_.referenceSlot : 0;
            if (slot >= 0 && static_cast<std::size_t>(slot) < inputs_.slots.size()) {
                context_.view.beginOffsetDrag();
                context_.view.dragOffset(inputs_.slots[static_cast<std::size_t>(slot)].slot.range,
                                         arg(1).toDouble());
                context_.view.endOffsetDrag();
            }
        }
    } else if (id == QLatin1String("zoom.rubber_band")) {
        // The menu form: a horizontal band from the cursor; the next click in the trace area ends it.
        if (const std::optional<double> x = cursorLogicalX(); x && !bandStart_) {
            const double y =
                static_cast<double>(layout_.traceArea.y + 0.5F * layout_.traceArea.h) / dprOrOne();
            startRubberBand(QPointF(*x, y), BandAxis::Horizontal);
        }
    } else if (id == QLatin1String("zoom.full_axis")) {
        if (arg(0).toString() == QLatin1String("y")) {
            verticalZoomFull();
        } else {
            viewZoomFull();
        }
    } else if (id == QLatin1String("zoom.wheel")) {
        const int notches = args.isEmpty() ? 1 : arg(0).toInt();
        plots::zoomAboutCursor(*this, inputs_, std::pow(kWheelZoomIn, notches));
    } else if (id == QLatin1String("pan.drag_axis")) {
        viewPanBy(arg(0).toDouble());
    } else if (id == QLatin1String("cursor.place")) {
        if (!inputs_.slots.empty()) {
            const plot::SlotData& main = inputs_.slots.front();
            const plot::DomainMapper map(inputs_.view.domain, main.range, main.outing, main.chain, Rate{}, 0);
            const double x =
                args.isEmpty() ? map.xOf(main.range.start + viewState().cursorT) : arg(0).toDouble();
            setCursorAtDomainX(x, body_.cursorSnap);
        }
    } else {
        return ComponentWidget::handleCommand(id, args);
    }
    return true;
}

void TimeDistanceGraph::toggleVariance() {
    plot::TimeDistanceBody body = body_;
    body.variance.show = !body.variance.show;
    requestBody(body, body.variance.show ? tr("Show variance") : tr("Hide variance"));
}

// L: the key column (06 section 14.4), as the outing graph's: none <-> labels (a "values" key hides too).
void TimeDistanceGraph::toggleKey() {
    plot::TimeDistanceBody body = body_;
    body.keyMode = body.keyMode == "none" ? "labels" : "none";
    requestBody(body, body.keyMode == "none" ? tr("Hide key") : tr("Show key"));
}

void TimeDistanceGraph::toggleMeasurements() {
    plot::TimeDistanceBody body = body_;
    body.keyStats = !body.keyStats;
    requestBody(body, body.keyStats ? tr("Show measurements") : tr("Hide measurements"));
}

void TimeDistanceGraph::toggleStatusStrip() {
    plot::TimeDistanceBody body = body_;
    body.showStatusStrip = !body.showStatusStrip;
    requestBody(body, body.showStatusStrip ? tr("Show status strip") : tr("Hide status strip"));
}

void TimeDistanceGraph::toggleDeviceEvents() {
    plot::TimeDistanceBody body = body_;
    body.showDeviceEvents = !body.showDeviceEvents;
    requestBody(body, body.showDeviceEvents ? tr("Show device events") : tr("Hide device events"));
}

void TimeDistanceGraph::cycleCursorStyle() {
    plot::TimeDistanceBody body = body_;
    body.cursorStyle =
        body.cursorStyle == "line" ? "crosshair" : (body.cursorStyle == "crosshair" ? "none" : "line");
    requestBody(body, tr("Cursor style %1").arg(QString::fromStdString(body.cursorStyle)));
}

void TimeDistanceGraph::cycleLayout() {
    plot::TimeDistanceBody body = body_;
    body.overlapMode = body.overlapped() ? "panels" : "overlapped";
    requestBody(body, body.overlapped() ? tr("Overlapped") : tr("Panels"));
}

void TimeDistanceGraph::cycleStyle() {
    plot::TimeDistanceBody body = body_;
    body.traceStyle = body.traceStyle == "points" ? "line" : "points";
    requestBody(body, body.traceStyle == "points" ? tr("Points") : tr("Lines"));
}

QString TimeDistanceGraph::cursorValuesText() const {
    QStringList lines;
    const plot::ViewState view = viewState();
    if (inputs_.slots.empty()) {
        return {};
    }
    const plot::SlotData& main = inputs_.slots.front();
    const Micros tc = main.range.start + view.cursorT;
    const std::optional<Micros> td =
        view.datumT ? std::optional<Micros>(main.range.start + *view.datumT) : std::nullopt;
    std::size_t flat = 0;
    for (const plot::TdGroup& g : body_.groups) {
        for (const plot::TdTrace& t : g.traces) {
            const std::size_t index = flat++;
            const plot::ResolvedChannel* rc =
                index < main.channels.size() && main.channels[index].name == t.channel ? &main.channels[index]
                                                                                       : nullptr;
            QStringList cells{QString::fromStdString(t.channel)};
            QString unit;
            if (rc != nullptr && rc->descriptor) {
                const int dps = t.dps.value_or(rc->dps);
                unit = QString::fromStdString(rc->displayUnit);
                double vc = std::numeric_limits<double>::quiet_NaN();
                cells << valueText(*rc, tc, dps, &vc);
                if (td) {
                    double vd = std::numeric_limits<double>::quiet_NaN();
                    cells << valueText(*rc, *td, dps, &vd);
                    cells << (std::isfinite(vc) && std::isfinite(vd)
                                  ? QString::number(vc - vd, 'f', std::clamp(dps, 0, 9))
                                  : QString());
                }
            } else {
                cells << QString();
                if (td) {
                    cells << QString() << QString();
                }
            }
            cells << unit;
            lines << cells.join(QLatin1Char('\t'));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

void TimeDistanceGraph::copyCursorValues() {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(cursorValuesText());
    }
}

ComponentWidget::ExportJob TimeDistanceGraph::exportJob(const ExportOptions& options) const {
    if (!prepared_ || inputs_.slots.empty()) {
        return {};
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(inputs_, options.range);
    std::vector<std::size_t> channels;
    for (std::size_t i = 0; i < body_.channelNames().size(); ++i) {
        channels.push_back(i);
    }
    const plots::LegacyExportInfo info{context_.view.definition().name};
    return [in, channels, info](const CancelToken& cancel) {
        return plots::legacyCsv(in, channels, info, cancel);
    };
}

bool TimeDistanceGraph::exportData(ExportSink& sink, const ExportOptions& options) const {
    const ExportJob job = exportJob(options);
    if (!job) {
        return false;
    }
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"), job);
    return csv && sink.write(*csv);
}

// ---- vertical zoom (04 section 8.3 item 6) ----------------------------------------------------------------

int TimeDistanceGraph::activeGroup() const {
    if (layout_.activeGroup >= 0) {
        return layout_.activeGroup;
    }
    for (std::size_t g = 0; g < body_.groups.size(); ++g) {
        for (const plot::TdTrace& t : body_.groups[g].traces) {
            if (t.channel == spec_.activeChannel) {
                return static_cast<int>(g);
            }
        }
    }
    return body_.groups.empty() ? -1 : 0;
}

plot::TdGroupZoom TimeDistanceGraph::verticalZoomOf(int group) const {
    const auto it = local_.vZoom.find(group);
    return it != local_.vZoom.end() ? it->second : plot::TdGroupZoom{};
}

void TimeDistanceGraph::setVerticalZoom(int group, plot::TdGroupZoom zoom) {
    changeVerticalZoom(group, zoom);
}

void TimeDistanceGraph::changeVerticalZoom(int group, plot::TdGroupZoom zoom) {
    if (group < 0 || static_cast<std::size_t>(group) >= body_.groups.size()) {
        return;
    }
    double lo = std::min(zoom.lo, zoom.hi);
    double hi = std::max(zoom.lo, zoom.hi);
    double span = std::clamp(hi - lo, kMinVerticalFraction, 1.0);
    lo = std::clamp(lo, 0.0, 1.0 - span);
    hi = lo + span;
    const plot::TdGroupZoom next{lo, hi};
    const plot::TdGroupZoom current = verticalZoomOf(group);
    if (next == current) {
        return;
    }
    if (next == plot::TdGroupZoom{}) {
        local_.vZoom.erase(group);
    } else {
        local_.vZoom[group] = next;
    }
    onViewChanged(viewState(), plot::mask(plot::Change::Scale));
}

void TimeDistanceGraph::verticalZoom(double factor) {
    const int g = activeGroup();
    const plot::TdGroupZoom z = verticalZoomOf(g);
    // About the active channel's value at the cursor (06 section 14.4), when it is valid and on the shown
    // scale; else about the middle of the shown span (D-203 (v)).
    double centre = 0.5 * (z.lo + z.hi);
    if (const std::optional<double> f = cursorValueFraction(g)) {
        centre = z.lo + *f * (z.hi - z.lo);
    }
    const double half = 0.5 * std::min(1.0, (z.hi - z.lo) * factor);
    double lo = centre - half;
    double hi = centre + half;
    if (lo < 0.0) {
        hi -= lo;
        lo = 0.0;
    }
    if (hi > 1.0) {
        lo -= hi - 1.0;
        hi = 1.0;
    }
    changeVerticalZoom(g, plot::TdGroupZoom{std::max(0.0, lo), hi});
}

std::optional<double> TimeDistanceGraph::cursorValueFraction(int group) const {
    if (group < 0 || static_cast<std::size_t>(group) >= local_.builtScales.size() || inputs_.slots.empty() ||
        spec_.activeChannel.empty()) {
        return std::nullopt;
    }
    const std::optional<std::pair<double, double>>& scale =
        local_.builtScales[static_cast<std::size_t>(group)];
    if (!scale || !(scale->second > scale->first)) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs_.slots.front();
    std::size_t flat = 0;
    for (const plot::TdGroup& gr : body_.groups) {
        for (const plot::TdTrace& t : gr.traces) {
            const std::size_t index = flat++;
            if (t.channel != spec_.activeChannel || index >= main.channels.size() ||
                main.channels[index].name != t.channel) {
                continue;
            }
            double v = std::numeric_limits<double>::quiet_NaN();
            static_cast<void>(valueText(main.channels[index], main.range.start + viewState().cursorT,
                                        t.dps.value_or(main.channels[index].dps), &v));
            if (!std::isfinite(v)) {
                return std::nullopt;
            }
            const double f = (v - scale->first) / (scale->second - scale->first);
            return f >= 0.0 && f <= 1.0 ? std::optional<double>(f) : std::nullopt;
        }
    }
    return std::nullopt;
}

void TimeDistanceGraph::verticalZoomFull(int group) {
    changeVerticalZoom(group < 0 ? activeGroup() : group, plot::TdGroupZoom{});
}

void TimeDistanceGraph::verticalPan(double fractionOfSpan) {
    const int g = activeGroup();
    const plot::TdGroupZoom z = verticalZoomOf(g);
    const double shift = fractionOfSpan * (z.hi - z.lo);
    changeVerticalZoom(g, plot::TdGroupZoom{z.lo + shift, z.hi + shift});
}

// ---- offsets (04 section 3.3, AC-M3-09)
// --------------------------------------------------------------------

std::optional<double> TimeDistanceGraph::offsetStep(int slot) const {
    if (slot < 0 || static_cast<std::size_t>(slot) >= inputs_.slots.size()) {
        return std::nullopt;
    }
    if (viewState().domain == plot::Domain::Distance) {
        return analysis::DistanceIndex::kStepM;
    }
    const plot::SlotData& s = inputs_.slots[static_cast<std::size_t>(slot)];
    const plot::ResolvedChannel* first = nullptr;
    for (const plot::ResolvedChannel& rc : s.channels) {
        if (!rc.descriptor || !rc.series || !std::holds_alternative<store::UniformSeries>(*rc.series)) {
            continue;
        }
        if (rc.name == spec_.activeChannel) {
            first = &rc;
            break;
        }
        if (first == nullptr) {
            first = &rc;
        }
    }
    if (first == nullptr) {
        return std::nullopt;
    }
    const auto& u = std::get<store::UniformSeries>(*first->series);
    if (!u.rate.valid()) {
        return std::nullopt;
    }
    return static_cast<double>(std::max<Micros>(1, u.rate.timeAt(1, 0)));
}

// ---- annotations (04 section 8.11 item 4)
// ------------------------------------------------------------------

bool TimeDistanceGraph::addAnnotationAtCursor(const QString& text) {
    const plot::SelectionSnapshot& s = context_.selection.snapshot();
    const model::Range* main = context_.selection.hasMain() ? context_.selection.resolve(s.main) : nullptr;
    const auto src = main != nullptr ? context_.selection.source(s.main.src) : nullptr;
    if (src == nullptr || text.isEmpty()) {
        return false;
    }
    model::Annotation a;
    a.id = uuidRandom();
    a.tUs = main->start + viewState().cursorT; // the Main source's absolute time
    a.text = text.toStdString();
    if (!spec_.activeChannel.empty()) {
        a.channel = spec_.activeChannel;
    }
    model::SidecarDocument doc = src->sidecar;
    doc.setAnnotation(model::canonicalLayerName(s.main.layer), std::move(a));
    return context_.selection.commitSidecar(src->id, std::move(doc), tr("Add annotation"));
}

bool TimeDistanceGraph::editAnnotation(const model::MarkerId& id, const QString& text) {
    const plot::SelectionSnapshot& s = context_.selection.snapshot();
    const auto src = context_.selection.hasMain() ? context_.selection.source(s.main.src) : nullptr;
    const std::string_view layer = model::canonicalLayerName(s.main.layer);
    if (src == nullptr || text.isEmpty() || !src->sidecar.hasLayer(layer)) {
        return false;
    }
    for (const model::Annotation& a : src->sidecar.layer(layer).annotations) {
        if (a.id == id.v) {
            model::Annotation edited = a;
            edited.text = text.toStdString();
            model::SidecarDocument doc = src->sidecar;
            doc.setAnnotation(layer, std::move(edited));
            return context_.selection.commitSidecar(src->id, std::move(doc), tr("Edit annotation"));
        }
    }
    return false;
}

bool TimeDistanceGraph::removeAnnotation(const model::MarkerId& id) {
    const plot::SelectionSnapshot& s = context_.selection.snapshot();
    const auto src = context_.selection.hasMain() ? context_.selection.source(s.main.src) : nullptr;
    if (src == nullptr) {
        return false;
    }
    model::SidecarDocument doc = src->sidecar;
    if (!doc.removeAnnotation(model::canonicalLayerName(s.main.layer), id.v)) {
        return false;
    }
    return context_.selection.commitSidecar(src->id, std::move(doc), tr("Remove annotation"));
}

const plot::TdMarkerEntry* TimeDistanceGraph::markerOf(const model::MarkerId& id) const {
    for (const plot::TdMarkerEntry& m : info_.markers) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

void TimeDistanceGraph::showAnnotationMenu(const plot::HitResult& hit, QPoint globalPos) {
    const plot::TdMarkerEntry* entry = markerOf(hit.markerId);
    if (entry == nullptr) {
        return;
    }
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("esses.annotationMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const model::MarkerId id = hit.markerId;
    const QString current = QString::fromStdString(entry->text);
    QAction* edit = menu->addAction(tr("Edit Annotation..."));
    edit->setObjectName(QStringLiteral("editAnnotation"));
    connect(edit, &QAction::triggered, this, [this, id, current] {
        auto* dialog = new QInputDialog(this);
        dialog->setObjectName(QStringLiteral("esses.editAnnotationDialog"));
        dialog->setWindowTitle(tr("Edit Annotation"));
        dialog->setLabelText(tr("Annotation:"));
        dialog->setTextValue(current);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QInputDialog::textValueSelected, this,
                [this, id](const QString& value) { editAnnotation(id, value); });
        dialog->open();
    });
    QAction* remove = menu->addAction(tr("Remove Annotation"));
    remove->setObjectName(QStringLiteral("removeAnnotation"));
    connect(remove, &QAction::triggered, this, [this, id] { removeAnnotation(id); });
    menu->popup(globalPos);
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

// The project's channel override limits (Channel Editor scale min / max) in base units on every resolved
// channel (04 section 8.3 items 2 and 5).
void TimeDistanceGraph::applyOverrides(plot::ComponentBuildInputs& in) const {
    const qt::ProjectSession* session = context_.selection.session();
    if (session == nullptr) {
        return;
    }
    const project::ChannelOverrides& overrides = session->overrides();
    const units::UnitRegistry* registry = context_.maths.units().get();
    // A limit as written ({v, unit}) in the channel's stored unit; "" is taken as the stored unit.
    const auto toStored = [registry](const model::Quantityd& q,
                                     const model::ChannelDescriptor& d) -> std::optional<double> {
        if (q.symbol.empty() || registry == nullptr) {
            return q.v;
        }
        const std::optional<units::UnitId> id = registry->find(q.symbol);
        if (!id) {
            return std::nullopt;
        }
        const double base = registry->toBase(*id)(q.v);
        return registry->contains(d.unit) ? registry->fromBase(d.unit)(base) : base;
    };
    for (plot::SlotData& slot : in.slots) {
        for (plot::ResolvedChannel& rc : slot.channels) {
            const project::ChannelOverrides::Entry* e = overrides.find(rc.name);
            if (e == nullptr || !rc.descriptor) {
                continue;
            }
            if (e->scaleMin) {
                rc.limitMin = toStored(*e->scaleMin, *rc.descriptor);
            }
            if (e->scaleMax) {
                rc.limitMax = toStored(*e->scaleMax, *rc.descriptor);
            }
        }
    }
}

void TimeDistanceGraph::prepare() {
    const qreal dpr = devicePixelRatioF();
    layers_.resize(size(), dpr);
    const QSize device = layers_.deviceSize();
    const plot::LayoutContext layout{device.width(), device.height(), static_cast<float>(dpr)};
    inputs_ = context_.render.prepareInputs(spec_, viewState(), context_.selection.snapshot(), layout,
                                            scheme_, metrics_);
    // A drag or key burst in progress (04 section 3.3 item 4): the pending offsets move the traces now.
    const auto& pending = context_.view.pendingOffsets();
    for (plot::SlotData& slot : inputs_.slots) {
        if (const auto it = pending.find(slot.slot.range); it != pending.end()) {
            if (inputs_.view.domain == plot::Domain::Time) {
                slot.range.offsetUs = static_cast<Micros>(std::llround(it->second));
            } else {
                slot.range.offsetM = it->second;
            }
        }
    }
    applyOverrides(inputs_);
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
    // (LayerCache::storeStatic); the image and what the build placed travel beside the result and are read
    // after the Dispatcher delivered it (AC-M1-02).
    auto raster = std::make_shared<QImage>();
    auto info = std::make_shared<plot::TdStaticInfo>();
    plot::TdLocalState local = local_;
    local.builtScales.clear();
    context_.render.submitBuild(
        context_.worksheetId, componentId(), Priority::Visible,
        [in = inputs_, local, fonts = fonts_, raster, info](const CancelToken& cancel) {
            Result<plot::ComponentBuild> built =
                plot::TimeDistanceBuilder::buildStatic(in, local, cancel, info.get());
            if (built && fonts != nullptr && !cancel.checkpoint()) {
                *raster = qt::RasterBackend::rasteriseStatic(built->list, *fonts);
            }
            return built;
        },
        this,
        [this, serial, raster, info](Result<plot::ComponentBuild> result) {
            onStaticBuilt(serial, std::move(result), std::move(*raster), info);
        });
    // The inputs changed (slots, statuses, size): the GUI-built layers follow at once.
    rebuildDynamic();
    rebuildChrome();
    updateWorksheetHints();
}

void TimeDistanceGraph::onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result,
                                      QImage raster, std::shared_ptr<plot::TdStaticInfo> info) {
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
        if (info != nullptr) {
            info_ = std::move(*info);
            local_.builtScales = info_.scales; // the Dynamic crosshair follows the drawn axes
        }
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
        rebuildDynamic();
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
    dynamicDirty_ = false;
    inputs_.view = viewState();
    plot::DrawList next = plot::TimeDistanceBuilder::buildDynamic(inputs_, local_);
    const plot::Rgba zoomColour = scheme_->role("cursorZoom", kBandFallback);
    if (bandStart_ && layout_.traceArea.w > 0.0F) { // rubber band (Dynamic, 04 section 5.2)
        const double width = static_cast<double>(layout_.traceWidthPx);
        const double traceX = static_cast<double>(layout_.traceArea.x);
        double lo = traceX;
        double hi = traceX + width;
        if (bandAxis_ != BandAxis::Vertical) {
            const double x0 = plot::DomainMapper::pxOf(*bandStart_, inputs_.view.zoom, traceX, width);
            const double x1 = plot::DomainMapper::pxOf(bandEnd_, inputs_.view.zoom, traceX, width);
            lo = std::clamp(std::min(x0, x1), traceX, traceX + width);
            hi = std::clamp(std::max(x0, x1), traceX, traceX + width);
        }
        float top = layout_.traceArea.y;
        float h = layout_.traceArea.h;
        if (bandAxis_ != BandAxis::Horizontal) {
            top = std::min(bandStartY_, bandEndY_);
            h = std::max(1.0F, std::abs(bandEndY_ - bandStartY_));
        }
        next.filledRect(
            plot::RectPx{static_cast<float>(lo), top, std::max(1.0F, static_cast<float>(hi - lo)), h},
            withAlpha(zoomColour, 0x40));
    }
    const float lineW = std::max(1.0F, std::round(layout_.dpr));
    if (hoverLineX_ && layout_.traceArea.h > 0.0F) { // the hovered min/max position (04 section 8.4)
        const float x = std::round(*hoverLineX_) + 0.5F;
        const std::array<float, 4> seg{x, layout_.traceArea.y, x, layout_.traceArea.y + layout_.traceArea.h};
        next.lines(seg.data(), 2, scheme_->role("cptForeground2", 0x707070FFU), lineW, plot::DashId::Dash);
    }
    if (guideY_) { // a Lo/Hi handle or a splitter being dragged
        const float y = std::round(*guideY_) + 0.5F;
        const std::array<float, 4> seg{layout_.traceArea.x, y, layout_.traceArea.x + layout_.traceArea.w, y};
        next.lines(seg.data(), 2, zoomColour, lineW, plot::DashId::Dash);
    }
    if (groupDropY_) { // a group heading being dragged: where the group goes, across the key and the panels
        const float y = std::round(*groupDropY_) + 0.5F;
        const float x0 = std::min(layout_.keyColumn.x, layout_.traceArea.x);
        const float x1 =
            std::max(layout_.keyColumn.x + layout_.keyColumn.w, layout_.traceArea.x + layout_.traceArea.w);
        const std::array<float, 4> seg{x0, y, x1, y};
        next.lines(seg.data(), 2, zoomColour, 2.0F * lineW, plot::DashId::Dash);
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

const plot::TdGroupLayout* TimeDistanceGraph::groupLayout(int group) const {
    for (const plot::TdGroupLayout& gl : layout_.groups) {
        if (gl.group == group) {
            return &gl;
        }
    }
    return nullptr;
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
    // The offset step of every visible slot's source (04 section 3.3 item 3): its active channel's period.
    if (viewState().domain == plot::Domain::Time) {
        for (std::size_t k = 0; k < inputs_.slots.size(); ++k) {
            if (const std::optional<double> step = offsetStep(static_cast<int>(k))) {
                context_.view.setSourceSamplePeriod(inputs_.slots[k].slot.range.src,
                                                    static_cast<Micros>(*step));
            }
        }
    }
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
    cache.setDynamic(plot::TimeDistanceBuilder::buildDynamic(in, local_));
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
    if (staticDirty_ || !prepared_ || layers_.staticImage().isNull()) {
        staticDirty_ = true;
        scheduleStaticBuild();
    } else if (dynamicDirty_) {
        rebuildDynamic();
    }
}

void TimeDistanceGraph::hideEvent(QHideEvent* event) {
    ComponentWidget::hideEvent(event);
    releaseTimer_->start(); // 30 s, then the Static image is released if still hidden (D-058)
}

// ---- mouse (04 sections 8.4-8.11) ------------------------------------------------------------------------

std::optional<double> TimeDistanceGraph::domainXAt(double logicalX) const {
    if (!prepared_ || layout_.traceWidthPx <= 0) {
        return std::nullopt;
    }
    const double dpr = dprOrOne();
    const double x0 = static_cast<double>(layout_.traceArea.x);
    const auto width = static_cast<double>(layout_.traceWidthPx);
    const double px = std::clamp(logicalX * dpr, x0, x0 + width);
    return plot::DomainMapper::xOfPx(px, viewState().zoom, x0, width);
}

double TimeDistanceGraph::logicalXOf(double domainX) const {
    const double px =
        plot::DomainMapper::pxOf(domainX, viewState().zoom, static_cast<double>(layout_.traceArea.x),
                                 static_cast<double>(layout_.traceWidthPx));
    return px / dprOrOne();
}

std::optional<double> TimeDistanceGraph::cursorLogicalX() const {
    if (!prepared_ || inputs_.slots.empty()) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs_.slots.front();
    const plot::ViewState view = viewState();
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
    if (const std::optional<double> x = domainXAt(logicalX)) {
        setCursorAtDomainX(*x, body_.cursorSnap);
    }
}

void TimeDistanceGraph::setCursorAtDomainX(double x, bool snap) {
    if (inputs_.slots.empty()) {
        return;
    }
    const plot::SlotData& main = inputs_.slots.front();
    const plot::ViewState view = viewState();
    const plot::DomainMapper map(view.domain, main.range, main.outing, main.chain, Rate{}, 0);
    if (view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return; // no x axis to place a cursor on (banner)
    }
    Micros t = map.timeAt(x);
    // Snap to the nearest Main sample of the active channel (04 section 8.5 item 1).
    if (const plot::ResolvedChannel* rc = snap ? activeMainChannel() : nullptr) {
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
    setViewCursor(t - main.range.start, qt::CursorSource::Click);
}

void TimeDistanceGraph::startRubberBand(QPointF logical, BandAxis axis) {
    const std::optional<double> x = domainXAt(logical.x());
    if (!x) {
        return;
    }
    bandStart_ = *x;
    bandEnd_ = *x;
    bandAxis_ = axis;
    bandStartY_ = static_cast<float>(logical.y() * dprOrOne());
    bandEndY_ = bandStartY_;
    bandGroup_ = activeGroup();
    // The band follows the pointer with no button held (double-click -> move -> click): those moves reach
    // mouseMoveEvent only with mouse tracking on (QApplication drops button-less moves otherwise).
    setMouseTracking(true);
    rebuildDynamic();
}

void TimeDistanceGraph::endRubberBand() {
    bandStart_.reset();
    setMouseTracking(body_.keyStats);
}

void TimeDistanceGraph::finishRubberBand(QPointF logical) {
    const std::optional<double> end = domainXAt(logical.x());
    const double start = *bandStart_;
    const BandAxis axis = bandAxis_;
    const float y0 = bandStartY_;
    const float y1 = static_cast<float>(logical.y() * dprOrOne());
    const int group = bandGroup_;
    endRubberBand();
    if (axis != BandAxis::Vertical && end && *end != start) {
        setViewZoom(plot::XSpan{std::min(start, *end), std::max(start, *end)});
    }
    if (axis != BandAxis::Horizontal) {
        // Vertical: the band's share of the active group's panel becomes its shown fraction.
        if (const plot::TdGroupLayout* gl = groupLayout(group); gl != nullptr && gl->panel.h > 0.0F) {
            const auto frac = [gl](float y) {
                return std::clamp(static_cast<double>((gl->panel.y + gl->panel.h - y) / gl->panel.h), 0.0,
                                  1.0);
            };
            const double a = frac(std::max(y0, y1));
            const double b = frac(std::min(y0, y1));
            if (b > a) {
                const plot::TdGroupZoom z = verticalZoomOf(group);
                const double span = z.hi - z.lo;
                changeVerticalZoom(group, plot::TdGroupZoom{z.lo + a * span, z.lo + b * span});
            }
        }
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
    const QPointF p = event->position();
    if (event->button() == Qt::RightButton) {
        const plot::HitResult hit = hitAt(p);
        if (hit.kind == plot::HitKind::Annotation) {
            showAnnotationMenu(hit, event->globalPosition().toPoint());
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (bandStart_) {
        finishRubberBand(p);
        return;
    }
    const plot::HitResult hit = hitAt(p);
    dragHit_ = hit;
    dragStartX_ = p.x();
    dragStartY_ = p.y();
    switch (hit.kind) {
    case plot::HitKind::Panel:
    case plot::HitKind::Trace:
        drag_ = Drag::Cursor;
        setCursorAtLogicalX(p.x());
        break;
    case plot::HitKind::Axis:
        if (hit.group < 0) { // the x axis (a group's axis strip is its vertical axis)
            drag_ = Drag::Pan;
            dragStartZoom_ = viewState().zoom;
        } else {
            drag_ = Drag::VPan;
            dragStartVZoom_ = verticalZoomOf(hit.group);
        }
        break;
    case plot::HitKind::ScrollBar:
        if (hit.handleKind == plot::HandleKind::ScrollThumb) {
            drag_ = hit.group < 0 ? Drag::Thumb : Drag::VThumb;
            dragStartZoom_ = viewState().zoom;
            dragStartVZoom_ = verticalZoomOf(hit.group);
        } else if (hit.group < 0 && layout_.scrollbar.w > 0.0F) {
            // A click on the track pages toward it (04 section 8.6: F / B).
            const double px = p.x() * dprOrOne();
            const double thumbCentre = 0.5 * (viewState().zoom.a + viewState().zoom.b);
            const plot::XSpan extent = info_.scrollExtent;
            const double clicked = extent.a + (px - static_cast<double>(layout_.scrollbar.x)) /
                                                  static_cast<double>(layout_.scrollbar.w) * extent.length();
            viewPanBy(clicked < thumbCentre ? -0.9 : 0.9);
        } else if (hit.group >= 0) {
            if (const plot::TdGroupLayout* gl = groupLayout(hit.group);
                gl != nullptr && gl->vScroll.h > 0.0F) {
                const plot::TdGroupZoom z = verticalZoomOf(hit.group);
                const double frac = static_cast<double>(
                    (gl->vScroll.y + gl->vScroll.h - static_cast<float>(p.y() * dprOrOne())) / gl->vScroll.h);
                const double shift = (frac > 0.5 * (z.lo + z.hi) ? 0.9 : -0.9) * (z.hi - z.lo);
                changeVerticalZoom(hit.group, plot::TdGroupZoom{z.lo + shift, z.hi + shift});
            }
        }
        break;
    case plot::HitKind::OffsetAxis:
        drag_ = Drag::Offset;
        dragLastX_ = p.x();
        context_.view.beginOffsetDrag();
        break;
    case plot::HitKind::Handle:
        drag_ = Drag::Handle;
        guideY_ = static_cast<float>(p.y() * dprOrOne());
        rebuildDynamic();
        break;
    case plot::HitKind::KeyLabel: {
        const std::vector<std::string> names = body_.channelNames();
        if (hit.trace >= 0 && static_cast<std::size_t>(hit.trace) < names.size()) {
            setActiveChannel(QString::fromStdString(names[static_cast<std::size_t>(hit.trace)]));
        }
        break;
    }
    case plot::HitKind::GroupHeading: // a drag moves the group (04 section 8.2 item 4, D-211 (e))
        drag_ = Drag::Group;
        groupDropIndex_.reset();
        groupDropY_.reset();
        break;
    default: // bands, markers, annotations: a click never moves the cursor (04 section 8.11 item 1)
        break;
    }
}

// M4 (D-211 (e)): where a group heading dragged to `logicalY` would go. The groups' keys in their order down
// the key column; the dragged group is placed before the first other group whose key's middle lies below the
// pointer. Nothing shows until the pointer has moved 4 logical px.
void TimeDistanceGraph::updateGroupDrop(double logicalY) {
    const int from = dragHit_.group;
    std::optional<int> index;
    std::optional<float> lineY;
    if (std::abs(logicalY - dragStartY_) >= 4.0 && from >= 0) {
        const float y = static_cast<float>(logicalY * dprOrOne());
        std::vector<const plot::TdGroupLayout*> others;
        for (const plot::TdGroupLayout& gl : layout_.groups) {
            if (gl.group != from && gl.key.h > 0.0F) {
                others.push_back(&gl);
            }
        }
        std::sort(
            others.begin(), others.end(),
            [](const plot::TdGroupLayout* a, const plot::TdGroupLayout* b) { return a->key.y < b->key.y; });
        std::size_t k = 0;
        while (k < others.size() && others[k]->key.y + 0.5F * others[k]->key.h < y) {
            ++k;
        }
        index = static_cast<int>(k);
        lineY = k < others.size() ? others[k]->key.y
                : others.empty()  ? layout_.keyColumn.y
                                  : others.back()->key.y + others.back()->key.h;
    }
    if (index != groupDropIndex_ || lineY != groupDropY_) {
        groupDropIndex_ = index;
        groupDropY_ = lineY;
        rebuildDynamic();
    }
}

bool TimeDistanceGraph::moveGroup(int from, int to) {
    const int n = static_cast<int>(body_.groups.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return false;
    }
    plot::TimeDistanceBody body = body_;
    // Panels laid out by explicit edges keep their heights, stacked in the new order from the first top.
    const bool explicitEdges =
        !body.overlapped() && std::any_of(body.groups.begin(), body.groups.end(), [](const plot::TdGroup& g) {
            return g.edgeTop != 0.0 || g.edgeBottom != 1.0;
        });
    double top = 1.0;
    for (const plot::TdGroup& g : body.groups) {
        top = std::min(top, g.edgeTop);
    }
    const auto move = [from, to](auto& v) {
        auto item = std::move(v[static_cast<std::size_t>(from)]);
        v.erase(v.begin() + from);
        v.insert(v.begin() + to, std::move(item));
    };
    move(body.groups);
    if (explicitEdges) {
        for (plot::TdGroup& g : body.groups) {
            const double h = std::max(0.0, g.edgeBottom - g.edgeTop);
            g.edgeTop = top;
            g.edgeBottom = std::min(1.0, top + h);
            top = g.edgeBottom;
        }
    }
    // The group's unknown members (x-i2, ...) travel with it: toJson merges the typed groups over `rest`'s by
    // index.
    QJsonObject rest = QJsonDocument::fromJson(QByteArray::fromStdString(body.rest.compact())).object();
    if (QJsonArray groups = rest.value(QStringLiteral("groups")).toArray(); groups.size() == n) {
        const QJsonValue moved = groups.at(from);
        groups.removeAt(from);
        groups.insert(to, moved);
        rest.insert(QStringLiteral("groups"), groups);
        if (Result<model::OpaqueJson> doc =
                model::OpaqueJson::parse(QJsonDocument(rest).toJson(QJsonDocument::Compact).toStdString())) {
            body.rest = std::move(*doc);
        }
    }
    // The vertical zoom (local view state by group index) follows the groups.
    std::map<int, plot::TdGroupZoom> zoom;
    for (const auto& [g, z] : local_.vZoom) {
        int moved = g;
        if (g == from) {
            moved = to;
        } else if (from < to && g > from && g <= to) {
            moved = g - 1;
        } else if (to < from && g >= to && g < from) {
            moved = g + 1;
        }
        zoom[moved] = z;
    }
    local_.vZoom = std::move(zoom);
    local_.builtScales.clear();
    requestBody(
        body,
        tr("Move group %1").arg(QString::fromStdString(body.groups[static_cast<std::size_t>(to)].name)));
    return true;
}

PropertiesDialog* TimeDistanceGraph::openGroupProperties(int group) {
    if (group < 0 || static_cast<std::size_t>(group) >= body_.groups.size()) {
        return nullptr;
    }
    PropertiesDialog* dialog = PropertiesDialog::openFor(*this);
    if (dialog != nullptr) {
        dialog->focusRecord(QStringLiteral("/groups"), group);
    }
    return dialog;
}

// 04 section 8.12 / 7.2 item 3: a right-click on a channel label makes it active and shows the channel menu;
// an annotation has its own menu (mousePressEvent); elsewhere the component menu (D-203 (s)).
void TimeDistanceGraph::contextMenuEvent(QContextMenuEvent* event) {
    const plot::HitResult hit = hitAt(event->pos());
    if (hit.kind == plot::HitKind::Annotation) {
        event->accept();
        return;
    }
    if (hit.kind == plot::HitKind::KeyLabel) {
        const std::vector<std::string> names = body_.channelNames();
        if (hit.trace >= 0 && static_cast<std::size_t>(hit.trace) < names.size()) {
            setActiveChannel(QString::fromStdString(names[static_cast<std::size_t>(hit.trace)]));
        }
        Q_EMIT focusRequested();
        QMenu* menu =
            commandMenu({QStringLiteral("channel.scale_offset"), QStringLiteral("channel.zero_at_cursor"),
                         QStringLiteral("channel.filter"), QStringLiteral("channel.properties"),
                         QStringLiteral("channel.edit_maths"), QStringLiteral("channel.status"),
                         QStringLiteral("channel.hide"), QStringLiteral("channel.remove")});
        menu->setObjectName(QStringLiteral("channelContextMenu"));
        if (menu->isEmpty()) {
            delete menu;
        } else {
            menu->popup(event->globalPos());
        }
        event->accept();
        return;
    }
    ComponentWidget::contextMenuEvent(event);
}

void TimeDistanceGraph::mouseMoveEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (bandStart_) {
        if (const std::optional<double> x = domainXAt(p.x())) {
            bandEnd_ = *x;
            bandEndY_ = static_cast<float>(p.y() * dprOrOne());
            rebuildDynamic();
        }
        event->accept();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
        // Hover: the position line of a min/max measurement (04 section 8.4).
        std::optional<float> line;
        const plot::HitResult hit = hitAt(p);
        if (hit.kind == plot::HitKind::KeyMeasurement && (hit.cell == 0 || hit.cell == 1)) {
            for (const plot::TdMeasurementEntry& e : info_.measurements) {
                if (e.valid && e.trace == hit.trace && e.slot == hit.slot) {
                    const double x = hit.cell == 0 ? e.minX : e.maxX;
                    line = static_cast<float>(logicalXOf(x) * dprOrOne());
                }
            }
        }
        if (line != hoverLineX_) {
            hoverLineX_ = line;
            rebuildDynamic();
        }
        ComponentWidget::mouseMoveEvent(event);
        return;
    }
    const double dpr = dprOrOne();
    switch (drag_) {
    case Drag::Cursor:
        setCursorAtLogicalX(p.x()); // Dynamic only
        break;
    case Drag::Pan:
    case Drag::Thumb:
        if (layout_.traceWidthPx > 0) {
            const double widthLogical = static_cast<double>(layout_.traceWidthPx) / dpr;
            double d = 0.0;
            if (drag_ == Drag::Pan) {
                d = -(p.x() - dragStartX_) / widthLogical * dragStartZoom_.length();
            } else if (info_.scrollExtent.length() > 0.0 && layout_.scrollbar.w > 0.0F) {
                const double barLogical = static_cast<double>(layout_.scrollbar.w) / dpr;
                d = (p.x() - dragStartX_) / barLogical * info_.scrollExtent.length();
            }
            setViewZoom(plot::XSpan{dragStartZoom_.a + d, dragStartZoom_.b + d});
        }
        break;
    case Drag::VPan:
    case Drag::VThumb:
        if (const plot::TdGroupLayout* gl = groupLayout(dragHit_.group);
            gl != nullptr && gl->panel.h > 0.0F) {
            const double heightLogical = static_cast<double>(gl->panel.h) / dpr;
            const double span = dragStartVZoom_.hi - dragStartVZoom_.lo;
            // Dragging the axis moves the data with the pointer; dragging the thumb moves the shown window.
            const double moved = (p.y() - dragStartY_) / heightLogical;
            const double shift = drag_ == Drag::VPan ? moved * span : -moved;
            changeVerticalZoom(dragHit_.group,
                               plot::TdGroupZoom{dragStartVZoom_.lo + shift, dragStartVZoom_.hi + shift});
        }
        break;
    case Drag::Offset:
        if (layout_.traceWidthPx > 0 && dragHit_.slot >= 0 &&
            static_cast<std::size_t>(dragHit_.slot) < inputs_.slots.size()) {
            // The sub-axis follows the pointer: an offset of the dragged distance in domain units.
            const double widthLogical = static_cast<double>(layout_.traceWidthPx) / dpr;
            const double delta = (p.x() - dragLastX_) / widthLogical * viewState().zoom.length();
            dragLastX_ = p.x();
            context_.view.dragOffset(inputs_.slots[static_cast<std::size_t>(dragHit_.slot)].slot.range,
                                     delta);
        }
        break;
    case Drag::Handle:
        guideY_ =
            static_cast<float>(std::clamp(p.y() * dpr, static_cast<double>(layout_.traceArea.y),
                                          static_cast<double>(layout_.traceArea.y + layout_.traceArea.h)));
        rebuildDynamic();
        break;
    case Drag::Group:
        updateGroupDrop(p.y());
        break;
    case Drag::None:
        break;
    }
    event->accept();
}

void TimeDistanceGraph::mouseReleaseEvent(QMouseEvent* event) {
    const Drag ended = drag_;
    drag_ = Drag::None;
    event->accept();
    if (ended == Drag::Group) {
        const std::optional<int> to = groupDropIndex_;
        groupDropIndex_.reset();
        groupDropY_.reset();
        rebuildDynamic();
        if (to) {
            moveGroup(dragHit_.group, *to);
        }
    } else if (ended == Drag::Offset) {
        context_.view.endOffsetDrag(); // one undo command for the whole drag (AC-M3-09)
    } else if (ended == Drag::Handle && guideY_ && layout_.traceArea.h > 0.0F) {
        const double f =
            std::clamp(static_cast<double>((*guideY_ - layout_.traceArea.y) / layout_.traceArea.h), 0.0, 1.0);
        guideY_.reset();
        rebuildDynamic();
        const int g = dragHit_.group;
        if (g < 0 || static_cast<std::size_t>(g) >= body_.groups.size()) {
            return;
        }
        plot::TimeDistanceBody body = body_;
        plot::TdGroup& group = body.groups[static_cast<std::size_t>(g)];
        switch (dragHit_.handleKind) {
        case plot::HandleKind::GroupLo:
            group.lo = std::min(f, group.hi);
            requestBody(body, tr("Group region"));
            break;
        case plot::HandleKind::GroupHi:
            group.hi = std::max(f, group.lo);
            requestBody(body, tr("Group region"));
            break;
        case plot::HandleKind::PanelSplitter: {
            const int lowerIndex = dragHit_.handle;
            if (lowerIndex < 0 || static_cast<std::size_t>(lowerIndex) >= body.groups.size()) {
                return;
            }
            // Panels laid out by default share the height equally: the edges become explicit first.
            const std::size_t n = body.groups.size();
            const bool allDefault =
                std::all_of(body.groups.begin(), body.groups.end(), [](const plot::TdGroup& gr) {
                    return gr.edgeTop == 0.0 && gr.edgeBottom == 1.0;
                });
            if (allDefault && n > 1) {
                for (std::size_t i = 0; i < n; ++i) {
                    body.groups[i].edgeTop = static_cast<double>(i) / static_cast<double>(n);
                    body.groups[i].edgeBottom = static_cast<double>(i + 1) / static_cast<double>(n);
                }
            }
            plot::TdGroup& upper = body.groups[static_cast<std::size_t>(g)];
            plot::TdGroup& lower = body.groups[static_cast<std::size_t>(lowerIndex)];
            const double minEdge = upper.edgeTop + 0.02;
            const double maxEdge = lower.edgeBottom - 0.02;
            if (maxEdge > minEdge) {
                const double edge = std::clamp(f, minEdge, maxEdge);
                upper.edgeBottom = edge;
                lower.edgeTop = edge;
                requestBody(body, tr("Resize panels"));
            }
            break;
        }
        default:
            break;
        }
    }
}

void TimeDistanceGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (event->button() != Qt::LeftButton) {
        ComponentWidget::mouseDoubleClickEvent(event);
        return;
    }
    const plot::HitResult hit = hitAt(p);
    const auto zoomToEntry = [this](const std::vector<plot::TdBandEntry>& entries, int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < entries.size()) {
            const plot::TdBandEntry& e = entries[static_cast<std::size_t>(index)];
            if (e.x1 > e.x0) {
                setViewZoom(plot::XSpan{e.x0, e.x1});
            }
        }
    };
    switch (hit.kind) {
    case plot::HitKind::Panel: {
        // Rubber band: double-click -> move -> click (04 section 8.6); ALT vertical, CTRL a box.
        drag_ = Drag::None;
        const Qt::KeyboardModifiers mods = event->modifiers();
        const BandAxis axis = (mods & Qt::ControlModifier) != 0 ? BandAxis::Box
                              : (mods & Qt::AltModifier) != 0   ? BandAxis::Vertical
                                                                : BandAxis::Horizontal;
        startRubberBand(p, axis);
        event->accept();
        return;
    }
    case plot::HitKind::Band:
        zoomToEntry(info_.ranges, hit.band);
        event->accept();
        return;
    case plot::HitKind::Section:
        zoomToEntry(info_.sections, hit.section);
        event->accept();
        return;
    case plot::HitKind::ScrollBar:
        if (hit.group < 0) {
            viewZoomFull(); // full in that axis (04 section 8.6)
        } else {
            verticalZoomFull(hit.group);
        }
        event->accept();
        return;
    case plot::HitKind::GroupHeading: // Group Properties (04 section 8.2 item 4, D-211 (e))
        drag_ = Drag::None;
        openGroupProperties(hit.group);
        event->accept();
        return;
    case plot::HitKind::KeyMeasurement:
        if (hit.cell == 0 || hit.cell == 1) {
            for (const plot::TdMeasurementEntry& e : info_.measurements) {
                if (e.valid && e.trace == hit.trace && e.slot == hit.slot) {
                    setCursorAtDomainX(hit.cell == 0 ? e.minX : e.maxX, false);
                }
            }
        }
        event->accept();
        return;
    default:
        break;
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
    const plot::XSpan z = viewState().zoom;
    setViewZoom(plot::XSpan{*xp - (*xp - z.a) * f, *xp + (z.b - *xp) * f});
    event->accept();
}

void TimeDistanceGraph::leaveEvent(QEvent* event) {
    if (hoverLineX_) {
        hoverLineX_.reset();
        rebuildDynamic();
    }
    ComponentWidget::leaveEvent(event);
}

QString TimeDistanceGraph::toolTipAt(QPointF logical) const {
    const plot::HitResult hit = hitAt(logical);
    switch (hit.kind) {
    case plot::HitKind::Marker:
    case plot::HitKind::Annotation:
        if (const plot::TdMarkerEntry* m = markerOf(hit.markerId)) {
            return QString::fromStdString(m->text);
        }
        return {};
    case plot::HitKind::StatusRow: {
        // The channel and its value (a bit field as its set bits) at the pointer, in the Main slot.
        if (hit.trace < 0 || static_cast<std::size_t>(hit.trace) >= layout_.statusChannels.size() ||
            inputs_.slots.empty()) {
            return {};
        }
        const plot::TdStatusChannel& sc = layout_.statusChannels[static_cast<std::size_t>(hit.trace)];
        QString text = QString::fromStdString(sc.name);
        const std::optional<double> x = domainXAt(logical.x());
        if (x && sc.series) {
            const plot::SlotData& main = inputs_.slots.front();
            const plot::DomainMapper map(inputs_.view.domain, main.range, main.outing, main.chain, Rate{}, 0);
            const double v = store::Resampler::valueAt(*sc.series, map.timeAt(*x), store::Interp::Previous);
            if (std::isfinite(v)) {
                const auto bits = static_cast<std::uint32_t>(std::llround(v));
                if (sc.descriptor && sc.descriptor->kind == store::SampleKind::Bitfield) {
                    QStringList set;
                    for (int b = 0; b < 32; ++b) {
                        if ((bits >> b) & 1U) {
                            set << QString::number(b);
                        }
                    }
                    text += set.isEmpty() ? tr(": no bit set")
                                          : tr(": bit %1").arg(set.join(QStringLiteral(", ")));
                } else {
                    text += QStringLiteral(": %1").arg(bits);
                }
            }
        }
        return text;
    }
    case plot::HitKind::Band:
        if (hit.band >= 0 && static_cast<std::size_t>(hit.band) < info_.ranges.size()) {
            return QString::fromStdString(info_.ranges[static_cast<std::size_t>(hit.band)].label);
        }
        return {};
    case plot::HitKind::Section:
        if (hit.section >= 0 && static_cast<std::size_t>(hit.section) < info_.sections.size()) {
            return QString::fromStdString(info_.sections[static_cast<std::size_t>(hit.section)].label);
        }
        return {};
    case plot::HitKind::OffsetAxis:
        if (hit.slot >= 0 && static_cast<std::size_t>(hit.slot) < inputs_.slots.size()) {
            const plot::SlotData& s = inputs_.slots[static_cast<std::size_t>(hit.slot)];
            return QString::fromStdString(
                s.slot.range.name + " " +
                plot::TimeDistanceBuilder::offsetText(inputs_.view.domain, s.range));
        }
        return {};
    default:
        return {};
    }
}

bool TimeDistanceGraph::event(QEvent* event) {
    if (event->type() == QEvent::HoverMove && drag_ == Drag::None) {
        // A group heading can be dragged (D-211 (e)): an open hand over it.
        const plot::HitResult hit = hitAt(static_cast<const QHoverEvent*>(event)->position());
        if (hit.kind == plot::HitKind::GroupHeading) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
    }
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = toolTipAt(help->pos());
        if (text.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        return true;
    }
    return ComponentWidget::event(event);
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

// M4 (D-211 (f)): an unlinked zoom, cursor or datum is mapped through the prepared inputs' Main slot.
std::optional<plot::DomainMapper> TimeDistanceGraph::mainMapper() const {
    return mainMapperOf(inputs_, viewState().domain);
}

} // namespace esses::widgets
