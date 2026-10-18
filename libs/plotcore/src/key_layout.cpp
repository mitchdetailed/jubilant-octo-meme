// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/key_layout.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace esses::plot {

namespace {

constexpr float kMinFraction = 0.12F;
constexpr float kMaxFraction = 0.40F;
constexpr float kMinLabelFraction = 0.30F; // the columns-aware layout's label floor

std::size_t linesOf(const KeyRow& row) noexcept {
    return std::max<std::size_t>(1, row.slotValues.size());
}

// The datum and delta cells right of the value cell; 0 without a datum, so the M1 positions are exact.
float trailingOf(const KeyGeometry& g) noexcept {
    float t = 0.0F;
    if (g.datumWidthPx > 0.0F) {
        t += g.padPx + g.datumWidthPx;
    }
    if (g.deltaWidthPx > 0.0F) {
        t += g.padPx + g.deltaWidthPx;
    }
    return t;
}

float widestOf(const ITextMetrics& metrics, TextStyleId style, std::span<const KeyValue> values, float dpr) {
    float w = 0.0F;
    for (const KeyValue& v : values) {
        w = std::max(w, metrics.advancePx(style, v.text, dpr));
    }
    return w;
}

// The factor that fits cells of total width `sum` plus `gaps` into `available` (1 when they fit).
float shrinkFactor(float available, float gaps, float sum) noexcept {
    if (!(sum > 0.0F) || gaps + sum <= available) {
        return 1.0F;
    }
    return std::max(0.0F, available - gaps) / sum;
}

} // namespace

RectPx KeyGeometry::squareRect(std::size_t row) const noexcept {
    if (row >= rowRects.size()) {
        return {};
    }
    const RectPx& r = rowRects[row];
    return RectPx{r.x + padPx, r.y + (lineHeightPx - squarePx) * 0.5F, squarePx, squarePx};
}

RectPx KeyGeometry::valueRect(std::size_t row, std::size_t line) const noexcept {
    if (row >= rowRects.size()) {
        return {};
    }
    const RectPx& r = rowRects[row];
    return RectPx{r.x + widthPx - padPx - trailingOf(*this) - valueWidthPx,
                  r.y + static_cast<float>(line) * lineHeightPx, valueWidthPx, lineHeightPx};
}

RectPx KeyGeometry::cellRect(std::size_t row, std::size_t line, KeyCell cell) const noexcept {
    if (row >= rowRects.size()) {
        return {};
    }
    const RectPx& r = rowRects[row];
    const float right = r.x + widthPx - padPx;
    const float valueY = r.y + static_cast<float>(line) * lineHeightPx;
    switch (cell) {
    case KeyCell::Value:
        return valueRect(row, line);
    case KeyCell::Datum: {
        if (!(datumWidthPx > 0.0F)) {
            return {};
        }
        const float after = deltaWidthPx > 0.0F ? padPx + deltaWidthPx : 0.0F;
        return RectPx{right - after - datumWidthPx, valueY, datumWidthPx, lineHeightPx};
    }
    case KeyCell::Delta:
        if (!(deltaWidthPx > 0.0F)) {
            return {};
        }
        return RectPx{right - deltaWidthPx, valueY, deltaWidthPx, lineHeightPx};
    case KeyCell::Min:
    case KeyCell::Max:
    case KeyCell::Avg: {
        if (!(measureWidthPx > 0.0F)) {
            return {};
        }
        const auto k = static_cast<float>(static_cast<int>(cell) - static_cast<int>(KeyCell::Min)); // 0, 1, 2
        const std::size_t valueLineCount = row < valueLines.size() ? valueLines[row] : 1U;
        const float y = r.y + static_cast<float>(valueLineCount + line) * lineHeightPx;
        const float x = right - (3.0F - k) * measureWidthPx - (2.0F - k) * padPx;
        return RectPx{x, y, measureWidthPx, lineHeightPx};
    }
    }
    return {};
}

