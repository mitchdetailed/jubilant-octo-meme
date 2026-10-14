// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The tiles (sub-plots) shared by scatter and histogram: grid layout, value axes, titles and hit regions.

#include "plot_scenes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace esses::plot::detail {

namespace {

constexpr float kTopMarginLogical = 6.0F;
constexpr float kRightMarginLogical = 8.0F;

} // namespace

std::vector<PlotTile> layoutPlotTiles(RectPx area, int count, int cols, const ITextMetrics& metrics,
                                      float dpr) {
    std::vector<PlotTile> tiles;
    if (count <= 0) {
        return tiles;
    }
    if (cols <= 0) {
        cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)) - 1e-9));
    }
    cols = std::clamp(cols, 1, count);
    const int rows = (count + cols - 1) / cols;
    const float lineH = geom::snap(metrics.lineHeightPx(TextStyleId::Axis, dpr));
    const float yAxisW = geom::snap(geom::kAxisStripLogical * dpr);
    const float xAxisH = geom::snap(geom::kXAxisLogical * dpr);
    const float top = geom::snap(kTopMarginLogical * dpr);
    const float right = geom::snap(kRightMarginLogical * dpr);
    for (int i = 0; i < count; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        const float x0 = area.x + geom::snap(area.w * static_cast<float>(c) / static_cast<float>(cols));
        const float x1 = area.x + geom::snap(area.w * static_cast<float>(c + 1) / static_cast<float>(cols));
        const float y0 = area.y + geom::snap(area.h * static_cast<float>(r) / static_cast<float>(rows));
        const float y1 = area.y + geom::snap(area.h * static_cast<float>(r + 1) / static_cast<float>(rows));
        PlotTile t;
        t.bounds = RectPx{x0, y0, x1 - x0, y1 - y0};
        const float plotX = x0 + lineH + yAxisW;
        const float plotY = y0 + top;
        const float plotW = std::max(0.0F, x1 - right - plotX);
        const float plotH = std::max(0.0F, y1 - lineH - xAxisH - plotY);
        t.plot = RectPx{plotX, plotY, plotW, plotH};
        t.yTitle = RectPx{x0, plotY, lineH, plotH};
        t.yAxis = RectPx{x0 + lineH, plotY, yAxisW, plotH};
        t.xAxis = RectPx{plotX, plotY + plotH, plotW, xAxisH};
        t.xTitle = RectPx{plotX, plotY + plotH + xAxisH, plotW, lineH};
        tiles.push_back(std::move(t));
    }
    return tiles;
}

float xPxOf(const PlotTile& tile, double v) {
    if (!tile.x.valid() || tile.x.vMax == tile.x.vMin) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const double f = (v - tile.x.vMin) / (tile.x.vMax - tile.x.vMin);
    return static_cast<float>(static_cast<double>(tile.plot.x) + f * static_cast<double>(tile.plot.w));
}

float yPxOf(const PlotTile& tile, double v) {
    if (!tile.y.valid() || tile.y.vMax == tile.y.vMin) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const double f = (tile.y.vMax - v) / (tile.y.vMax - tile.y.vMin);
    return static_cast<float>(static_cast<double>(tile.plot.y) + f * static_cast<double>(tile.plot.h));
}

