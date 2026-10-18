// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ZoomSummaryBar (06 sections 2.1 and 4.5; 04 section 8.6; D-133; 07 section 6.1): the strip above the
// worksheet tab bar that shows the one workspace-level zoom: the Main outing extent with lap boundaries, the
// zoom span as a translucent rectangle and the cursor as a line, in the active worksheet's domain (read from
// its WorksheetViewModel, which composes the WorkspaceViewModel's shared zoom). Mouse: click = set cursor,
// drag = move span, SHIFT+click = extendZoom (zoom.extend), double-click = zoomFull. Repaints on the
// worksheet's changed(ChangeMask) (Zoom, Cursor, Domain, Selection, Data).
// As built (M3): the picture is a plot::DrawList (buildDrawList, a pure function of a Summary and the scheme:
// the DrawList JSON golden of tests/golden/drawlist/zoom_summary/) painted with qt::RasterBackend::paintList.
// Positions are in the worksheet's domain relative to the Main range start (04 section 3.4): time exactly,
// distance through the Main lap's DistanceChain (the fraction rule while it is pending, as the F9 rule does).
// A click converts back the same way and sets the cursor (CursorSource::Click); a drag moves the span by
// the pointer's travel (one zoomTo per mouse move).
#pragma once

#include <esses/plotcore/domain_mapper.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/scheme.hpp>

#include <QWidget>
#include <memory>
#include <optional>
#include <vector>

namespace esses::qt {
class DataSelectionModel;
class WorksheetViewModel;
} // namespace esses::qt

namespace esses::widgets {

class ZoomSummaryBar final : public QWidget {
    Q_OBJECT
public:
    // What one paint shows, in domain units relative to the Main range start.
    struct Summary {
        bool valid = false;       // a Main range exists
        plot::XSpan extent;       // the Main outing
        plot::XSpan main;         // the Main range [0, |R|)
        std::vector<double> laps; // lap starts inside the outing (the Main lap's start is 0)
        plot::XSpan zoom;
        std::optional<double> cursor;
        std::optional<double> datum;
    };

    explicit ZoomSummaryBar(QWidget* parent = nullptr);
    ~ZoomSummaryBar() override;

    // The active worksheet (null: an empty bar). Not owned.
    void setWorksheet(qt::WorksheetViewModel* worksheet);
    qt::WorksheetViewModel* worksheet() const noexcept { return worksheet_; }
    // Lap boundaries of the Main outing come from the selection's Main source. Not owned.
    void setSelection(qt::DataSelectionModel* selection);
    void setScheme(std::shared_ptr<const project::Scheme> scheme);

    QSize sizeHint() const override;

    // The current summary (recomputed on every change of the worksheet).
    const Summary& summary() const noexcept { return summary_; }
    // The picture of `s` in a layer of `ctx` (device px), the scheme's roles as colours.
    static plot::DrawList buildDrawList(const Summary& s, const plot::LayoutContext& ctx,
                                        const project::Scheme& scheme);
    // The domain position under a widget-logical x (nullopt without a Main range).
    std::optional<double> xAt(double logicalX) const;
    // The widget-logical x of a domain position.
    double pxOf(double x) const;
    int repaints() const noexcept { return repaints_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void recompute();
    std::optional<double> timeOfX(double x) const; // domain x -> microseconds from the Main start
    double xOfT(double tRel) const;                // the inverse

    qt::WorksheetViewModel* worksheet_ = nullptr;
    std::shared_ptr<const plot::DistanceChain> chain_;
    plot::RangeGeom geom_;
    plot::Domain domain_ = plot::Domain::Time;
    qt::DataSelectionModel* selection_ = nullptr;
    std::shared_ptr<const project::Scheme> scheme_;
    Summary summary_;
    std::optional<double> dragFrom_; // domain x of the press while dragging the span
    plot::XSpan dragZoom_;
    bool dragged_ = false;
    int repaints_ = 0;
};

} // namespace esses::widgets
