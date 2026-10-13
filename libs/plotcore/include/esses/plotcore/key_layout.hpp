// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// KeyLayout (04 sections 5.5 and 8.4): the key column of a graph: channel labels (active channel marked), the
// value at the Main cursor (value@main) per visible slot, later value@datum, delta and the measurements row.
// Column width = padding + active-square slot + the widest label advance + the value column, clamped to
// 12-40 % of the component width (labels are elided first, then values). Labels are Static; values are
// Dynamic (rebuilt on Cursor/Datum, "..." while pending); the active-channel square is Chrome (04 section
// 5.2) at squareRect().
//
// Rows: one per trace. A row with slotValues has one line per visible slot (Main first, each value in that
// slot's trace colour, 04 section 8.4 "per visible slot"); the label sits on the first line. Static builds
// pass width templates as values so the column width does not change when the cursor moves.
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/text_metrics.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace esses::plot {

struct KeyValue {
    std::string text; // formatted value; "..." pending; "" none
    Rgba colour = 0x000000FF;
};

struct KeyRow {
    std::string label;     // "Corr Speed [km/h]"; "Brake Pres (missing)" (D-013)
    std::string valueText; // formatted value@main when slotValues is empty; "..." pending; "" none
    Rgba colour = 0x000000FF;
    int group = 0;
    int trace = 0;
    bool active = false;              // the active-channel square
    bool missing = false;             // D-013 "(missing)": drawn in the secondary text colour
    std::vector<KeyValue> slotValues; // one line per visible slot (Main first)
};

struct KeyGeometry {
    float widthPx = 0.0F;
    float labelWidthPx = 0.0F;
    float valueWidthPx = 0.0F;
    std::vector<RectPx> rowRects; // one per row, device px, relative to the key column origin
    float lineHeightPx = 0.0F;    // one line of a row
    float padPx = 0.0F;
    float squarePx = 0.0F;

    // The active-channel square of a row and the value cell of one of its lines, relative to the key origin.
    RectPx squareRect(std::size_t row) const noexcept;
    RectPx valueRect(std::size_t row, std::size_t line) const noexcept;
};

class KeyLayout {
public:
    static KeyGeometry layout(std::span<const KeyRow> rows, const ITextMetrics& metrics,
                              float componentWidthPx, float rowHeightPx, float dpr);
    // Static part: the labels (row colour; the secondary colour for missing channels), elided to the label
    // column.
    static void emitLabels(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                           float xPx, float yPx, Rgba textColour);
    // Dynamic part: the value column, right-aligned, on `background` boxes.
    static void emitValues(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                           float xPx, float yPx, Rgba textColour, Rgba background);
};

} // namespace esses::plot
