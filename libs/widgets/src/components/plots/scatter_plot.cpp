// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterPlot (04 section 10; D-196, D-198): the layers through LayeredComponent and plot::ScatterGeometry,
// the cycles and toggles, the point pick, the axis zoom of the component's own axes, Copy Equation and the
// CSV export.

#include "plots_common.hpp"

#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/scatter_plot.hpp>

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <variant>

namespace esses::widgets {

namespace {

constexpr auto kScatterStatic = static_cast<plot::ChangeMask>(plot::kStaticRebuild | plot::kFullRebuild);
constexpr plot::Rgba kBandFallback = 0x1A73E8FFU;

LayeredComponent::Builders scatterBuilders() {
    LayeredComponent::Builders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return plot::ScatterGeometry::buildStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        return plot::ScatterGeometry::buildDynamic(in);
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return plot::ScatterGeometry::buildChrome(in, pending);
    };
    return b;
}

plot::ScatterBody parseBody(const JsonDoc& json) {
    Result<plot::ScatterBody> body = plot::ScatterBody::parse(json);
    return body ? std::move(*body) : plot::ScatterBody{};
}

template <std::size_t N>
std::string nextOf(const std::string& current, const std::array<const char*, N>& cycle) {
    for (std::size_t i = 0; i < N; ++i) {
        if (current == cycle[i]) {
            return cycle[(i + 1) % N];
        }
    }
    return cycle[0];
}

// A channel of Main by name (the units of the axes).
const plot::ResolvedChannel* mainChannel(const plot::ComponentBuildInputs& in, std::size_t index,
                                         const std::string& name) {
    if (in.slots.empty()) {
        return nullptr;
    }
    const plot::SlotData& main = in.slots.front();
    if (index < main.channels.size() && main.channels[index].name == name) {
        return &main.channels[index];
    }
    for (const plot::ResolvedChannel& rc : main.channels) {
        if (rc.name == name) {
            return &rc;
        }
    }
    return nullptr;
}

QByteArray scatterCsv(const plot::ComponentBuildInputs& in, const plot::ScatterBody& body,
                      const std::vector<plot::ScatterExportSet>& sets) {
    bool withZ = false;
    for (const plot::ScatterExportSet& s : sets) {
        withZ = withZ || !s.z.empty();
    }
    QByteArray out = withZ ? QByteArray("set,x,y,z\n") : QByteArray("set,x,y\n");
    for (const plot::ScatterExportSet& s : sets) {
        const QString trace =
            s.trace >= 0 && static_cast<std::size_t>(s.trace) < body.traces.size()
                ? QString::fromStdString(body.traces[static_cast<std::size_t>(s.trace)].yChannel)
                : QStringLiteral("Average");
        const QString slot = s.slot < in.slots.size() ? plots::slotLabel(in.slots[s.slot]) : QString();
        const QByteArray label = plots::csvText(trace + QLatin1Char('@') + slot);
        const std::size_t n = std::min(s.x.size(), s.y.size());
        for (std::size_t i = 0; i < n; ++i) {
            out += label;
            out += ',';
            out += plots::csvNumber(s.x[i]);
            out += ',';
            out += plots::csvNumber(s.y[i]);
            if (withZ) {
                out += ',';
                out += i < s.z.size() ? plots::csvNumber(s.z[i]) : QByteArray();
            }
            out += '\n';
        }
    }
    return out;
}

} // namespace

