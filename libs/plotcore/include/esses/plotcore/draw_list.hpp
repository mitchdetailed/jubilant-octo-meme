// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The DrawList IR (04 sections 5.1-5.2, D-025): what builders emit and backends rasterise. All coordinates
// are device pixels; builders receive a LayoutContext and multiply logical sizes by dpr themselves (04
// section 6.6). Scissor nests by replacement (w = 0 restores the full layer); Quads consume four vertices
// each with the command colour; Clear fills the layer; text is never converted to geometry here (TextRun:
// UTF-8 bytes in `text`, a style id and an anchor; glyphs exist only in esses_qtrender). Layers: Static
// (worker build + GUI raster into one cached image), Dynamic (cursor, datum, key values; GUI-built, painted
// directly), Chrome (caption, focus border, pending stripe, banner; GUI-built). The JSON form (toJson) is the
// DrawList golden of AC-M1-04 that runs on every OS (08 section 6.4, D-127).
#pragma once

#include <esses/project/rgba.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esses::plot {

using Rgba = project::Rgba; // 0xRRGGBBAA, persisted as "#rrggbbaa"

enum class LayerKind : std::uint8_t { Static, Dynamic, Chrome };
enum class CmdKind : std::uint8_t { Clear, Scissor, Polyline, Points, Lines, Quads, FilledRect, TextRun };
enum class DashId : std::uint8_t { Solid, Dash, Dot, DashDot };
enum class TextStyleId : std::uint8_t {
    Axis,
    Caption,
    Key,
    KeyValue,
    Band,
    Measurement,
    Banner,
    Gauge,
    GaugeUnits,
    Table,
    Equation,
    ColourBand,
    TrackLabel
};
enum class TextAnchor : std::uint8_t {
    TopLeft,
    TopCentre,
    TopRight,
    MiddleLeft,
    Centre,
    MiddleRight,
    BottomLeft,
    BottomCentre,
    BottomRight
};

struct RectPx {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    friend bool operator==(const RectPx&, const RectPx&) = default;
};

struct LayoutContext {
    int widthPx = 0;
    int heightPx = 0;
    float dpr = 1.0F;
};

struct TextRun {
    std::uint32_t byteOffset = 0; // into DrawList::text (UTF-8)
    std::uint32_t byteLen = 0;
    float x = 0.0F; // device px anchor point
    float y = 0.0F;
    TextStyleId style = TextStyleId::Axis;
    TextAnchor anchor = TextAnchor::TopLeft;
    std::uint8_t quarterTurns = 0; // 0 or 1 (vertical axis titles)
    Rgba colour = 0x000000FF;
    float maxWidthPx = 0.0F; // > 0: elide with an ellipsis
    Rgba background = 0;     // 0: none
    // Added in M3 (the numeric gauge's dynamic font, 04 section 16 item 1; D-198): the style's point size
    // times `scale` (> 0; 1 = the style's size). Builders measure a scaled run as the style's metrics times
    // scale.
    float scale = 1.0F;
    friend bool operator==(const TextRun&, const TextRun&) = default;
};

struct DrawCmd {
    CmdKind kind = CmdKind::Clear;
    std::uint32_t vertexOffset = 0; // Polyline/Points/Lines/Quads: index of the first vertex (pairs in verts)
    std::uint32_t vertexCount = 0;
    Rgba colour = 0x000000FF;
    float widthPx = 1.0F;
    DashId dash = DashId::Solid;
    float pointSizePx = 0.0F;
    RectPx rect;                 // Scissor, FilledRect
    std::uint32_t textIndex = 0; // TextRun
    friend bool operator==(const DrawCmd&, const DrawCmd&) = default;
};

struct DrawList {
    int widthPx = 0;
    int heightPx = 0;
    float dpr = 1.0F;
    std::vector<DrawCmd> cmds;
    std::vector<float> verts; // interleaved x, y device px
    std::vector<TextRun> texts;
    std::string text;

    // Builder helpers: append a command and its data, returning the command index.
    std::size_t clear(Rgba colour);
    std::size_t scissor(RectPx rect); // w == 0 restores the full layer
    std::size_t filledRect(RectPx rect, Rgba colour);
    std::size_t polyline(const float* xy, std::size_t vertexCount, Rgba colour, float lineWidthPx,
                         DashId dash = DashId::Solid);
    // Independent segments: vertexCount is even, vertices (2k, 2k+1) form segment k.
    std::size_t lines(const float* xy, std::size_t vertexCount, Rgba colour, float lineWidthPx,
                      DashId dash = DashId::Solid);
    std::size_t points(const float* xy, std::size_t vertexCount, Rgba colour, float pointSizePx);
    // The outline of `rect` as a closed polyline of width lineWidthPx drawn inside the rectangle.
    std::size_t rectOutline(RectPx rect, Rgba colour, float lineWidthPx);
    std::size_t textRun(std::string_view utf8, float x, float y, TextStyleId style, TextAnchor anchor,
                        Rgba colour, float maxWidthPx = 0.0F, std::uint8_t quarterTurns = 0,
                        Rgba background = 0);
    // Sub-list placement (panels): appends `other` translated by (dxPx, dyPx).
    void append(const DrawList& other, float dxPx, float dyPx);
    bool empty() const noexcept { return cmds.empty(); }
    std::string_view textOf(const TextRun& run) const noexcept {
        return std::string_view(text).substr(run.byteOffset, run.byteLen);
    }

    // The DrawList golden form (AC-M1-04, 08 section 6.4): deterministic JSON, one command per line, in
    // command order. Coordinates and sizes are written with at most two decimals (fixed rounding, trailing
    // zeros dropped, -0 as 0), so last-bit floating-point differences between compilers do not change a
    // golden; colours as "#rrggbb" / "#rrggbbaa"; enum values by name; text runs carry their UTF-8 text (and
    // "scale" only when it is not 1, so the M1 goldens are unchanged).
    std::string toJson() const;

    friend bool operator==(const DrawList&, const DrawList&) = default;
};

} // namespace esses::plot
