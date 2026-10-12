// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Rgba: a colour as 0xRRGGBBAA, persisted as "#rrggbbaa" or "#rrggbb" (alpha ff) (04 section 5.1, 06 section
// 16, 02 section 6.9 scheme-1). esses_plotcore's plot::Rgba is the same type (an alias of this one).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace esses::project {

using Rgba = std::uint32_t;

constexpr Rgba rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 0xFF) noexcept {
    return (Rgba{r} << 24) | (Rgba{g} << 16) | (Rgba{b} << 8) | Rgba{a};
}
constexpr std::uint8_t red(Rgba c) noexcept {
    return static_cast<std::uint8_t>(c >> 24);
}
constexpr std::uint8_t green(Rgba c) noexcept {
    return static_cast<std::uint8_t>(c >> 16);
}
constexpr std::uint8_t blue(Rgba c) noexcept {
    return static_cast<std::uint8_t>(c >> 8);
}
constexpr std::uint8_t alpha(Rgba c) noexcept {
    return static_cast<std::uint8_t>(c);
}

// "#rrggbb" or "#rrggbbaa", case-insensitive; nullopt otherwise.
std::optional<Rgba> parseRgba(std::string_view text) noexcept;
// "#rrggbb" when alpha is ff, else "#rrggbbaa" (lower case).
std::string formatRgba(Rgba c);

} // namespace esses::project
