// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HitTester (04 section 5.5; ARCHITECTURE_OUTLINE section 4 esses_plotcore): pixel -> {kind, group, trace,
// slot, band, marker, handle, ...} for clicks, hover and drag. Built alongside the Static DrawList on the
// worker and kept with the layer; queried on the GUI thread. M1 basics: panels, key labels and traces by
// rectangle. M3 (07 section 6.1 "full HitTester"): the kinds and handles of the bands, markers, annotations,
// the status strip, the variance panel, the offset axis, the scrollbars, the Lo/Hi and splitter handles, the
// outing zoom rectangle, scatter points, histogram bins and track sections; hitAll() for stacked regions; and
// the nearest-point query of scatter (04 section 10.1 item 7: a 64 x 64 bucket grid over the point array). M3
// contract: the new kinds, HandleKind and the HitResult members are final; hitAll() is final. addPoints() and
// nearestPoint() (src/geometry/hit_tester_points.cpp) carry an optional tag per point (D-196: a scatter
// point's Main-lap time, so a click moves the cursor without the gathered points).
#pragma once

#include <esses/model/ids.hpp>
#include <esses/plotcore/draw_list.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace esses::plot {

// Appended in M3 after Handle (the M1 values keep their numbers).
enum class HitKind : std::uint8_t {
    None,
    Panel,
    Trace,
    KeyLabel,
    KeyValue,
    Axis,
    Band,
    Marker,
    Handle,
    GroupHeading,   // key column group heading (drag to reorder, double-click: Group Properties)
    KeyMeasurement, // a min/max/avg cell of the measurements row (hover line, double-click: cursor)
    ScrollBar,      // horizontal or vertical scrollbar (double-click: full)
    OffsetAxis,     // the O axis (one sub-axis per slot)
    Variance,       // the variance panel
    StatusRow,      // one status-strip row (hover: channel and bit)
    Annotation,     // annotation rectangle in the marker band or its dotted line
    Point,          // scatter point (nearestPoint)
    Bin,            // histogram bin
    Section,        // track section (track_position, section band)
    ZoomSpan,       // the outing graph's / zoom summary bar's zoom rectangle
    Caption         // caption bar (double-click: maximise)
};

enum class HandleKind : std::uint8_t {
    None,
    GroupLo,       // overlapped mode: a group's lo / hi handle at the right edge (04 section 8.2 item 2)
    GroupHi,       //
    PanelSplitter, // panels mode: the splitter between two panels
    ZoomStart,     // outing zoom rectangle: ends and bottom edge (04 section 9 item 3)
    ZoomEnd,       //
    ZoomPan,       //
    OffsetMain,    // offset axis sub-axes (04 section 8.9)
    OffsetReference,
    ScrollThumb,
    ScrollStart,
    ScrollEnd
};

struct HitResult {
    HitKind kind = HitKind::None;
    int group = -1;
    int trace = -1;
    int slot = -1;
    int band = -1;
    model::MarkerId markerId;
    int handle = -1; // M1: an index; M3 handles also set handleKind
    // M3 additions
    HandleKind handleKind = HandleKind::None;
    int point = -1;   // index into the point set (nearestPoint)
    int bin = -1;     // histogram bin
    int section = -1; // index into TrackMap::sections
    int cell = -1;    // measurement cell: 0 min, 1 max, 2 avg
};

struct PointPx {
    float x = 0.0F;
    float y = 0.0F;
};

class HitTester {
public:
    static constexpr int kPointGrid = 64; // buckets per side of a point set's bounds

    // Later additions win over earlier ones (paint order).
    void addRect(RectPx rect, HitResult result);
    HitResult hit(float xPx, float yPx) const noexcept;
    // Every region under the pixel, topmost (last added) first; the bands and the trace area overlap.
    std::vector<HitResult> hitAll(float xPx, float yPx) const;
    // A point set (one trace of one slot) for nearest-point queries; `base` supplies kind (Point when None),
    // trace and slot, and the result's `point` is the index into `points`. Bucketed over `bounds` on a
    // kPointGrid grid (points outside it fall into the edge buckets; non-finite points are never found).
    // `tags` is empty or holds one value per point (D-196).
    void addPoints(RectPx bounds, std::span<const PointPx> points, HitResult base,
                   std::span<const std::int64_t> tags = {});
    // The nearest point of every point set within maxDistancePx (Euclidean, screen space, inclusive; within a
    // set the lower index and across sets the later set wins ties); kind None when there is none. `tag`, when
    // not null, receives the point's tag (0 when its set has none or nothing was found).
    HitResult nearestPoint(float xPx, float yPx, float maxDistancePx, std::int64_t* tag = nullptr) const;
    void clear() noexcept {
        regions_.clear();
        pointSets_.clear();
    }
    bool empty() const noexcept { return regions_.empty() && pointSets_.empty(); }

private:
    struct Region {
        RectPx rect;
        HitResult result;
    };
    struct PointSet {
        RectPx bounds;
        HitResult base;
        std::vector<PointPx> points;
        std::vector<std::int64_t> tags;         // empty or one per point
        std::vector<std::uint32_t> bucketStart; // kPointGrid^2 + 1 offsets into `order`
        std::vector<std::uint32_t> order;       // indices of the finite points, sorted by bucket
    };
    std::vector<Region> regions_;
    std::vector<PointSet> pointSets_;
};

} // namespace esses::plot
