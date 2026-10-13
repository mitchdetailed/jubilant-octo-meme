// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal to esses_qtrender: the Static-layer line rasteriser (04 section 6.5 item 3, the remedies before
// GL: integer-snapped 1 px polylines, no per-call stroking). QPainter strokes a dense M4 polyline as one path
// whose outline crosses every scanline of the trace's vertical extent; for noisy data (the plot_bench
// workload of 04 section 6.4) that costs ~100 ms for 20 traces x 3,800 px at DPR 1 and seconds for the
// antialiased 2 px traces of DPR 2. This writes solid polylines straight into the Format_ARGB32_Premultiplied
// layer instead:
//   * width <= 1 device px: aliased; each vertex snaps to the device pixel containing it; segments are
//     Bresenham lines that exclude their end pixel (the next segment starts there), the polyline's last pixel
//     is drawn; consecutive vertical segments in one pixel column merge into one span. Every pixel of one
//     polyline is written once, so translucent strokes blend once (QPainter's single-path behaviour).
//   * width > 1: antialiased runs; a steep segment covers, on every pixel row whose centre lies in its
//     half-open y range, the horizontal extent w * length / |dy| centred on the line, a shallow one the
//     vertical extent per pixel column; edge pixels get their exact coverage of that run. Flat ends, no join
//     geometry (consecutive half-open segments abut).
// Translucent colours blend SourceOver in premultiplied space. Every write is clipped to `clip`.
#pragma once

#include <esses/project/rgba.hpp>

#include <QPoint>
#include <QRect>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>

namespace esses::qt::detail {

class SpanRaster {
public:
    // `bits`/`bytesPerLine` of a Format_ARGB32_Premultiplied image; `clip` inside the image.
    SpanRaster(uchar* bits, qsizetype bytesPerLine, QRect clip, project::Rgba colour);

    // Vertices [0, n) of `xy` (interleaved device px, all finite).
    void aliasedPolyline(const float* xy, std::size_t n);
    void thickPolyline(const float* xy, std::size_t n, float widthPx);
    // One independent segment (Lines): both end pixels drawn.
    void aliasedSegment(float x0, float y0, float x1, float y1);
    void thickSegment(float x0, float y0, float x1, float y1, float widthPx);

    // Widths this rasteriser serves; wider strokes go through QPainter.
    static constexpr float kMaxWidthPx = 8.0F;

private:
    void line(QPoint a, QPoint b, bool includeStart, bool includeEnd);
    void vspan(int x, int y0, int y1); // inclusive, any order
    void hspan(int y, int x0, int x1);
    void blend(int x, int y, unsigned coverage);          // coverage 0..255, unclipped
    void coverRow(int y, double a, double b);             // horizontal coverage run [a, b) on row y
    void coverColumn(int x, double a, double b);          // vertical coverage run [a, b) on column x
    void verticalRun(double a, double b, int y0, int y1); // coverRow(y, a, b) for y0..y1
    void thick(double x0, double y0, double x1, double y1, double widthPx);

    uchar* bits_;
    qsizetype bpl_;
    QRect clip_;
    quint32 premul_;
    bool opaque_;
};

} // namespace esses::qt::detail
