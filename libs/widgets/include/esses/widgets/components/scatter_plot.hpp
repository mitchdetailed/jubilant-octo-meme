// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterPlot, JSON type "scatter" (04 section 10; D-027, D-196, D-198; 07 section 6.1).
// X against N Y traces with optional colour (K legend) and gating channels; the layers come from
// plot::ScatterGeometry through LayeredComponent (Static on Zoom, Offsets, Scale, Size, Domain, Selection,
// Data and Scheme: the points of every visible slot's visible span, overlays beneath Main; Dynamic on the
// cursor and the datum: the Main point at the cursor as a ring or cross, the datum hollow). Behaviours:
//   G  component.cycle_layout  overlapped -> tiled -> average (spec edit, undoable)
//   S  component.cycle_style   points -> pointsTrend -> trend
//   Y  cursor.style            cross -> ring -> none
//   K  component.colour_legend the colour legend (spec.legend.colour); L component.legend the channel legend
//   LEFT/RIGHT (CTRL: x10)     the cursor to the previous / next Main point (one X sample)
//   click                      the nearest Main point within 8 logical px (HitTester::nearestPoint, its tag
//                              the Main-lap time, D-196) -> setCursor on the link group
//   double-click, move, click  box zoom on the component's own axes (ESC cancels); the wheel zooms both axes
//                              x0.8 / x1.25 per notch about the pointer; a double-click on an axis restores.
//                              The axis zoom is view state of the widget (not persisted): the inputs get
//                              manual x/y scales in the axes' display units (adjustInputs)
//   component.copy_equation    the Main trend equation as last drawn to the clipboard (Component menu)
//   Export Data                CSV "set,x,y[,z]" per point of every set (04 section 10.1 item 10)
// The worksheet zoom only selects the data span (UP/DOWN/F2/W are the shell's Summary commands). ALT+n is
// not handled (a BandAxis has no per-band visibility; D-198). Overlay policy AllOverlays (04 section 7.4);
// key context Summary (06 section 14.2).
#pragma once

#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <optional>
#include <vector>

namespace esses::widgets {

class ScatterPlot final : public LayeredComponent {
    Q_OBJECT
public:
    static constexpr const char* kType = "scatter";
    static constexpr double kPickRadiusLogical = 8.0; // a click picks Main points this close
    static constexpr double kWheelZoomIn = 0.8;       // per notch

    // The component's own axis zoom, in the display units of the axes.
    struct AxisZoom {
        double xMin = 0.0;
        double xMax = 0.0;
        double yMin = 0.0;
        double yMax = 0.0;
        friend bool operator==(const AxisZoom&, const AxisZoom&) = default;
    };

    ScatterPlot(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~ScatterPlot() override;

    const char* type() const override { return kType; }
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    std::optional<QString> activeChannel() const override; // the spec's, else the first visible Y trace
    bool acceptsChannelDrop() const override { return true; }
    // A Y trace (the X channel while there is none); CTRL makes it the X channel.
    void addChannel(const QString& name, bool newPanel) override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    ExportJob exportJob(const ExportOptions& options) const override;
    bool handleCommand(const QString& id, const QVariantList& args) override;
    bool cancelTransient() override; // ESC: drops a box zoom in progress

    const plot::ScatterBody& body() const noexcept { return body_; }
    void cycleLayout();
    void cycleStyle();
    void cycleCursorStyle();
    void toggleColourLegend();
    void toggleLegend();
    // Moves the cursor by `points` Main points (X samples); false without a uniform X channel in Main.
    bool stepCursor(int points);
    // The Main trend equation of the last delivered Static build (Copy Equation); empty without one.
    QString equationText() const { return equation_; }
    bool copyEquation() const;
    // The axes of the current inputs (device px, display units).
    std::vector<plot::ScatterAxes> axes() const;
    const std::optional<AxisZoom>& axisZoom() const noexcept { return axisZoom_; }
    void setAxisZoom(std::optional<AxisZoom> zoom);
    bool boxZoomActive() const noexcept { return boxStart_.has_value(); }
    // Moves the cursor to the Main point nearest to a widget-logical position (within kPickRadiusLogical).
    bool pickPoint(QPointF logical);

protected:
    void adjustInputs(plot::ComponentBuildInputs& inputs) const override;
    void onStaticDelivered(const plot::ComponentBuild& build) override;
    void appendDynamic(plot::DrawList& list) const override;
    void onSpecApplied() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct DataPoint {
        int tile = 0;
        double x = 0.0;
        double y = 0.0;
    };
    std::optional<DataPoint> dataAt(QPointF logical, int tile = -1) const;
    void requestScatterBody(const plot::ScatterBody& body, const QString& undoText);

    plot::ScatterBody body_;
    std::optional<AxisZoom> axisZoom_;
    std::optional<DataPoint> boxStart_; // box zoom: double-click, move, click
    DataPoint boxEnd_;
    QString equation_;
    int wheelRemainder_ = 0;
};

} // namespace esses::widgets