KeyGeometry KeyLayout::layout(std::span<const KeyRow> rows, const ITextMetrics& metrics,
                              float componentWidthPx, float rowHeightPx, float dpr) {
    KeyGeometry g;
    g.padPx = 3.0F * dpr;
    g.squarePx = 7.0F * dpr;
    g.lineHeightPx = rowHeightPx;
    float labelW = 0.0F;
    float valueW = 0.0F;
    for (const KeyRow& row : rows) {
        labelW = std::max(labelW, metrics.advancePx(TextStyleId::Key, row.label, dpr));
        if (row.slotValues.empty()) {
            valueW = std::max(valueW, metrics.advancePx(TextStyleId::KeyValue, row.valueText, dpr));
        }
        for (const KeyValue& v : row.slotValues) {
            valueW = std::max(valueW, metrics.advancePx(TextStyleId::KeyValue, v.text, dpr));
        }
    }
    const float fixed = 3.0F * g.padPx + g.squarePx + (valueW > 0.0F ? g.padPx : 0.0F);
    const float natural = fixed + labelW + valueW;
    const float minW = kMinFraction * componentWidthPx;
    const float maxW = std::max(minW, kMaxFraction * componentWidthPx);
    g.widthPx = std::clamp(natural, minW, maxW);
    g.valueWidthPx = valueW;
    g.labelWidthPx = g.widthPx - fixed - valueW; // grows into spare width, shrinks (elides) first
    if (g.labelWidthPx < 0.0F) {
        g.labelWidthPx = 0.0F;
        g.valueWidthPx = std::max(0.0F, g.widthPx - fixed);
    }
    g.rowRects.reserve(rows.size());
    g.valueLines.reserve(rows.size());
    float y = 0.0F;
    for (const KeyRow& row : rows) {
        const float h = rowHeightPx * static_cast<float>(linesOf(row));
        g.rowRects.push_back(RectPx{0.0F, y, g.widthPx, h});
        g.valueLines.push_back(static_cast<std::uint32_t>(linesOf(row)));
        y += h;
    }
    return g;
}

