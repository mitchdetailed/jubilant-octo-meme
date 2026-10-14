// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The scatter component (04 section 10): its persisted body (04 section 10.2, schemas/components/scatter-1)
// and its layer builders (ScatterGeometry; ARCHITECTURE_OUTLINE section 4 esses_plotcore), shared by the
// widget (esses_widgets ScatterPlot) and OffscreenRenderer / esses-render.
//   buildStatic   worker, Priority::Visible, on Zoom | Selection | Data | Offsets: ScatterBinner::gather over
//                 each visible slot's visible span (overlays beneath Main, in key colours), layout overlapped
//                 | tiled | average, style points | pointsTrend | trend with TrendFit per trace and slot
//                 (equation text for Main), interval average, density mode above the threshold, the
//                 component's own axes (x/y scales channel | manual, box zoom and wheel on its own axes), the
//                 colour legend (K); plus HitTester point sets (nearestPoint: click -> setCursor on the link
//                 group)
//   buildDynamic  GUI thread: the Main point at the cursor as a ring, the datum point hollow
//   buildChrome   GUI thread: caption, focus border, pending stripe
// SlotData::channels order (componentChannelUses): x, every y trace, then the colour channel and the gating
// channel when set.
// The body's members are final (the schema is the contract, AC-M3-08); src/geometry/scatter_geometry.cpp
// (layout, scales and styles in its header comment; the frame / scene / emit split in
// src/geometry/plot_scenes.hpp). The points come from analysis::ScatterBinner, the trend from
// analysis::TrendFit; a failing (Unsupported) binner fails the Static build. The point sets of the HitTester
// carry each point's Main-lap time as its tag (HitTester::nearestPoint, D-196): a click sets the cursor to
// it.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/analysis/trend_fit.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esses::plot {

struct ScatterTrace {
    std::string yChannel;
    bool visible = true;
    Micros yDelayUs = 0;
};

struct ScatterBody { // 04 section 10.2
    std::string xChannel;
    std::vector<ScatterTrace> traces;
    std::string colourChannel;                    // colourChannel.channel ("" = none)
    std::optional<analysis::BandAxis> colourAxis; // colourChannel.axis
    std::string gatingChannel;                    // gating.channel ("" = none)
    std::string layout = "overlapped";            // overlapped | tiled | average
    int tileCols = 0;                             // 0 = auto (ceil(sqrt(N)))
    std::string style = "points";                 // points | pointsTrend | trend
    float pointSize = 2.0F;
    analysis::TrendSpec trend; // trend.type/order/throughZero
    bool showEquation = true;
    int significantDigits = 4;
    bool intervalAverage = false; // intervalAverage.enabled
    std::optional<analysis::UnitValue> intervalBinWidth;
    ScaleSpec xScale;
    ScaleSpec yScale;
    ScaleSpec zScale;
    std::string cursorStyle = "cross";
    bool densityAuto = true;
    std::uint64_t densityThreshold = 1000000;
    int densityBinsX = 256;
    int densityBinsY = 256;
    bool showGrid = true;
    JsonDoc rest; // the body as parsed: unknown members survive toJson()

    static Result<ScatterBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
    std::vector<ChannelUse> channels() const; // x, y..., colour, gating
};

// The points of one set as buildStatic gathers them (Export CSV, 04 section 10.1 item 10; D-198): display
// units; z is empty without a colour channel.
struct ScatterExportSet {
    int trace = -1;       // body trace; -1 = the mean of the visible traces (layout "average")
    std::size_t slot = 0; // index into ComponentBuildInputs::slots
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z;
};

// One tile's data area and scales as the Static and Dynamic layers draw them (the widget's box zoom, wheel
// and point picking on the component's own axes; D-198). A scale is NaN while the SummaryPyramid it comes
// from is incomplete.
struct ScatterAxes {
    RectPx plot;    // device px
    int trace = -1; // tiled: the body trace of the tile; -1: every visible trace
    double xMin = 0.0;
    double xMax = 0.0;
    double yMin = 0.0;
    double yMax = 0.0;
};

class ScatterGeometry {
public:
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
    // Worker: the sets of every visible slot (Main first, then Overlay 0 and Overlays 1..n) through
    // ScatterBinner, as buildStatic gathers them; a body that does not parse, Cancelled and Unsupported fail.
    static Result<std::vector<ScatterExportSet>> exportSets(const ComponentBuildInputs& inputs,
                                                            const CancelToken& cancel);
    // Any thread, reads no samples: the tiles of the frame (the layout, the manual or summary scales).
    static std::vector<ScatterAxes> axes(const ComponentBuildInputs& inputs);
};

} // namespace esses::plot
