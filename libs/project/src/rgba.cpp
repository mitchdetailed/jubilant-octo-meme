// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/project/rgba.hpp>

#include <array>

namespace esses::project {

namespace {

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::optional<Rgba> parseRgba(std::string_view text) noexcept {
    if (text.size() != 7 && text.size() != 9) {
        return std::nullopt;
    }
    if (text.front() != '#') {
        return std::nullopt;
    }
    Rgba value = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const int v = hexValue(text[i]);
        if (v < 0) {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<Rgba>(v);
    }
    if (text.size() == 7) {
        value = (value << 8) | 0xFFU;
    }
    return value;
}

std::string formatRgba(Rgba c) {
    static constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const int bytes = alpha(c) == 0xFF ? 3 : 4;
    std::string out = "#";
    for (int i = 0; i < bytes; ++i) {
        const auto byte = static_cast<std::uint8_t>(c >> (24 - 8 * i));
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

} // namespace esses::project
