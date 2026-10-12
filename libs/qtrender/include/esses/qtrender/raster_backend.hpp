// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RasterBackend (04 section 6.1, D-025): plot::IRenderBackend on QPainter into QImage layers.
//  1. Layers are QImage(Format_ARGB32_Premultiplied) of device-pixel size tagged with the DPR; released
//     layers go to a small free-list keyed by size, so a zoom step at constant size allocates nothing.
//  2. Antialiasing on for Polyline/Lines wider than 1 px, off for 1 px lines, which are drawn as
//     integer-snapped QPoint arrays (each vertex snaps to the device pixel containing it); text antialiasing
//     on (grayscale: the fonts carry NoSubpixelAntialias).
//  3. Polylines in slices of <= 4,096 points (consecutive slices share a vertex; a non-finite vertex breaks
//     the line like a gap); Points with drawPoints (round dots when pointSizePx > 1, else single pixels);
//     Quads: axis-aligned quads with fillRect, others with an antialiased drawPolygon; FilledRect with
//     fillRect (edges rounded to device pixels); Scissor with setClipRect (intersected with the painter's
//     clip at entry; w = 0 restores that clip); Clear fills the list's layer rectangle with
//     CompositionMode_Source.
//  4. TextRun -> QStaticText cached per thread by (font set, style, dpr, text) (LRU 4,096), fonts from
//     SchemeFonts, anchors resolved with QFontMetricsF (box = advance x (ascent + descent); the anchor point
//     names a corner, an edge midpoint or the centre of that box), elision with elidedText (ElideRight at
//     maxWidthPx). quarterTurns = 1 rotates the run -90 degrees (reading upwards) about its anchor point; the
//     anchor applies in the run's own frame. A non-zero background fills the text box first.
//  5. compose blits SourceOver in order; readback converts to straight alpha. Widgets paint Dynamic and
//     Chrome lists directly with paintList(); compose/readback serve OffscreenRenderer, goldens and Print
//     to Clipboard.
//  6. Static layers (draw(), and LayerCache::rasteriseStatic) write solid strokes up to 8 device px
//     straight into the layer's pixels instead of stroking them with QPainter (04 section 6.5 item 3;
//     QPainter costs ~100 ms at DPR 1 and seconds at DPR 2 for 20 noisy M4 traces): the same picture, see
//     src/span_raster.hpp. paintList() (Dynamic, Chrome, any painter) always uses QPainter.
// Coordinates: every DrawList coordinate is a device pixel of a layer of list.dpr; paintList maps them onto
// the painter's logical coordinate system (scale 1 / list.dpr), so the same call paints a DPR-tagged QImage
// and a widget of that DPR 1:1 in device pixels.
// GUI thread (or the calling thread of OffscreenRenderer) only, except the static rasteriseStatic(), which a
// worker calls (04 section 6.3 step 2 "worker build + raster"; fonts are per-thread, 04 section 5.4).
#pragma once

#include <esses/plotcore/render_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QImage>
#include <QPainter>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::qt {

class RasterBackend final : public plot::IRenderBackend {
public:
    // `fonts` nullptr: the default SchemeFonts (FontSpec defaults for every role).
    RasterBackend(std::shared_ptr<const SchemeFonts> fonts,
                  std::shared_ptr<const plot::ITextMetrics> metrics);
    ~RasterBackend() override;
    RasterBackend(const RasterBackend&) = delete;
    RasterBackend& operator=(const RasterBackend&) = delete;

    // A layer begun while another is open releases the open one first.
    void beginLayer(plot::LayerKind kind, int widthPx, int heightPx, float dpr, plot::Rgba clear) override;
    void draw(const plot::DrawList& list) override;
    plot::LayerHandle endLayer() override;
    void compose(std::span<const plot::LayerHandle> inOrder, plot::LayerHandle target) override;
    plot::Bitmap readback(plot::LayerHandle layer) override;
    void release(plot::LayerHandle layer) override;
    std::shared_ptr<const plot::ITextMetrics> textMetrics() const override { return metrics_; }

    // The layer's image (implicitly shared; null for an unknown handle).
    QImage image(plot::LayerHandle layer) const;
    std::size_t liveLayers() const noexcept { return layers_.size(); }
    std::size_t freeLayers() const noexcept { return free_.size(); }
    static constexpr std::size_t kMaxFreeLayers = 4;

    // Qt-level entry, not part of IRenderBackend: paints a list (Static, Dynamic or Chrome) onto a painter
    // whose logical coordinate system is the list's layer at list.dpr (a DPR-tagged QImage or a widget).
    static void paintList(QPainter& painter, const plot::DrawList& list, const SchemeFonts& fonts);

    // Added in the M1 integration (AC-M1-02, 04 section 6.3 step 2 "worker build + raster"): rasterises a
    // Static list into a DPR-tagged Format_ARGB32_Premultiplied image of the list's size (a spare buffer of
    // that size is reused, see LayerCache::storeStatic), cleared to `clear` unless the list starts with a
    // Clear. On a worker of a TaskPool (`pool`, else the calling worker's pool,
    // TaskPool::currentWorkerPool()) the image is painted in vertical strips in parallel (sub-tasks joined
    // with Rule W): each strip paints, in list order and in the layer's own coordinates, the commands whose
    // bounds reach its columns, clipped to them, and walks only the part of an opaque x-monotonic trace
    // polyline that reaches it, so the picture equals one LayerCache::rasteriseStatic pass pixel for pixel on
    // every platform. Strip edges lie on whole logical pixels: a non-integer DPR, a NoBlock thread or no pool
    // paint one strip. Null for an empty list size.
    static QImage rasteriseStatic(const plot::DrawList& list, const SchemeFonts& fonts, plot::Rgba clear = 0,
                                  TaskPool* pool = nullptr);
    static constexpr int kMaxStaticStrips = 32;
    static constexpr int kMinStripColumns = 64;

private:
    const SchemeFonts& fonts() const;

    std::shared_ptr<const SchemeFonts> fonts_;
    std::shared_ptr<const plot::ITextMetrics> metrics_;
    std::map<std::uint32_t, QImage> layers_;
    std::vector<QImage> free_;
    std::uint32_t nextId_ = 1;
    std::uint32_t current_ = 0; // the layer between beginLayer and endLayer
};

} // namespace esses::qt