void emitTileAxes(DrawList& list, HitTester& hits, const PlotTile& tile, int tileIndex, bool showGrid,
                  bool xAxis, const geom::Colours& colours, const ITextMetrics& metrics, float dpr) {
    const RectPx& plot = tile.plot;
    if (!(plot.w > 0.0F) || !(plot.h > 0.0F)) {
        return;
    }
    const float lineW = geom::lineWidthOf(dpr);
    std::vector<AxisTick> xTicks;
    std::vector<AxisTick> yTicks;
    AxisSpec xSpec;
    xSpec.kind = AxisKind::Value;
    if (xAxis && tile.x.valid()) {
        xSpec.lo = tile.x.vMin;
        xSpec.hi = tile.x.vMax;
        xSpec.lengthPx = plot.w;
        xTicks = AxisLayout::ticks(xSpec, metrics, dpr);
    }
    AxisSpec ySpec;
    ySpec.kind = AxisKind::Value;
    ySpec.vertical = true;
    if (tile.y.valid()) {
        ySpec.lo = tile.y.vMax;
        ySpec.hi = tile.y.vMin;
        ySpec.lengthPx = plot.h;
        yTicks = AxisLayout::ticks(ySpec, metrics, dpr);
    }
    if (showGrid) {
        std::vector<float> grid;
        for (const AxisTick& t : xTicks) {
            const float x = std::round(plot.x + t.posPx) + 0.5F;
            grid.insert(grid.end(), {x, plot.y, x, plot.y + plot.h});
        }
        for (const AxisTick& t : yTicks) {
            const float y = std::round(plot.y + t.posPx) + 0.5F;
            grid.insert(grid.end(), {plot.x, y, plot.x + plot.w, y});
        }
        if (!grid.empty()) {
            list.lines(grid.data(), grid.size() / 2, colours.grid, lineW);
        }
    }
    list.rectOutline(plot, colours.frame, lineW);
    const float axisLineH = metrics.lineHeightPx(TextStyleId::Axis, dpr);
    if (!yTicks.empty()) {
        AxisLayout::emit(list, ySpec, geom::keepVertical(yTicks, axisLineH, 0.0F, plot.h), tile.yAxis,
                         colours.foreground, dpr);
    }
    if (!xTicks.empty()) {
        AxisLayout::emit(list, xSpec,
                         geom::keepHorizontal(xTicks, metrics, dpr, tile.yAxis.x - plot.x,
                                              tile.bounds.x + tile.bounds.w - plot.x),
                         tile.xAxis, colours.foreground, dpr);
    }
    if (!tile.xTitleText.empty() && tile.xTitle.w > 0.0F) {
        list.textRun(tile.xTitleText, tile.xTitle.x + 0.5F * tile.xTitle.w,
                     tile.xTitle.y + 0.5F * tile.xTitle.h, TextStyleId::Axis, TextAnchor::Centre,
                     colours.foreground, tile.xTitle.w);
    }
    if (!tile.yTitleText.empty() && tile.yTitle.h > 0.0F) {
        list.textRun(tile.yTitleText, tile.yTitle.x + 0.5F * tile.yTitle.w,
                     tile.yTitle.y + 0.5F * tile.yTitle.h, TextStyleId::Axis, TextAnchor::Centre,
                     colours.foreground, tile.yTitle.h, 1);
    }
    HitResult panel;
    panel.kind = HitKind::Panel;
    panel.group = tileIndex;
    panel.trace = tile.trace;
    hits.addRect(plot, panel);
    HitResult ax;
    ax.kind = HitKind::Axis;
    ax.group = tileIndex;
    ax.handle = 0;
    if (xAxis) {
        hits.addRect(tile.xAxis, ax);
    }
    ax.handle = 1;
    hits.addRect(tile.yAxis, ax);
}

void emitBandLegend(DrawList& list, const RectPx& rect, const std::vector<analysis::Band>& bands,
                    const geom::Colours& colours, const ITextMetrics& metrics, float dpr) {
    if (!(rect.w > 0.0F) || bands.empty()) {
        return;
    }
    const float pad = 3.0F * dpr;
    const float sw = kLegendSwatchLogical * dpr;
    const float rowH = std::max(metrics.lineHeightPx(TextStyleId::ColourBand, dpr), sw + 2.0F * dpr);
    double step = 0.0;
    for (const analysis::Band& b : bands) {
        if (!b.lessMin && !b.greaterMax && b.hi > b.lo) {
            step = b.hi - b.lo;
            break;
        }
    }
    const int decimals = geom::decimalsForStep(step);
    float y = rect.y + 2.0F * pad;
    for (auto it = bands.rbegin(); it != bands.rend(); ++it) {
        if (y + rowH > rect.y + rect.h) {
            break;
        }
        list.filledRect(RectPx{rect.x + pad, y + 0.5F * (rowH - sw), sw, sw}, static_cast<Rgba>(it->colour));
        const std::string label =
            it->lessMin ? "< " + geom::formatValue(it->hi, decimals) : geom::formatValue(it->lo, decimals);
        list.textRun(label, rect.x + 2.0F * pad + sw, y + 0.5F * rowH, TextStyleId::ColourBand,
                     TextAnchor::MiddleLeft, colours.foreground, std::max(1.0F, rect.w - 3.0F * pad - sw));
        y += rowH;
    }
}

} // namespace esses::plot::detail
