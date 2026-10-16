// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Small helpers shared by the maths language sources (src/lang): diagnostic construction, ASCII character
// classes and case mapping, span arithmetic and the nearest-name search behind the "did you mean" hints.
// Internal to esses_maths.
#pragma once

#include <esses/maths/diagnostics.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::maths::lang {

inline Diagnostic makeDiag(std::string_view code, Diagnostic::Level level, Span span, std::string message,
                           std::string hint = {}) {
    Diagnostic d;
    d.code = std::string(code);
    d.level = level;
    d.span = span;
    d.message = std::move(message);
    d.hint = std::move(hint);
    return d;
}

inline Span spanOf(std::size_t begin, std::size_t end) noexcept {
    return Span{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end)};
}

inline Span join(Span a, Span b) noexcept {
    return Span{std::min(a.begin, b.begin), std::max(a.end, b.end)};
}

constexpr bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}
constexpr bool isAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool isIdentStart(char c) noexcept {
    return isAlpha(c) || c == '_';
}
constexpr bool isIdentChar(char c) noexcept {
    return isIdentStart(c) || isDigit(c);
}
constexpr char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr char upperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}
inline std::string toUpperAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = upperAscii(c);
    }
    return out;
}

// Optimal string alignment distance (Levenshtein with adjacent transpositions), capped: returns cap + 1 as
// soon as the distance must exceed `cap`.
inline std::size_t editDistance(std::string_view a, std::string_view b, std::size_t cap) {
    const std::size_t la = a.size();
    const std::size_t lb = b.size();
    if ((la > lb ? la - lb : lb - la) > cap) {
        return cap + 1;
    }
    std::vector<std::size_t> prev2(lb + 1), prev(lb + 1), cur(lb + 1);
    for (std::size_t j = 0; j <= lb; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= la; ++i) {
        cur[0] = i;
        std::size_t rowMin = cur[0];
        for (std::size_t j = 1; j <= lb; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            std::size_t v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                v = std::min(v, prev2[j - 2] + 1);
            }
            cur[j] = v;
            rowMin = std::min(rowMin, v);
        }
        if (rowMin > cap) {
            return cap + 1;
        }
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[lb];
}

// The candidate nearest to `name` (case-insensitive, distance at most max(1, size / 3), capped at 3); empty
// when none is close enough. Ties keep the first candidate.
inline std::string nearestName(std::string_view name, const std::vector<std::string>& candidates) {
    const std::string key = toLowerAscii(name);
    const std::size_t cap = std::min<std::size_t>(3, std::max<std::size_t>(1, key.size() / 3));
    std::string best;
    std::size_t bestDistance = cap + 1;
    for (const std::string& c : candidates) {
        const std::size_t d = editDistance(key, toLowerAscii(c), cap);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

} // namespace esses::maths::lang
