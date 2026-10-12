// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "font_cache.hpp"

#include <esses/qtrender/qt_text_metrics.hpp>

#include <QString>

namespace esses::qt {

namespace {

std::size_t styleIndex(plot::TextStyleId style) noexcept {
    return static_cast<std::size_t>(style) % QtTextMetrics::kStyles;
}

} // namespace

QtTextMetrics::QtTextMetrics(const project::Scheme& scheme, std::uint64_t generation)
    : generation_(generation), id_(detail::nextFontSetId()) {
    for (std::size_t i = 0; i < kStyles; ++i) {
        const project::FontSpec spec =
            scheme.font(detail::roleForStyle(static_cast<plot::TextStyleId>(i), false));
        fonts_[i] = FontDescription{spec.family, spec.pointSize, spec.weight, spec.italic, false};
    }
}

std::shared_ptr<const QtTextMetrics> QtTextMetrics::create(const project::Scheme& scheme,
                                                           std::uint64_t generation) {
    return std::make_shared<const QtTextMetrics>(scheme, generation);
}

namespace {

const detail::FontEntry& entryFor(std::uint64_t id, std::size_t index,
                                  const QtTextMetrics::FontDescription& d) {
    const auto slot = static_cast<std::uint32_t>(index);
    if (const detail::FontEntry* e = detail::findThreadFont(id, slot)) {
        return *e;
    }
    return detail::insertThreadFont(id, slot, detail::makeFont(d.family, d.pointSize, d.weight, d.italic));
}

} // namespace

float QtTextMetrics::advancePx(plot::TextStyleId style, std::string_view utf8, float dpr) const {
    const std::size_t i = styleIndex(style);
    const detail::FontEntry& e = entryFor(id_, i, fonts_[i]);
    const QString text = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    return static_cast<float>(e.metrics.horizontalAdvance(text) * static_cast<qreal>(dpr));
}

float QtTextMetrics::lineHeightPx(plot::TextStyleId style, float dpr) const {
    const std::size_t i = styleIndex(style);
    return static_cast<float>(entryFor(id_, i, fonts_[i]).metrics.height() * static_cast<qreal>(dpr));
}

float QtTextMetrics::ascentPx(plot::TextStyleId style, float dpr) const {
    const std::size_t i = styleIndex(style);
    return static_cast<float>(entryFor(id_, i, fonts_[i]).metrics.ascent() * static_cast<qreal>(dpr));
}

const QtTextMetrics::FontDescription& QtTextMetrics::description(plot::TextStyleId style) const {
    return fonts_[styleIndex(style)];
}

} // namespace esses::qt
