// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/text_metrics.hpp>

#include <cstddef>

namespace esses::plot {

namespace {

std::size_t codePoints(std::string_view utf8) noexcept {
    std::size_t n = 0;
    for (const char c : utf8) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++n;
        }
    }
    return n;
}

constexpr std::array<float, FixedTextMetrics::kStyles> kDefaultPointSizes{8.0F,  // Axis
                                                                          9.0F,  // Caption
                                                                          8.0F,  // Key
                                                                          8.0F,  // KeyValue
                                                                          8.0F,  // Band
                                                                          8.0F,  // Measurement
                                                                          9.0F,  // Banner
                                                                          16.0F, // Gauge
                                                                          8.0F,  // GaugeUnits
                                                                          8.0F,  // Table
                                                                          8.0F,  // Equation
                                                                          8.0F,  // ColourBand
                                                                          8.0F}; // TrackLabel

} // namespace

FixedTextMetrics::FixedTextMetrics() : pointSizes_(kDefaultPointSizes) {}

FixedTextMetrics::FixedTextMetrics(const std::array<float, kStyles>& pointSizes, std::uint64_t generation)
    : pointSizes_(pointSizes), generation_(generation) {}

float FixedTextMetrics::emPx(TextStyleId style, float dpr) const noexcept {
    const auto index = static_cast<std::size_t>(style);
    const float points = index < kStyles ? pointSizes_[index] : 8.0F;
    return points * dpr * 96.0F / 72.0F;
}

float FixedTextMetrics::advancePx(TextStyleId style, std::string_view utf8, float dpr) const {
    return 0.6F * emPx(style, dpr) * static_cast<float>(codePoints(utf8));
}

float FixedTextMetrics::lineHeightPx(TextStyleId style, float dpr) const {
    return 1.2F * emPx(style, dpr);
}

float FixedTextMetrics::ascentPx(TextStyleId style, float dpr) const {
    return 0.9F * emPx(style, dpr);
}

} // namespace esses::plot