KeyGeometry KeyLayout::layout(std::span<const KeyRow> rows, const ITextMetrics& metrics,
                              float componentWidthPx, float rowHeightPx, float dpr,
                              const KeyColumns& columns) {
    if (columns.values && !columns.datum && !columns.measurements) {
        KeyGeometry g = layout(rows, metrics, componentWidthPx, rowHeightPx, dpr);
        g.columns = columns;
        return g;
    }
    KeyGeometry g;
    g.columns = columns;
    g.padPx = 3.0F * dpr;
    g.squarePx = 7.0F * dpr;
    g.lineHeightPx = rowHeightPx;
    const bool datum = columns.values && columns.datum;
    float labelW = 0.0F;
    float valueW = 0.0F;
    float datumW = 0.0F;
    float deltaW = 0.0F;
    float measureW = 0.0F;
    for (const KeyRow& row : rows) {
        labelW = std::max(labelW, metrics.advancePx(TextStyleId::Key, row.label, dpr));
        if (columns.values) {
            if (row.slotValues.empty()) {
                valueW = std::max(valueW, metrics.advancePx(TextStyleId::KeyValue, row.valueText, dpr));
            }
            valueW = std::max(valueW, widestOf(metrics, TextStyleId::KeyValue, row.slotValues, dpr));
        }
        if (datum) {
            datumW = std::max(datumW, widestOf(metrics, TextStyleId::KeyValue, row.datumValues, dpr));
            deltaW = std::max(deltaW, widestOf(metrics, TextStyleId::KeyValue, row.deltaValues, dpr));
        }
        if (columns.measurements) {
            for (const KeyMeasurement& m : row.measurements) {
                for (const KeyValue* v : {&m.min, &m.max, &m.avg}) {
                    measureW = std::max(measureW, metrics.advancePx(TextStyleId::Measurement, v->text, dpr));
                }
            }
        }
    }
    if (datum) { // without templates the datum and delta cells take the value cell's width
        datumW = datumW > 0.0F ? datumW : valueW;
        deltaW = deltaW > 0.0F ? deltaW : valueW;
    }
    const float pad = g.padPx;
    const auto cellWidth = [pad](float w) { return w > 0.0F ? pad + w : 0.0F; };
    const float valueCells = cellWidth(valueW) + cellWidth(datumW) + cellWidth(deltaW);
    const float measureCells = measureW > 0.0F ? 3.0F * (pad + measureW) : 0.0F;
    const float cells = std::max(valueCells, measureCells);
    const float base = 3.0F * pad + g.squarePx;
    const float natural = base + labelW + cells;
    const float minW = kMinFraction * componentWidthPx;
    const float maxW = std::max(minW, kMaxFraction * componentWidthPx);
    g.widthPx = std::clamp(natural, minW, maxW);
    g.labelWidthPx = g.widthPx - base - cells; // grows into spare width, shrinks (elides) first
    // The label keeps up to kMinLabelFraction of the column (time_distance completion): below that the cells
    // shrink instead, so a datum or the measurements never elide the channel names away.
    const float minLabel = std::min(labelW, kMinLabelFraction * g.widthPx);
    if (g.labelWidthPx < minLabel) {
        g.labelWidthPx = minLabel;
        const float available = std::max(0.0F, g.widthPx - base - minLabel);
        const auto shown = [](float w) { return w > 0.0F ? 1.0F : 0.0F; };
        const float fv = shrinkFactor(available, pad * (shown(valueW) + shown(datumW) + shown(deltaW)),
                                      valueW + datumW + deltaW);
        const float fm = shrinkFactor(available, 3.0F * pad * shown(measureW), 3.0F * measureW);
        valueW *= fv;
        datumW *= fv;
        deltaW *= fv;
        measureW *= fm;
    }
    g.valueWidthPx = valueW;
    g.datumWidthPx = datumW;
    g.deltaWidthPx = deltaW;
    g.measureWidthPx = measureW;
    g.rowRects.reserve(rows.size());
    g.valueLines.reserve(rows.size());
    float y = 0.0F;
    for (const KeyRow& row : rows) {
        const std::size_t valueLineCount = linesOf(row);
        const std::size_t lines = valueLineCount + (columns.measurements ? row.measurements.size() : 0);
        const float h = rowHeightPx * static_cast<float>(lines);
        g.rowRects.push_back(RectPx{0.0F, y, g.widthPx, h});
        g.valueLines.push_back(static_cast<std::uint32_t>(valueLineCount));
        y += h;
    }
    return g;
}

void KeyLayout::emitLabels(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                           float xPx, float yPx, Rgba textColour) {
    const float labelX = xPx + 2.0F * geometry.padPx + geometry.squarePx;
    for (std::size_t i = 0; i < rows.size() && i < geometry.rowRects.size(); ++i) {
        const KeyRow& row = rows[i];
        const RectPx& r = geometry.rowRects[i];
        if (row.label.empty()) {
            continue;
        }
        list.textRun(row.label, labelX + r.x, yPx + r.y + geometry.lineHeightPx * 0.5F, TextStyleId::Key,
                     TextAnchor::MiddleLeft, row.missing ? textColour : row.colour,
                     std::max(geometry.labelWidthPx, 1.0F));
    }
}

