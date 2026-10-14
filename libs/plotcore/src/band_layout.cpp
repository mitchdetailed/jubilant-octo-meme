// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BandLayout (04 sections 8.1 and 8.11): the range, section and marker bands stacked from the top of the
// area, each round(14 x dpr) device px, omitted when hidden. Every band is filled with the band background,
// its cells and markers are clipped to it (Scissor), and a frame line closes it at the bottom. Cells draw
// their edges as ticks (the outing graph's lap boundaries, 04 section 9 item 5); range labels are elided to
// the cell, section labels appear only when the cell is wider than the label. Later cells and markers paint
// over earlier ones and win hits (HitTester order).

#include "geometry/geometry_common.hpp"

#include <esses/plotcore/band_layout.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace esses::plot {

namespace {

struct Span {
    float x0 = 0.0F;
    float x1 = 0.0F;
    bool empty() const noexcept { return !(x1 > x0); }
};

// The cell's span in device px clipped to the band.
Span clippedSpan(const BandCell& cell, const RectPx& band) {
    float a = band.x + cell.x0;
    float b = band.x + cell.x1;
    if (b < a) {
        std::swap(a, b);
    }
    return Span{std::max(a, band.x), std::min(b, band.x + band.w)};
}

Rgba sectionColour(const BandCell& cell, const BandColours& colours) {
    if (cell.colour != 0) {
        return cell.colour;
    }
    if (cell.category == "corner") {
        return colours.sectionCorner;
    }
    if (cell.category == "straight") {
        return colours.sectionStraight;
    }
    return colours.background;
}

void bandFrame(DrawList& list, const RectPx& band, const BandColours& colours, float lineW) {
    const float y = band.y + band.h - 0.5F * lineW;
    const std::array<float, 4> line{band.x, y, band.x + band.w, y};
    list.lines(line.data(), 2, colours.frame, lineW);
}

// Cells of the range or section band: fill, edge ticks, label, hit region.
void emitCells(DrawList& list, const std::vector<BandCell>& cells, const RectPx& band, bool sections,
               const ITextMetrics& metrics, const BandColours& colours, float dpr, HitTester* hits) {
    const float lineW = std::max(1.0F, std::round(dpr));
    const float pad = 3.0F * dpr;
    const float minLabel = metrics.advancePx(TextStyleId::Band, "\xE2\x80\xA6", dpr); // room for "..."
    std::vector<float> edges;
    for (const BandCell& cell : cells) {
        const Span s = clippedSpan(cell, band);
        if (s.empty()) {
            continue;
        }
        const Rgba fill =
            sections ? sectionColour(cell, colours) : (cell.colour != 0 ? cell.colour : colours.background);
        list.filledRect(RectPx{s.x0, band.y, s.x1 - s.x0, band.h}, fill);
        for (const float edge : {band.x + std::min(cell.x0, cell.x1), band.x + std::max(cell.x0, cell.x1)}) {
            const float x = std::round(edge) + 0.5F;
            const bool repeated = !edges.empty() && edges[edges.size() - 4] == x; // a shared cell boundary
            if (edge >= band.x && edge <= band.x + band.w && !repeated) {
                edges.insert(edges.end(), {x, band.y, x, band.y + band.h});
            }
        }
        const float room = s.x1 - s.x0 - 2.0F * pad;
        if (!cell.label.empty() && room > 0.0F) {
            const float advance = metrics.advancePx(TextStyleId::Band, cell.label, dpr);
            const bool fits = advance <= room;
            if (fits || (!sections && room >= minLabel)) {
                list.textRun(cell.label, 0.5F * (s.x0 + s.x1), band.y + 0.5F * band.h, TextStyleId::Band,
                             TextAnchor::Centre, colours.text, fits ? 0.0F : room);
            }
        }
        if (hits != nullptr) {
            HitResult h;
            h.kind = sections ? HitKind::Section : HitKind::Band;
            h.slot = cell.slot;
            if (sections) {
                h.section = cell.index;
            } else {
                h.band = cell.index;
            }
            hits->addRect(RectPx{s.x0, band.y, s.x1 - s.x0, band.h}, h);
        }
    }
    if (!edges.empty()) {
        list.lines(edges.data(), edges.size() / 2, colours.frame, lineW);
    }
}

void emitMarkers(DrawList& list, const std::vector<BandMarker>& markers, const RectPx& band,
                 const BandColours& colours, float dpr, HitTester* hits) {
    const float lineW = std::max(1.0F, std::round(dpr));
    const float half = 3.0F * dpr;  // annotation half width, beacon hit half width
    const float tri = 4.0F * dpr;   // device-event triangle half width
    const float inset = 2.0F * dpr; // vertical inset of rectangles and triangles
    for (const BandMarker& m : markers) {
        const float x = band.x + m.x;
        if (!std::isfinite(x) || x < band.x - tri || x > band.x + band.w + tri) {
            continue;
        }
        HitResult h;
        h.markerId = m.id;
        RectPx hit;
        switch (m.kind) {
        case BandMarkerKind::Beacon: {
            const float sx = std::round(x) + 0.5F;
            const std::array<float, 4> tick{sx, band.y, sx, band.y + band.h};
            list.lines(tick.data(), 2, m.colour != 0 ? m.colour : colours.marker, lineW);
            h.kind = HitKind::Marker;
            hit = RectPx{sx - half, band.y, 2.0F * half, band.h};
            break;
        }
        case BandMarkerKind::Annotation: {
            const RectPx r{x - half, band.y + inset, 2.0F * half, std::max(1.0F, band.h - 2.0F * inset)};
            list.filledRect(r, m.colour != 0 ? m.colour : colours.annotation);
            h.kind = HitKind::Annotation;
            hit = r;
            break;
        }
        case BandMarkerKind::DeviceEvent: {
            const float top = band.y + inset;
            const float bottom = band.y + band.h - inset;
            const std::array<float, 8> quad{x - tri, top, x + tri, top, x, bottom, x, bottom};
            geom::quads(list, quad.data(), 4, m.colour != 0 ? m.colour : colours.deviceEvent);
            h.kind = HitKind::Marker;
            hit = RectPx{x - tri, band.y, 2.0F * tri, band.h};
            break;
        }
        }
        if (hits != nullptr) {
            hits->addRect(hit, h);
        }
    }
}

} // namespace