ScatterPlot::ScatterPlot(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : LayeredComponent(spec, context, scatterBuilders(), kScatterStatic, parent),
      body_(parseBody(spec.body)) {}

ScatterPlot::~ScatterPlot() = default;

ComponentWidget::OverlayPolicy ScatterPlot::overlayPolicy() const {
    return OverlayPolicy::AllOverlays;
}

qt::CommandContext ScatterPlot::commandContext() const {
    return qt::CommandContext::Summary;
}

void ScatterPlot::onSpecApplied() {
    body_ = parseBody(spec_.body);
}

std::optional<QString> ScatterPlot::activeChannel() const {
    if (!spec_.activeChannel.empty()) {
        return QString::fromStdString(spec_.activeChannel);
    }
    for (const plot::ScatterTrace& t : body_.traces) {
        if (t.visible && !t.yChannel.empty()) {
            return QString::fromStdString(t.yChannel);
        }
    }
    return std::nullopt;
}

QStringList ScatterPlot::contextCommands() const {
    return {plots::kCycleLayout,
            plots::kCycleStyle,
            plots::kCursorStyle,
            plots::kColourLegend,
            plots::kCopyEquation,
            QStringLiteral("component.export_data"),
            QStringLiteral("component.print_clipboard"),
            QStringLiteral("component.properties")};
}

// ---- edits

void ScatterPlot::requestScatterBody(const plot::ScatterBody& body, const QString& undoText) {
    requestBody(body.toJson(), undoText);
}

void ScatterPlot::addChannel(const QString& name, bool newPanel) {
    const std::string channel = name.toStdString();
    if (channel.empty()) {
        return;
    }
    plot::ScatterBody body = body_;
    if (body.xChannel.empty() || newPanel) {
        if (body.xChannel == channel) {
            return;
        }
        body.xChannel = channel;
        requestScatterBody(body, tr("X channel %1").arg(name));
        return;
    }
    const bool present = std::any_of(body.traces.begin(), body.traces.end(),
                                     [&](const plot::ScatterTrace& t) { return t.yChannel == channel; });
    if (present) {
        return;
    }
    plot::ScatterTrace trace;
    trace.yChannel = channel;
    body.traces.push_back(std::move(trace));
    requestScatterBody(body, tr("Add %1").arg(name));
}

void ScatterPlot::cycleLayout() {
    static constexpr std::array<const char*, 3> kCycle{"overlapped", "tiled", "average"};
    plot::ScatterBody body = body_;
    body.layout = nextOf(body.layout, kCycle);
    requestScatterBody(body, tr("Scatter layout %1").arg(QString::fromStdString(body.layout)));
}

void ScatterPlot::cycleStyle() {
    static constexpr std::array<const char*, 3> kCycle{"points", "pointsTrend", "trend"};
    plot::ScatterBody body = body_;
    body.style = nextOf(body.style, kCycle);
    requestScatterBody(body, tr("Scatter style %1").arg(QString::fromStdString(body.style)));
}

void ScatterPlot::cycleCursorStyle() {
    static constexpr std::array<const char*, 3> kCycle{"cross", "ring", "none"};
    plot::ScatterBody body = body_;
    body.cursorStyle = nextOf(body.cursorStyle, kCycle);
    requestScatterBody(body, tr("Cursor style %1").arg(QString::fromStdString(body.cursorStyle)));
}

void ScatterPlot::toggleColourLegend() {
    project::ComponentSpec next = spec_;
    next.legend.colour = !next.legend.colour;
    requestSpec(next, next.legend.colour ? tr("Show colour legend") : tr("Hide colour legend"));
}

void ScatterPlot::toggleLegend() {
    project::ComponentSpec next = spec_;
    next.legend.channels = !next.legend.channels;
    requestSpec(next, next.legend.channels ? tr("Show legend") : tr("Hide legend"));
}

bool ScatterPlot::handleCommand(const QString& id, const QVariantList& args) {
    if (id == plots::kCycleLayout) {
        cycleLayout();
        return true;
    }
    if (id == plots::kCycleStyle) {
        cycleStyle();
        return true;
    }
    if (id == plots::kCursorStyle) {
        cycleCursorStyle();
        return true;
    }
    if (id == plots::kColourLegend) {
        toggleColourLegend();
        return true;
    }
    if (id == plots::kLegend) {
        toggleLegend();
        return true;
    }
    if (id == plots::kCopyEquation) {
        return copyEquation();
    }
    if (id == plots::kCursorLeft || id == plots::kCursorRight || id == plots::kCursorLeftFast ||
        id == plots::kCursorRightFast) {
        const int sign = id == plots::kCursorLeft || id == plots::kCursorLeftFast ? -1 : 1;
        const int step = id == plots::kCursorLeftFast || id == plots::kCursorRightFast ? 10 : 1;
        return stepCursor(sign * step);
    }
    return ComponentWidget::handleCommand(id, args);
}

// ---- cursor

bool ScatterPlot::stepCursor(int points) {
    const plot::ComponentBuildInputs& in = inputs();
    const plot::ResolvedChannel* x = mainChannel(in, 0, body_.xChannel);
    if (x == nullptr || !x->series) {
        return false;
    }
    const auto* u = std::get_if<store::UniformSeries>(&*x->series);
    if (u == nullptr || !u->rate.valid()) {
        return false;
    }
    const plot::SlotData& main = in.slots.front();
    const Micros period = std::max<Micros>(1, u->rate.timeAt(1, 0));
    const Micros t = main.range.start + context_.view.state().cursorT;
    const auto index =
        static_cast<Micros>(std::llround(static_cast<double>(t - u->t0) / static_cast<double>(period)));
    const Micros next = u->t0 + (index + points) * period;
    context_.view.setCursor(next - main.range.start, qt::CursorSource::Keyboard);
    return true;
}

bool ScatterPlot::pickPoint(QPointF logical) {
    const QPointF p = devicePoint(logical);
    const float radius = static_cast<float>(kPickRadiusLogical) * hitsDpr();
    std::int64_t tag = 0;
    const plot::HitResult hit =
        hits().nearestPoint(static_cast<float>(p.x()), static_cast<float>(p.y()), radius, &tag);
    if (hit.kind == plot::HitKind::None) {
        return false;
    }
    context_.view.setCursor(tag, qt::CursorSource::Click); // the point's Main-lap time (D-196)
    return true;
}

// ---- equation

void ScatterPlot::onStaticDelivered(const plot::ComponentBuild& build) {
    equation_.clear();
    for (const plot::TextRun& run : build.list.texts) {
        if (run.style == plot::TextStyleId::Equation) {
            const std::string_view text = build.list.textOf(run);
            equation_ = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
            break;
        }
    }
}

bool ScatterPlot::copyEquation() const {
    if (equation_.isEmpty()) {
        return false;
    }
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(equation_);
        return true;
    }
    return false;
}

