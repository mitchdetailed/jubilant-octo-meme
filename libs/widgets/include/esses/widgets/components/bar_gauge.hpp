// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BarGauge, JSON type "gauge_bar" (04 section 17; D-027, D-198; 07 section 6.1).
// One channel as a bar from its origin to the value at the cursor, the reference overlay as a marker line;
// the layers come from plot::GaugeGeometry's bar builders through LayeredComponent: Static (the scale, the
// colour bands, the track) on Size, Scale, Domain, Selection, Data and Scheme; Dynamic (the bar, the marker,
// the value text) on the cursor and every other change. Behaviours:
//   ALT+n  component.colour_band [n]  toggles colour band n (spec edit, undoable; bands beyond the list are
//                                     no-ops)
//   ENTER / drop                      the channel replaces the gauge's channel
//   Export Data                       CSV of the channel's Main samples over the export range
// Overlay policy MainAndReference (04 section 7.4); key context Component (06 section 14.2).
#pragma once

#include <esses/plotcore/gauge_geometry.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <optional>

namespace esses::widgets {

class BarGauge final : public LayeredComponent {
    Q_OBJECT
public:
    static constexpr const char* kType = "gauge_bar";

    BarGauge(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~BarGauge() override;

    const char* type() const override { return kType; }
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    std::optional<QString> activeChannel() const override; // the gauge's channel
    bool acceptsChannelDrop() const override { return true; }
    void addChannel(const QString& name, bool newPanel) override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    bool handleCommand(const QString& id, const QVariantList& args) override;

    const plot::GaugeBarBody& body() const noexcept { return body_; }
    // ALT+n: toggles band n; false (no edit) for a band beyond the list.
    bool toggleColourBand(int band);

protected:
    void onSpecApplied() override;

private:
    plot::GaugeBarBody body_;
};

} // namespace esses::widgets
