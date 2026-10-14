// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// NumericGauge (04 section 16; D-198): the layers through LayeredComponent and plot::GaugeGeometry's numeric
// builders, ALT+n, the channel drop and the CSV export.

#include "plots_common.hpp"

#include <esses/widgets/components/numeric_gauge.hpp>

#include <utility>

namespace esses::widgets {

namespace {

// Zoom and Offsets move nothing a numeric gauge draws: they only rebuild its Dynamic layer.
constexpr auto kGaugeStatic =
    static_cast<plot::ChangeMask>(plot::Change::Size | plot::Change::Scale | plot::kFullRebuild);

LayeredComponent::Builders numericBuilders() {
    LayeredComponent::Builders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return plot::GaugeGeometry::buildNumericStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        return plot::GaugeGeometry::buildNumericDynamic(in);
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return plot::GaugeGeometry::buildNumericChrome(in, pending);
    };
    return b;
}

plot::GaugeNumericBody parseBody(const JsonDoc& json) {
    Result<plot::GaugeNumericBody> body = plot::GaugeNumericBody::parse(json);
    return body ? std::move(*body) : plot::GaugeNumericBody{};
}

} // namespace

NumericGauge::NumericGauge(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : LayeredComponent(spec, context, numericBuilders(), kGaugeStatic, parent), body_(parseBody(spec.body)) {}

NumericGauge::~NumericGauge() = default;

ComponentWidget::OverlayPolicy NumericGauge::overlayPolicy() const {
    return OverlayPolicy::MainAndReference;
}

qt::CommandContext NumericGauge::commandContext() const {
    return qt::CommandContext::Component;
}

void NumericGauge::onSpecApplied() {
    body_ = parseBody(spec_.body);
}

std::optional<QString> NumericGauge::activeChannel() const {
    if (body_.channel.empty()) {
        return std::nullopt;
    }
    return QString::fromStdString(body_.channel);
}

QStringList NumericGauge::contextCommands() const {
    return {QStringLiteral("component.export_data"), QStringLiteral("component.print_clipboard"),
            QStringLiteral("component.properties")};
}

void NumericGauge::addChannel(const QString& name, bool) {
    const std::string channel = name.toStdString();
    if (channel.empty() || channel == body_.channel) {
        return;
    }
    plot::GaugeNumericBody body = body_;
    body.channel = channel;
    body.displayUnit.clear(); // the new channel's own unit
    requestBody(body.toJson(), tr("Gauge channel %1").arg(name));
}

bool NumericGauge::toggleColourBand(int band) {
    if (band < 0 || static_cast<std::size_t>(band) >= body_.colourBands.size()) {
        return false; // bands beyond the list are no-ops (04 section 7.2 item 4)
    }
    plot::GaugeNumericBody body = body_;
    plot::ColourBand& b = body.colourBands[static_cast<std::size_t>(band)];
    b.visible = !b.visible;
    requestBody(body.toJson(),
                b.visible ? tr("Show colour band %1").arg(band) : tr("Hide colour band %1").arg(band));
    return true;
}

bool NumericGauge::handleCommand(const QString& id, const QVariantList& args) {
    if (id == plots::kColourBand) {
        toggleColourBand(plots::bandArg(args));
        return true; // handled, also when the band does not exist
    }
    return ComponentWidget::handleCommand(id, args);
}

bool NumericGauge::exportData(ExportSink& sink, const ExportOptions& options) const {
    if (!prepared()) {
        return false;
    }
    const plot::ComponentBuildInputs in = plots::exportInputs(liveInputs(), options.range);
    const std::optional<QByteArray> csv =
        plots::runOffGui(context_, componentId() + QStringLiteral("/export"),
                         [in](const CancelToken& cancel) { return plots::timeSeriesCsv(in, {0}, cancel); });
    return csv && sink.write(*csv);
}

} // namespace esses::widgets
