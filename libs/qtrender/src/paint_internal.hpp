// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal to esses_qtrender: the pieces of RasterBackend::paintList that LayerCache also needs (text
// placement for damage regions, command bounds) and the shared default font set.
#pragma once

#include "font_cache.hpp"

#include <esses/plotcore/draw_list.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <optional>

namespace esses::qt::detail {

QColor toQColor(plot::Rgba c);

// The SchemeFonts used when none is given (FontSpec defaults for every role).
const SchemeFonts& defaultSchemeFonts();

// The calling thread's cached font of a text style of `fonts`.
const FontEntry& schemeFontEntry(const SchemeFonts& fonts, plot::TextStyleId style, bool rotated);
// The same font with its point size times `scale` (TextRun::scale; 1, a non-positive or a non-finite scale
// give the style's font), cached per thread by the scale in thousandths.
const FontEntry& schemeFontEntry(const SchemeFonts& fonts, plot::TextStyleId style, bool rotated,
                                 float scale);
// The font a text run is drawn with: its style, rotation and scale.
inline const FontEntry& runFontEntry(const SchemeFonts& fonts, const plot::TextRun& run) {
    return schemeFontEntry(fonts, run.style, run.quarterTurns == 1, run.scale);
}

struct TextPlacement {
    QString text;         // after elision
    QPointF origin;       // the anchor point, logical pixels (run.x / dpr, run.y / dpr)
    QRectF box;           // the text box in the run's own frame, relative to `origin`, logical pixels
    bool rotated = false; // quarterTurns = 1: the frame is rotated -90 degrees about `origin`
    QTransform frame() const;
};

// nullopt for a run whose bytes lie outside list.text or whose anchor is not finite.
std::optional<TextPlacement> placeText(const plot::DrawList& list, const plot::TextRun& run,
                                       const FontEntry& font, qreal dpr);

struct CommandBounds {
    QRectF rect;             // device pixels, including antialiasing spill and the stroke margin
    bool wholeLayer = false; // Clear
};

// Rasterises a Static list into its layer image: RasterBackend::paintList plus the direct line rasteriser
// (span_raster.hpp) for solid strokes when the image is Format_ARGB32_Premultiplied at the list's DPR. Used
// by RasterBackend::draw and LayerCache::rasteriseStatic, i.e. every Static layer of a widget or of
// OffscreenRenderer; Dynamic and Chrome lists are painted with paintList only.
void paintStaticList(QImage& image, const plot::DrawList& list, const SchemeFonts& fonts);

// Static layer buffers handed back by LayerCache::storeStatic when a worker-rasterised image replaces them,
// reused by RasterBackend::rasteriseStatic (04 section 6.5 item 3 "QImage reuse": a zoom step allocates and
// page-faults no 4 B x device-area buffer). Process-wide, any thread, at most kSpareStaticBuffers images (the
// most recent sizes); a buffer still shared elsewhere is not kept.
inline constexpr std::size_t kSpareStaticBuffers = 2;
void recycleStaticBuffer(QImage image);
// A spare buffer of exactly `size` in Format_ARGB32_Premultiplied, or a null image.
QImage takeStaticBuffer(QSize size);

// What a command paints: strokes (Polyline, Lines, Points) widen their vertex box by (w + 2) device pixels
// (04 section 6.2: "x +- (w + 2)"), fills and text by 1. Scissor and malformed commands paint nothing.
CommandBounds commandBounds(const plot::DrawList& list, const plot::DrawCmd& cmd, const SchemeFonts& fonts);

} // namespace esses::qt::detail
