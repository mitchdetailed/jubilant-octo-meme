// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BarGauge (04 section 17; D-198): the layers through LayeredComponent and plot::GaugeGeometry's bar
// builders, ALT+n, the channel drop and the CSV export.

#include "plots_common.hpp"

#include <esses/widgets/components/bar_gauge.hpp>

#include <utility>

namespace esses::widgets {

namespace {

// Zoom and Offsets move nothing a bar gauge draws: they only rebuild its Dynamic layer.
constexpr auto kGaugeStatic =
    static_cast<plot::ChangeMask>(plot::Change::Size | plot::Change::Scale | plot::kFullRebuild);

LayeredComponent::Builders barBuilders() {
    LayeredComponent::Builders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return plot::GaugeGeometry::buildBarStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        return plot::GaugeGeometry::buildBarDynamic(in);
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return plot::GaugeGeometry::buildBarChrome(in, pending);
    };
    return b;
}

plot::GaugeBarBody parseBody(const JsonDoc& json) {
    Result<plot::GaugeBarBody> body = plot::GaugeBarBody::parse(json);
    return body ? std::move(*body) : plot::GaugeBarBody{};
}

} // namespace

BarGauge::BarGauge(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : LayeredComponent(spec, context, barBuilders(), kGaugeStatic, parent), body_(parseBody(spec.body)) {}

BarGauge::~BarGauge() = default;

ComponentWidget::OverlayPolicy BarGauge::overlayPolicy() const {
    return OverlayPolicy::MainAndReference;
}

qt::CommandContext BarGauge::commandContext() const {
    return qt::CommandContext::Component;
}

void BarGauge::onSpecApplied() {
    body_ = parseBody(spec_.body);
}

std::optional<QString> BarGauge::activeChannel() const {
    if (body_.channel.empty()) {
        return std::nullopt;
    }
    return QString::fromStdString(body_.channel);
}

QStringList BarGauge::contextCommands() const {
    return {QStringLiteral("component.export_data"), QStringLiteral("component.print_clipboard"),
            QStringLiteral("component.properties")};
}

void BarGauge::addChannel(const QString& name, bool) {
    const std::string channel = name.toStdString();
    if (channel.empty() || channel == body_.channel) {
        return;
    }
    plot::GaugeBarBody body = body_;
    body.channel = channel;
    body.displayUnit.clear(); // the new channel's own unit
    requestBody(body.toJson(), tr("Gauge channel %1").arg(name));
}

bool BarGauge::toggleColourBand(int band) {
    if (band < 0 || static_cast<std::size_t>(band) >= body_.colourBands.size()) {
        return false; // bands beyond the list are no-ops (04 section 7.2 item 4)
    }
    plot::GaugeBarBody body = body_;
    plot::ColourBand& b = body.colourBands[static_cast<std::size_t>(band)];
    b.visible = !b.visible;
    requestBody(body.toJson(),
                b.visible ? tr("Show colour band %1").arg(band) : tr("Hide colour band %1").arg(band));
    return true;
}

bool BarGauge::handleCommand(const QString& id, const QVariantList& args) {
    if (id == plots::kColourBand) {
        toggleColourBand(plots::bandArg(args));
        return true; // handled, also when the band does not exist
    }
    return ComponentWidget::handleCommand(id, args);
}

ComponentWidget::ExportJob BarGauge::exportJob(const ExportOptions& options) const {
    if (!prepared()) {
        return {};
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(liveInputs(), options.range);
    return [in](const CancelToken& cancel) { return plots::timeSeriesCsv(in, {0}, cancel); };
}

bool BarGauge::exportData(ExportSink& sink, const ExportOptions& options) const {
    const ExportJob job = exportJob(options);
    if (!job) {
        return false;
    }
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"), job);
    return csv && sink.write(*csv);
}

} // namespace esses::widgets
