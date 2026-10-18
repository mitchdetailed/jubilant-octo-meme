// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Histogram, JSON type "histogram" (04 section 11; D-027, D-196, D-198; 07 section 6.1).
// Bins from a BandAxis; the layers come from plot::HistogramGeometry through LayeredComponent (Static on
// Zoom, Offsets, Scale, Size, Domain, Selection, Data and Scheme: HistogramBinner per visible trace and slot
// over the visible span, gating, excludeUntrusted, colour sub-bars, overlays outlined beneath Main; Dynamic
// on the cursor: the bin of the active channel's value at the cursor highlighted, no datum). Behaviours:
//   G  component.cycle_layout  overlapped <-> tiled (spec edit, undoable)
//   S  component.cycle_style   bar -> line -> text
//   K  component.colour_legend the colour legend; L component.legend the channel legend
//   click on a bin of a tile   makes that tile's trace the active channel (drawn last, its bin highlighted)
//   wheel                      the worksheet zoom x0.8 / x1.25 per notch about the cursor (zoom.wheel,
//                              Summary): the histogram recomputes over the new span
//   Export Data                CSV "bin_lo,bin_hi,<trace@slot>..." in the body's y mode (04 section 11.1
//                              item 10); an open band's infinite limit is an empty field
// ALT+n is not handled (a BandAxis has no per-band visibility; D-198). Overlay policy AllOverlays (04 section
// 7.4); key context Summary (06 section 14.2).
#pragma once

#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <optional>

namespace esses::widgets {

class Histogram final : public LayeredComponent {
    Q_OBJECT
public:
    static constexpr const char* kType = "histogram";
    static constexpr double kWheelZoomIn = 0.8; // per notch, about the cursor

    Histogram(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~Histogram() override;

    const char* type() const override { return kType; }
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    std::optional<QString> activeChannel() const override; // the spec's, else the first visible trace
    bool acceptsChannelDrop() const override { return true; }
    void addChannel(const QString& name, bool newPanel) override; // appends a trace
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    ExportJob exportJob(const ExportOptions& options) const override;
    bool handleCommand(const QString& id, const QVariantList& args) override;

    const plot::HistogramBody& body() const noexcept { return body_; }
    void cycleLayout();
    void cycleStyle();
    void toggleColourLegend();
    void toggleLegend();
    void setActiveChannel(const QString& name);

protected:
    void onSpecApplied() override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void requestHistogramBody(const plot::HistogramBody& body, const QString& undoText);

    plot::HistogramBody body_;
    int wheelRemainder_ = 0;
};

} // namespace esses::widgets
