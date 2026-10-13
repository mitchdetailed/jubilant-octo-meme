// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/key_layout.hpp>

#include <algorithm>

namespace esses::plot {

namespace {

constexpr float kMinFraction = 0.12F;
constexpr float kMaxFraction = 0.40F;

std::size_t linesOf(const KeyRow& row) noexcept {
    return std::max<std::size_t>(1, row.slotValues.size());
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
    return RectPx{r.x + widthPx - padPx - valueWidthPx, r.y + static_cast<float>(line) * lineHeightPx,
                  valueWidthPx, lineHeightPx};
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
    float y = 0.0F;
    for (const KeyRow& row : rows) {
        const float h = rowHeightPx * static_cast<float>(linesOf(row));
        g.rowRects.push_back(RectPx{0.0F, y, g.widthPx, h});
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
    for (std::size_t i = 0; i < rows.size() && i < geometry.rowRects.size(); ++i) {
        const KeyRow& row = rows[i];
        const RectPx& r = geometry.rowRects[i];
        const float right = xPx + r.x + geometry.widthPx - geometry.padPx;
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

} // namespace esses::plot