// ---- axis zoom

std::vector<plot::ScatterAxes> ScatterPlot::axes() const {
    if (!prepared()) {
        return {};
    }
    return plot::ScatterGeometry::axes(inputs());
}

void ScatterPlot::setAxisZoom(std::optional<AxisZoom> zoom) {
    if (zoom == axisZoom_) {
        return;
    }
    axisZoom_ = zoom;
    onViewChanged(context_.view.state(), plot::mask(plot::Change::Scale));
}

void ScatterPlot::adjustInputs(plot::ComponentBuildInputs& in) const {
    if (!axisZoom_) {
        return;
    }
    plot::ScatterBody body = parseBody(in.spec.body);
    const plot::ResolvedChannel* x = mainChannel(in, 0, body.xChannel);
    const plot::ResolvedChannel* y = nullptr;
    for (std::size_t t = 0; t < body.traces.size() && y == nullptr; ++t) {
        if (body.traces[t].visible && !body.traces[t].yChannel.empty()) {
            y = mainChannel(in, 1 + t, body.traces[t].yChannel);
        }
    }
    const std::string xUnit = x != nullptr ? x->displayUnit : std::string{};
    const std::string yUnit = y != nullptr ? y->displayUnit : std::string{};
    body.xScale = plot::ScaleSpec{plot::ScaleMode::Manual, analysis::UnitValue{axisZoom_->xMin, xUnit},
                                  analysis::UnitValue{axisZoom_->xMax, xUnit}};
    body.yScale = plot::ScaleSpec{plot::ScaleMode::Manual, analysis::UnitValue{axisZoom_->yMin, yUnit},
                                  analysis::UnitValue{axisZoom_->yMax, yUnit}};
    in.spec.body = body.toJson();
}

std::optional<ScatterPlot::DataPoint> ScatterPlot::dataAt(QPointF logical, int tile) const {
    const std::vector<plot::ScatterAxes> all = axes();
    const double dpr = inputs().layout.dpr > 0.0F ? static_cast<double>(inputs().layout.dpr) : 1.0;
    const double px = logical.x() * dpr;
    const double py = logical.y() * dpr;
    if (tile < 0) {
        for (std::size_t i = 0; i < all.size() && tile < 0; ++i) {
            const plot::RectPx& r = all[i].plot;
            if (px >= static_cast<double>(r.x) && px <= static_cast<double>(r.x + r.w) &&
                py >= static_cast<double>(r.y) && py <= static_cast<double>(r.y + r.h)) {
                tile = static_cast<int>(i);
            }
        }
    }
    if (tile < 0 || static_cast<std::size_t>(tile) >= all.size()) {
        return std::nullopt;
    }
    const plot::ScatterAxes& a = all[static_cast<std::size_t>(tile)];
    if (!(a.plot.w > 0.0F) || !(a.plot.h > 0.0F) || !std::isfinite(a.xMin) || !std::isfinite(a.xMax) ||
        !std::isfinite(a.yMin) || !std::isfinite(a.yMax)) {
        return std::nullopt;
    }
    const double fx =
        std::clamp((px - static_cast<double>(a.plot.x)) / static_cast<double>(a.plot.w), 0.0, 1.0);
    const double fy =
        std::clamp((py - static_cast<double>(a.plot.y)) / static_cast<double>(a.plot.h), 0.0, 1.0);
    return DataPoint{tile, a.xMin + fx * (a.xMax - a.xMin), a.yMax - fy * (a.yMax - a.yMin)};
}

