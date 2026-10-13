// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IRenderBackend (04 section 5.3, D-025): the seam between the Qt-free plot core and a rasteriser. The MVP
// ships esses_qtrender's RasterBackend (raster image layers); a GlBackend may follow behind the same
// interface only when the escalation rule of 04 section 6.5 fires. compose/readback serve OffscreenRenderer,
// goldens, print and clipboard; widgets paint Dynamic and Chrome lists directly (04 section 6.2).
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/text_metrics.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace esses::plot {

struct LayerHandle {
    std::uint32_t id = 0; // 0: none
    friend bool operator==(const LayerHandle&, const LayerHandle&) = default;
};

struct Bitmap { // straight alpha, row-major, 4 bytes per pixel (r, g, b, a)
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual void beginLayer(LayerKind kind, int widthPx, int heightPx, float dpr, Rgba clear) = 0;
    virtual void draw(const DrawList& list) = 0; // may be called several times per layer
    virtual LayerHandle endLayer() = 0;
    virtual void compose(std::span<const LayerHandle> inOrder, LayerHandle target) = 0;
    virtual Bitmap readback(LayerHandle layer) = 0; // goldens, clipboard, print
    virtual void release(LayerHandle layer) = 0;
    virtual std::shared_ptr<const ITextMetrics>
    textMetrics() const = 0; // immutable snapshot for the current scheme

protected:
    IRenderBackend() = default;
    IRenderBackend(const IRenderBackend&) = default;
    IRenderBackend& operator=(const IRenderBackend&) = default;
};

} // namespace esses::plot