BandGeometry BandLayout::layout(const BandLayoutInput& input, RectPx area, float dpr) {
    BandGeometry g;
    const float h = std::round(kBandHeight * (dpr > 0.0F ? dpr : 1.0F));
    float y = area.y;
    const auto take = [&](bool show, RectPx& rect) {
        rect = RectPx{area.x, y, area.w, show ? h : 0.0F};
        y += rect.h;
    };
    take(input.showRanges, g.range);
    take(input.showSections, g.section);
    take(input.showMarkers, g.marker);
    g.heightPx = y - area.y;
    return g;
}

void BandLayout::emit(DrawList& list, const BandLayoutInput& input, const BandGeometry& geometry,
                      const ITextMetrics& metrics, const BandColours& colours, float dpr, HitTester* hits) {
    const float d = dpr > 0.0F ? dpr : 1.0F;
    const float lineW = std::max(1.0F, std::round(d));
    const auto shown = [](const RectPx& r) { return r.h > 0.0F && r.w > 0.0F; };
    if (shown(geometry.range)) {
        list.filledRect(geometry.range, colours.background);
        list.scissor(geometry.range);
        emitCells(list, input.ranges, geometry.range, false, metrics, colours, d, hits);
        list.scissor(RectPx{});
        bandFrame(list, geometry.range, colours, lineW);
    }
    if (shown(geometry.section)) {
        list.filledRect(geometry.section, colours.background);
        list.scissor(geometry.section);
        emitCells(list, input.sections, geometry.section, true, metrics, colours, d, hits);
        list.scissor(RectPx{});
        bandFrame(list, geometry.section, colours, lineW);
    }
    if (shown(geometry.marker)) {
        list.filledRect(geometry.marker, colours.background);
        list.scissor(geometry.marker);
        emitMarkers(list, input.markers, geometry.marker, colours, d, hits);
        list.scissor(RectPx{});
        bandFrame(list, geometry.marker, colours, lineW);
    }
}

} // namespace esses::plot
