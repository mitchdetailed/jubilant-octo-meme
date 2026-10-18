// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The two halves of the scatter and histogram builders (internal; the tests include it through the plotcore
// test target's src include path):
//   frame   a pure function of the inputs that reads no samples (layout of caption, legend and tiles, the
//           axes' scales from the manual limits or the Main source's SummaryPyramid, the bands from the
//           BandAxis definitions), so the GUI-thread Dynamic build lines up with the worker's Static build;
//   scene   what the worker computes from the samples through esses_analysis (ScatterBinner, TrendFit,
//           HistogramBinner), in draw order;
//   emit    the DrawList and HitTester of a frame and a scene.
// buildStatic = frame + compute the scene + fill the scales the summary could not give + emit. The DrawList
// JSON goldens drive emit with fixed scenes, so they pin the drawing and stay independent of the binning.
#pragma once

#include "geometry_common.hpp"

#include <esses/analysis/band_axis.hpp>
#include <esses/analysis/histogram_binner.hpp>
#include <esses/analysis/scatter_binner.hpp>
#include <esses/analysis/trend_fit.hpp>
#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/plotcore/scatter_geometry.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace esses::plot::detail {

// ---- shared

// One sub-plot: from the left a rotated y title strip (one Axis line) and the y tick strip (36 logical px),
// the plot, a right margin; from the top a margin, the plot, the x tick strip (18) and the x title row.
struct PlotTile {
    RectPx bounds;
    RectPx plot; // the data area
    RectPx yTitle;
    RectPx yAxis; // the y tick strip, left of the plot
    RectPx xAxis; // the x tick strip, under the plot
    RectPx xTitle;
    std::string xTitleText;
    std::string yTitleText;
    geom::Scale x;
    geom::Scale y;
    int trace = -1; // tiled: the body trace of the tile; -1: every visible trace
};

inline constexpr float kLegendSwatchLogical = 10.0F;

// `count` tiles in a grid of `cols` columns (0: ceil(sqrt(count))) over `area`, row by row.
std::vector<PlotTile> layoutPlotTiles(RectPx area, int count, int cols, const ITextMetrics& metrics,
                                      float dpr);
// Grid lines at the ticks (showGrid), the plot frame, the tick labels of the valid scales (x when `xAxis`),
// the titles, and the Panel / Axis hit regions (group = the tile index; Axis handle 0 = x, 1 = y).
void emitTileAxes(DrawList& list, HitTester& hits, const PlotTile& tile, int tileIndex, bool showGrid,
                  bool xAxis, const geom::Colours& colours, const ITextMetrics& metrics, float dpr);
// The device-px position of a value on a tile's axes (NaN outside a valid scale).
float xPxOf(const PlotTile& tile, double v);
float yPxOf(const PlotTile& tile, double v);
// The colour legend (K) in `rect`: a swatch and the lower limit of each band, the highest band on top.
void emitBandLegend(DrawList& list, const RectPx& rect, const std::vector<analysis::Band>& bands,
                    const geom::Colours& colours, const ITextMetrics& metrics, float dpr);

// ---- scatter

struct ScatterFrame {
    RectPx caption;
    RectPx legend; // w == 0 when hidden (K off or no colour channel)
    std::vector<PlotTile> tiles;
    std::vector<int> traces;            // the visible traces with a channel, body order
    bool average = false;               // layout "average": one set per slot, the mean of the visible traces
    std::vector<analysis::Band> zBands; // colour bands in the colour channel's display unit
    std::string zUnit;
};

struct ScatterSet {
    int trace = -1;       // body trace; -1 = the average
    std::size_t slot = 0; // index into ComponentBuildInputs::slots
    int tile = 0;
    analysis::ScatterSeries series; // display units
    std::optional<analysis::TrendResult> trend;
    std::string equation; // Main sets with showEquation and a trend
    std::optional<analysis::IntervalAverage> interval;
    analysis::DensityGrid density; // nx == 0 unless density mode
};

struct ScatterScene {
    std::vector<ScatterSet> sets; // draw order: overlays n..1, Overlay 0, Main
    bool density = false;
};

ScatterFrame scatterFrame(const ComponentBuildInputs& in, const ScatterBody& body);
// The data extents for every scale the frame left invalid (summary incomplete), widened when degenerate.
void fitScatterScales(ScatterFrame& frame, const ScatterScene& scene);
void emitScatterStatic(ComponentBuild& build, const ComponentBuildInputs& in, const ScatterBody& body,
                       const ScatterFrame& frame, const ScatterScene& scene);
// The cursor marks: per Main trace of each tile, the point (x, y) at the cursor and, with a datum, at the
// datum (NaN when unknown).
struct ScatterCursorPoint {
    int tile = 0;
    double x = 0.0;
    double y = 0.0;
    bool datum = false;
};
std::vector<ScatterCursorPoint> scatterCursorPoints(const ComponentBuildInputs& in, const ScatterBody& body,
                                                    const ScatterFrame& frame);
void emitScatterDynamic(DrawList& list, const ComponentBuildInputs& in, const ScatterBody& body,
                        const ScatterFrame& frame, const std::vector<ScatterCursorPoint>& points);

// ---- histogram

struct HistogramFrame {
    RectPx caption;
    RectPx legend; // w == 0 when hidden
    std::vector<PlotTile> tiles;
    std::vector<int> traces;          // the visible traces with a channel, body order
    std::vector<analysis::Band> bins; // in binUnit
    std::string binUnit;
    std::vector<analysis::Band> colourBands; // colour channel bands (sub-bars, legend)
    analysis::HistogramYMode yMode = analysis::HistogramYMode::Percent;
};

struct HistogramSet {
    int trace = 0;
    std::size_t slot = 0;
    int tile = 0;
    analysis::HistogramResult result;
};

struct HistogramScene {
    std::vector<HistogramSet> sets; // draw order: overlays n..1, Overlay 0, Main; the active trace last
};

HistogramFrame histogramFrame(const ComponentBuildInputs& in, const HistogramBody& body);
// The y scales from the data when the body's yScale is auto (0 to the largest value).
void fitHistogramScales(HistogramFrame& frame, const HistogramBody& body, const HistogramScene& scene);
void emitHistogramStatic(ComponentBuild& build, const ComponentBuildInputs& in, const HistogramBody& body,
                         const HistogramFrame& frame, const HistogramScene& scene);
// The bin of the active channel's value at the cursor (Main), when it lies in a bin.
std::optional<std::size_t> histogramCursorBin(const ComponentBuildInputs& in, const HistogramBody& body,
                                              const HistogramFrame& frame);
void emitHistogramDynamic(DrawList& list, const ComponentBuildInputs& in, const HistogramBody& body,
                          const HistogramFrame& frame, std::optional<std::size_t> bin);

} // namespace esses::plot::detail
