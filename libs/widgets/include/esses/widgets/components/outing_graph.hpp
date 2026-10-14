// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OutingGraph, JSON type "outing" (04 section 9; D-027, D-196, D-198; 07 section 6.1).
// The whole Main outing from the SummaryPyramid (plot::OutingGeometry through LayeredComponent: Static on
// Selection, Data, Size, Scale, Domain and Scheme; the zoom, the cursor and the key values are Dynamic, so a
// zoom or cursor change never rebuilds the traces). The shared zoom is the draggable rectangle: its handles
// come from OutingGeometry::zoomRect() (D-196 (d)): dragging an end (5 logical px either side) sets that end
// of the worksheet zoom, dragging the bottom edge pans at constant span (hitAt reports HitKind::ZoomSpan with
// HandleKind::ZoomStart / ZoomEnd / ZoomPan). It is not zoomable itself: the wheel (x0.8 / x1.25 about the
// pointer) and the rubber band (double-click, move, click; ESC cancels) set the worksheet zoom, UP/DOWN/F2/W
// are the shell's Graph commands; links.zoom is always honoured. A click or drag in a panel places the
// cursor; a double-click on a lap of the range band zooms the worksheet to that lap. S cycles line <->
// points, L the key column (labels <-> none). enableOverlays draws the overlays' outings in time mode
// (overlayPolicy() is then AllOverlays). Export Data: CSV of the outing's channels in Main over the export
// range. Overlay policy MainOnly (04 section 7.4); key context Graph (06 section 14.2).
#pragma once

#include <esses/plotcore/outing_geometry.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <QRectF>
#include <cstdint>
#include <optional>

namespace esses::widgets {

class OutingGraph final : public LayeredComponent {
    Q_OBJECT
public:
    static constexpr const char* kType = "outing";
    static constexpr double kHandleLogical =
        5.0;                                    // grab distance of the zoom rectangle's ends and bottom edge
    static constexpr double kWheelZoomIn = 0.8; // per notch, the worksheet zoom about the pointer

    OutingGraph(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~OutingGraph() override;

    const char* type() const override { return kType; }
    OverlayPolicy overlayPolicy() const override; // MainOnly; AllOverlays with enableOverlays
    qt::CommandContext commandContext() const override;
    std::optional<QString> activeChannel() const override; // the spec's, else the first trace
    bool acceptsChannelDrop() const override { return true; }
    // Like-units grouping (04 section 8.2 item 3): the first group of the channel's quantity, else (or with
    // CTRL) a new group.
    void addChannel(const QString& name, bool newPanel) override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    bool handleCommand(const QString& id, const QVariantList& args) override;
    plot::HitResult hitAt(QPointF logical) const override; // the zoom handles first, then the Static hits
    bool cancelTransient() override;                       // ESC: drops a rubber band in progress

    const plot::OutingBody& body() const noexcept { return body_; }
    // The zoom rectangle in widget-logical px (empty before the first build or outside the outing).
    QRectF zoomRectLogical() const;
    // Worksheet x (the zoom's units) at a widget-logical x, and back; nullopt / NaN before the first build.
    std::optional<double> worksheetXAt(double logicalX) const;
    double logicalXOfWorksheet(double x) const;
    // Zooms the worksheet to lap `index` of the range band (its cells in band order).
    bool zoomToLap(int index);
    bool rubberBandActive() const noexcept { return bandStart_.has_value(); }
    void toggleKey();
    void cycleStyle();

protected:
    void onSpecApplied() override;
    void appendDynamic(plot::DrawList& list) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Drag : std::uint8_t { None, Cursor, ZoomStart, ZoomEnd, ZoomPan };
    struct Axis {
        double x0 = 0.0;     // device px of the outing start
        double width = 0.0;  // device px of the whole outing
        double origin = 0.0; // worksheet x of the outing start
        double length = 0.0; // worksheet units of the whole outing
    };
    std::optional<Axis> axis() const;
    void setCursorAtLogicalX(double logicalX);
    void requestOutingBody(const plot::OutingBody& body, const QString& undoText);

    plot::OutingBody body_;
    Drag drag_ = Drag::None;
    double dragStartX_ = 0.0; // worksheet x under the pointer when a pan started
    plot::XSpan dragStartZoom_;
    std::optional<double> bandStart_; // rubber band, worksheet x
    double bandEnd_ = 0.0;
    int wheelRemainder_ = 0;
};

} // namespace esses::widgets
