// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Histogram (04 section 11; D-196, D-198): the layers through LayeredComponent and plot::HistogramGeometry,
// the cycles and toggles, the active trace by a click on a tile, the wheel on the worksheet zoom and the CSV
// export.

#include "plots_common.hpp"

#include <esses/widgets/components/histogram.hpp>

#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace esses::widgets {

namespace {

constexpr auto kHistogramStatic = static_cast<plot::ChangeMask>(plot::kStaticRebuild | plot::kFullRebuild);

LayeredComponent::Builders histogramBuilders() {
    LayeredComponent::Builders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return plot::HistogramGeometry::buildStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        return plot::HistogramGeometry::buildDynamic(in);
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return plot::HistogramGeometry::buildChrome(in, pending);
    };
    return b;
}

plot::HistogramBody parseBody(const JsonDoc& json) {
    Result<plot::HistogramBody> body = plot::HistogramBody::parse(json);
    return body ? std::move(*body) : plot::HistogramBody{};
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

QByteArray histogramCsv(const plot::ComponentBuildInputs& in, const plot::HistogramBody& body,
                        const plot::HistogramExport& table) {
    QByteArray out = "bin_lo,bin_hi";
    for (const plot::HistogramExportColumn& c : table.columns) {
        const QString trace =
            c.trace >= 0 && static_cast<std::size_t>(c.trace) < body.traces.size()
                ? QString::fromStdString(body.traces[static_cast<std::size_t>(c.trace)].channel)
                : QString();
        const QString slot = c.slot < in.slots.size() ? plots::slotLabel(in.slots[c.slot]) : QString();
        out += ',';
        out += plots::csvText(trace + QLatin1Char('@') + slot);
    }
    out += '\n';
    for (std::size_t j = 0; j < table.bins.size(); ++j) {
        out += plots::csvNumber(table.bins[j].lo);
        out += ',';
        out += plots::csvNumber(table.bins[j].hi);
        for (const plot::HistogramExportColumn& c : table.columns) {
            out += ',';
            out += j < c.values.size() ? plots::csvNumber(c.values[j]) : QByteArray();
        }
        out += '\n';
    }
    return out;
}

} // namespace

Histogram::Histogram(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : LayeredComponent(spec, context, histogramBuilders(), kHistogramStatic, parent),
      body_(parseBody(spec.body)) {}

Histogram::~Histogram() = default;

ComponentWidget::OverlayPolicy Histogram::overlayPolicy() const {
    return OverlayPolicy::AllOverlays;
}

qt::CommandContext Histogram::commandContext() const {
    return qt::CommandContext::Summary;
}

void Histogram::onSpecApplied() {
    body_ = parseBody(spec_.body);
}

std::optional<QString> Histogram::activeChannel() const {
    if (!spec_.activeChannel.empty()) {
        return QString::fromStdString(spec_.activeChannel);
    }
    for (const plot::HistogramTrace& t : body_.traces) {
        if (t.visible && !t.channel.empty()) {
            return QString::fromStdString(t.channel);
        }
    }
    return std::nullopt;
}

QStringList Histogram::contextCommands() const {
    return {plots::kCycleLayout,
            plots::kCycleStyle,
            plots::kColourLegend,
            QStringLiteral("component.export_data"),
            QStringLiteral("component.print_clipboard"),
            QStringLiteral("component.properties")};
}

// ---- edits

void Histogram::requestHistogramBody(const plot::HistogramBody& body, const QString& undoText) {
    requestBody(body.toJson(), undoText);
}

void Histogram::addChannel(const QString& name, bool) {
    const std::string channel = name.toStdString();
    if (channel.empty()) {
        return;
    }
    const bool present = std::any_of(body_.traces.begin(), body_.traces.end(),
                                     [&](const plot::HistogramTrace& t) { return t.channel == channel; });
    if (present) {
        return;
    }
    plot::HistogramBody body = body_;
    plot::HistogramTrace trace;
    trace.channel = channel;
    body.traces.push_back(std::move(trace));
    requestHistogramBody(body, tr("Add %1").arg(name));
}

void Histogram::cycleLayout() {
    static constexpr std::array<const char*, 2> kCycle{"overlapped", "tiled"};
    plot::HistogramBody body = body_;
    body.layout = nextOf(body.layout, kCycle);
    requestHistogramBody(body, tr("Histogram layout %1").arg(QString::fromStdString(body.layout)));
}

void Histogram::cycleStyle() {
    static constexpr std::array<const char*, 3> kCycle{"bar", "line", "text"};
    plot::HistogramBody body = body_;
    body.style = nextOf(body.style, kCycle);
    requestHistogramBody(body, tr("Histogram style %1").arg(QString::fromStdString(body.style)));
}

void Histogram::toggleColourLegend() {
    project::ComponentSpec next = spec_;
    next.legend.colour = !next.legend.colour;
    requestSpec(next, next.legend.colour ? tr("Show colour legend") : tr("Hide colour legend"));
}

void Histogram::toggleLegend() {
    project::ComponentSpec next = spec_;
    next.legend.channels = !next.legend.channels;
    requestSpec(next, next.legend.channels ? tr("Show legend") : tr("Hide legend"));
}

void Histogram::setActiveChannel(const QString& name) {
    const std::string channel = name.toStdString();
    if (channel == spec_.activeChannel) {
        return;
    }
    project::ComponentSpec next = spec_;
    next.activeChannel = channel;
    requestSpec(next, tr("Active channel %1").arg(name));
}

bool Histogram::handleCommand(const QString& id, const QVariantList& args) {
    if (id == plots::kCycleLayout) {
        cycleLayout();
        return true;
    }
    if (id == plots::kCycleStyle) {
        cycleStyle();
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
    return ComponentWidget::handleCommand(id, args);
}

// ---- mouse

void Histogram::mousePressEvent(QMouseEvent* event) {
    ComponentWidget::mousePressEvent(event); // focusRequested
    event->accept();
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const plot::HitResult hit = hitAt(event->position());
    if (hit.kind == plot::HitKind::Bin && hit.trace >= 0 &&
        static_cast<std::size_t>(hit.trace) < body_.traces.size()) {
        setActiveChannel(QString::fromStdString(body_.traces[static_cast<std::size_t>(hit.trace)].channel));
    }
}

void Histogram::wheelEvent(QWheelEvent* event) {
    const plot::HitResult hit = hitAt(event->position());
    if (hit.kind != plot::HitKind::Bin && hit.kind != plot::HitKind::Panel) {
        ComponentWidget::wheelEvent(event);
        return;
    }
    event->accept();
    const int notches = plots::wheelNotches(wheelRemainder_, event->angleDelta().y());
    if (notches != 0) {
        plots::zoomAboutCursor(context_, inputs(), std::pow(kWheelZoomIn, notches));
    }
}

// ---- export

bool Histogram::exportData(ExportSink& sink, const ExportOptions& options) const {
    if (!prepared()) {
        return false;
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(liveInputs(), options.range);
    const plot::HistogramBody body = body_;
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"),
                         [in, body](const CancelToken& cancel) -> std::optional<QByteArray> {
                             Result<plot::HistogramExport> table =
                                 plot::HistogramGeometry::exportTable(in, cancel);
                             if (!table) {
                                 return std::nullopt;
                             }
                             return histogramCsv(in, body, *table);
                         });
    return csv && sink.write(*csv);
}

} // namespace esses::widgets