void KeyLayout::emitValues(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                           float xPx, float yPx, Rgba textColour, Rgba background) {
    if (!(geometry.valueWidthPx > 0.0F)) {
        return;
    }
    const float trailing = trailingOf(geometry);
    for (std::size_t i = 0; i < rows.size() && i < geometry.rowRects.size(); ++i) {
        const KeyRow& row = rows[i];
        const RectPx& r = geometry.rowRects[i];
        const float right = xPx + r.x + geometry.widthPx - geometry.padPx - trailing;
        if (row.slotValues.empty()) {
            if (!row.valueText.empty()) {
                list.textRun(row.valueText, right, yPx + r.y + geometry.lineHeightPx * 0.5F,
                             TextStyleId::KeyValue, TextAnchor::MiddleRight, textColour,
                             geometry.valueWidthPx, 0, background);
            }
            continue;
        }
        for (std::size_t line = 0; line < row.slotValues.size(); ++line) {
            const KeyValue& v = row.slotValues[line];
            if (v.text.empty()) {
                continue;
            }
            list.textRun(v.text, right, yPx + r.y + (static_cast<float>(line) + 0.5F) * geometry.lineHeightPx,
                         TextStyleId::KeyValue, TextAnchor::MiddleRight, v.colour, geometry.valueWidthPx, 0,
                         background);
        }
    }
}

void KeyLayout::emitDatumValues(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                                float xPx, float yPx, Rgba textColour, Rgba background) {
    if (!(geometry.datumWidthPx > 0.0F) && !(geometry.deltaWidthPx > 0.0F)) {
        return;
    }
    const auto emitCell = [&](std::size_t row, std::size_t line, KeyCell cell, const KeyValue& v) {
        const RectPx c = geometry.cellRect(row, line, cell);
        if (!(c.w > 0.0F) || v.text.empty()) {
            return;
        }
        list.textRun(v.text, xPx + c.x + c.w, yPx + c.y + 0.5F * geometry.lineHeightPx, TextStyleId::KeyValue,
                     TextAnchor::MiddleRight, v.colour != 0 ? v.colour : textColour, c.w, 0, background);
    };
    for (std::size_t i = 0; i < rows.size() && i < geometry.rowRects.size(); ++i) {
        const KeyRow& row = rows[i];
        for (std::size_t line = 0; line < row.datumValues.size(); ++line) {
            emitCell(i, line, KeyCell::Datum, row.datumValues[line]);
        }
        for (std::size_t line = 0; line < row.deltaValues.size(); ++line) {
            emitCell(i, line, KeyCell::Delta, row.deltaValues[line]);
        }
    }
}

void KeyLayout::emitMeasurements(DrawList& list, std::span<const KeyRow> rows, const KeyGeometry& geometry,
                                 float xPx, float yPx, Rgba textColour, HitTester* hits) {
    if (!(geometry.measureWidthPx > 0.0F)) {
        return;
    }
    for (std::size_t i = 0; i < rows.size() && i < geometry.rowRects.size(); ++i) {
        const KeyRow& row = rows[i];
        for (std::size_t m = 0; m < row.measurements.size(); ++m) {
            const KeyMeasurement& meas = row.measurements[m];
            const std::array<const KeyValue*, 3> values{&meas.min, &meas.max, &meas.avg};
            for (std::size_t k = 0; k < values.size(); ++k) {
                const auto cell = static_cast<KeyCell>(static_cast<int>(KeyCell::Min) + static_cast<int>(k));
                const RectPx c = geometry.cellRect(i, m, cell);
                const KeyValue& v = *values[k];
                if (!v.text.empty()) {
                    list.textRun(v.text, xPx + c.x + c.w, yPx + c.y + 0.5F * geometry.lineHeightPx,
                                 TextStyleId::Measurement, TextAnchor::MiddleRight,
                                 v.colour != 0 ? v.colour : textColour, c.w);
                }
                if (hits != nullptr) {
                    HitResult h;
                    h.kind = HitKind::KeyMeasurement;
                    h.group = row.group;
                    h.trace = row.trace;
                    h.slot = static_cast<int>(m);
                    h.cell = static_cast<int>(k);
                    hits->addRect(RectPx{xPx + c.x, yPx + c.y, c.w, c.h}, h);
                }
            }
        }
    }
}

} // namespace esses::plot
