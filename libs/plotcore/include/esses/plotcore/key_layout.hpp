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
//
// M3 completion (07 section 6.1; 04 section 8.4; src/key_layout.cpp): KeyColumns selects the columns. With a
// datum (D) a value line holds value@main, value@datum and delta, right-aligned in that order; measurements
// (M) add one line per measurement (per visible slot) under the row's value lines, holding min, max and avg
// over the visible span right-aligned (Measurement text style). Hovering min/max draws a line at its
// position, double-click moves the cursor there; the cell positions come back through the HitTester as
// HitKind::KeyMeasurement. The width rule adds the shown cells (each after a pad) to the label column; the
// label keeps up to 30 % of the clamped column, below that the cells shrink proportionally (the time_distance
// completion: a datum never elides the channel names away). With the default KeyColumns the
// column-aware layout is the M1 layout exactly, so the value cell, emitValues and the M1 DrawLists do not
// move.
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/text_metrics.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace esses::plot {

struct KeyValue {
    std::string text; // formatted value; "..." pending; "" none
    Rgba colour = 0x000000FF;
};

// M3: one measurement line of a row (min, max, avg over the visible span); minXPx/maxXPx are the device-px
// x positions of the extremes in the trace area (NaN when unknown), for the hover line.
struct KeyMeasurement {
    KeyValue min;
    KeyValue max;
    KeyValue avg;
    float minXPx = 0.0F;
    float maxXPx = 0.0F;
    bool positionsKnown = false;
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
    // M3: with a datum, value@datum and delta per line (same order as slotValues); with measurements, one
    // entry per visible slot.
    std::vector<KeyValue> datumValues;
    std::vector<KeyValue> deltaValues;
    std::vector<KeyMeasurement> measurements;
};

// Which columns the key shows (04 section 8.4 table).
struct KeyColumns {
    bool values = true;        // key.values; value@main (value@cursor without a datum)
    bool datum = false;        // D: value@datum and delta
    bool measurements = false; // M: the min/max/avg row per trace and slot
};

enum class KeyCell : std::uint8_t { Value, Datum, Delta, Min, Max, Avg };

struct KeyGeometry {
    float widthPx = 0.0F;
    float labelWidthPx = 0.0F;
    float valueWidthPx = 0.0F;
    std::vector<RectPx> rowRects; // one per row, device px, relative to the key column origin
    float lineHeightPx = 0.0F;    // one line of a row
    float padPx = 0.0F;
    float squarePx = 0.0F;

    // M3 (zero unless the columns are shown)
    float datumWidthPx = 0.0F;
    float deltaWidthPx = 0.0F;
    float measureWidthPx = 0.0F; // each of min, max, avg
    KeyColumns columns;
    std::vector<std::uint32_t> valueLines; // per row: its value lines (the measurement lines follow them)

    // The active-channel square of a row and the value cell of one of its lines, relative to the key origin.
    RectPx squareRect(std::size_t row) const noexcept;
    RectPx valueRect(std::size_t row, std::size_t line) const noexcept;
    // Any cell of a row's line, relative to the key origin; w == 0 when the column is not shown. Value, Datum
    // and Delta take a value line; Min, Max and Avg a measurement line (0 = the row's first). valueRect
    // equals cellRect(..., Value).
    RectPx cellRect(std::size_t row, std::size_t line, KeyCell cell) const noexcept;
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

    // The layout with the datum and measurement columns (04 section 8.4); with the default KeyColumns it is
    // the M1 layout above.
    static KeyGeometry layout(std::span<const KeyRow> rows, const ITextMetrics& metrics,
                              float componentWidthPx, float rowHeightPx, float dpr,
                              const KeyColumns& columns);
    // Dynamic: value@datum and delta per line (right-aligned on `background` boxes, each value's colour).
    static void emitDatumValues(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                                float xPx, float yPx, Rgba textColour, Rgba background);
    // Static (rebuilt with the span): the measurement lines; adds HitKind::KeyMeasurement regions to `hits`
    // (cell 0 min, 1 max, 2 avg; group/trace from the row, slot = the measurement's index) when not null. A
    // value with colour 0 is drawn in textColour.
    static void emitMeasurements(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                                 float xPx, float yPx, Rgba textColour, HitTester* hits);
};

} // namespace esses::plot