void ScatterPlot::appendDynamic(plot::DrawList& list) const {
    if (!boxStart_) {
        return;
    }
    const std::vector<plot::ScatterAxes> all = axes();
    if (static_cast<std::size_t>(boxStart_->tile) >= all.size()) {
        return;
    }
    const plot::ScatterAxes& a = all[static_cast<std::size_t>(boxStart_->tile)];
    if (!(a.xMax > a.xMin) || !(a.yMax > a.yMin)) {
        return;
    }
    const auto xPx = [&](double v) {
        return a.plot.x + static_cast<float>((v - a.xMin) / (a.xMax - a.xMin)) * a.plot.w;
    };
    const auto yPx = [&](double v) {
        return a.plot.y + static_cast<float>((a.yMax - v) / (a.yMax - a.yMin)) * a.plot.h;
    };
    const float x0 = std::min(xPx(boxStart_->x), xPx(boxEnd_.x));
    const float x1 = std::max(xPx(boxStart_->x), xPx(boxEnd_.x));
    const float y0 = std::min(yPx(boxStart_->y), yPx(boxEnd_.y));
    const float y1 = std::max(yPx(boxStart_->y), yPx(boxEnd_.y));
    const plot::Rgba colour = scheme()->role("cursorZoom", kBandFallback);
    const plot::RectPx r{x0, y0, std::max(1.0F, x1 - x0), std::max(1.0F, y1 - y0)};
    list.filledRect(r, (colour & 0xFFFFFF00U) | 0x40U);
    list.rectOutline(r, (colour & 0xFFFFFF00U) | 0xC0U, std::max(1.0F, std::round(list.dpr)));
}

bool ScatterPlot::cancelTransient() {
    if (!boxStart_) {
        return false;
    }
    boxStart_.reset();
    setMouseTracking(false);
    rebuildDynamic();
    return true;
}

// ---- mouse

void ScatterPlot::mousePressEvent(QMouseEvent* event) {
    ComponentWidget::mousePressEvent(event); // focusRequested
    event->accept();
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF p = event->position();
    if (boxStart_) {
        const std::optional<DataPoint> end = dataAt(p, boxStart_->tile);
        const DataPoint start = *boxStart_;
        boxStart_.reset();
        setMouseTracking(false);
        if (end && end->x != start.x && end->y != start.y) {
            setAxisZoom(AxisZoom{std::min(start.x, end->x), std::max(start.x, end->x),
                                 std::min(start.y, end->y), std::max(start.y, end->y)});
        }
        rebuildDynamic(); // removes the box
        return;
    }
    if (hitAt(p).kind == plot::HitKind::Panel) {
        pickPoint(p);
    }
}

void ScatterPlot::mouseMoveEvent(QMouseEvent* event) {
    if (boxStart_) {
        if (const std::optional<DataPoint> d = dataAt(event->position(), boxStart_->tile)) {
            boxEnd_ = *d;
            rebuildDynamic();
        }
        event->accept();
        return;
    }
    ComponentWidget::mouseMoveEvent(event);
}

void ScatterPlot::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPointF p = event->position();
    if (event->button() == Qt::LeftButton) {
        const plot::HitResult hit = hitAt(p);
        if (hit.kind == plot::HitKind::Panel) {
            if (const std::optional<DataPoint> d = dataAt(p)) {
                boxStart_ = *d;
                boxEnd_ = *d;
                setMouseTracking(true); // the box follows the pointer with no button held
                rebuildDynamic();
            }
            event->accept();
            return;
        }
        if (hit.kind == plot::HitKind::Axis) {
            setAxisZoom(std::nullopt); // a double-click on an axis restores (04 section 10.1 item 8)
            event->accept();
            return;
        }
    }
    ComponentWidget::mouseDoubleClickEvent(event); // caption: maximise
}

void ScatterPlot::wheelEvent(QWheelEvent* event) {
    const QPointF p = event->position();
    if (hitAt(p).kind != plot::HitKind::Panel) {
        ComponentWidget::wheelEvent(event);
        return;
    }
    event->accept();
    const int notches = plots::wheelNotches(wheelRemainder_, event->angleDelta().y());
    if (notches == 0) {
        return;
    }
    const std::optional<DataPoint> d = dataAt(p);
    if (!d) {
        return;
    }
    const plot::ScatterAxes a = axes()[static_cast<std::size_t>(d->tile)];
    const double f = std::pow(kWheelZoomIn, notches);
    setAxisZoom(AxisZoom{d->x - (d->x - a.xMin) * f, d->x + (a.xMax - d->x) * f, d->y - (d->y - a.yMin) * f,
                         d->y + (a.yMax - d->y) * f});
}

void ScatterPlot::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && cancelTransient()) {
        event->accept();
        return;
    }
    ComponentWidget::keyPressEvent(event);
}

// ---- export

bool ScatterPlot::exportData(ExportSink& sink, const ExportOptions& options) const {
    if (!prepared()) {
        return false;
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(liveInputs(), options.range);
    const plot::ScatterBody body = body_;
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"),
                         [in, body](const CancelToken& cancel) -> std::optional<QByteArray> {
                             Result<std::vector<plot::ScatterExportSet>> sets =
                                 plot::ScatterGeometry::exportSets(in, cancel);
                             if (!sets) {
                                 return std::nullopt;
                             }
                             return scatterCsv(in, body, *sets);
                         });
    return csv && sink.write(*csv);
}

} // namespace esses::widgets
