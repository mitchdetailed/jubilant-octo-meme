// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Time base (D-008, 02 section 1): Micros is int64 microseconds since the data layer's origin.
#pragma once

#include <esses/base/config.hpp>

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace esses {

using Micros = std::int64_t;    // microseconds since the data layer's origin
using SampleIdx = std::int64_t; // sample index within a series (negative before t0)

struct UtcInstant { // optional absolute anchor of Micros 0 per layer
    std::int64_t unixUs = 0;
    friend constexpr auto operator<=>(const UtcInstant&, const UtcInstant&) = default;
};

inline constexpr Micros kMicrosPerMilli = 1'000;
inline constexpr Micros kMicrosPerSecond = 1'000'000;
inline constexpr Micros kMicrosPerMinute = 60 * kMicrosPerSecond;
inline constexpr Micros kMicrosPerHour = 60 * kMicrosPerMinute;
inline constexpr Micros kMicrosPerDay = 24 * kMicrosPerHour;
inline constexpr Micros kMicrosMax = std::numeric_limits<Micros>::max();
inline constexpr Micros kMicrosMin = std::numeric_limits<Micros>::min();

// a + b, clamped to [kMicrosMin, kMicrosMax].
constexpr Micros saturatingAdd(Micros a, Micros b) noexcept {
    if (b > 0 && a > kMicrosMax - b) {
        return kMicrosMax;
    }
    if (b < 0 && a < kMicrosMin - b) {
        return kMicrosMin;
    }
    return a + b;
}

// a - b, clamped to [kMicrosMin, kMicrosMax].
constexpr Micros saturatingSub(Micros a, Micros b) noexcept {
    if (b < 0 && a > kMicrosMax + b) {
        return kMicrosMax;
    }
    if (b > 0 && a < kMicrosMin + b) {
        return kMicrosMin;
    }
    return a - b;
}

// value * factor, clamped. factor > 0.
constexpr Micros saturatingScale(std::int64_t value, std::int64_t factor) noexcept {
    if (value > 0 && value > kMicrosMax / factor) {
        return kMicrosMax;
    }
    if (value < 0 && value < kMicrosMin / factor) {
        return kMicrosMin;
    }
    return value * factor;
}

constexpr Micros microsFromSeconds(std::int64_t seconds) noexcept {
    return saturatingScale(seconds, kMicrosPerSecond);
}
constexpr Micros microsFromMillis(std::int64_t millis) noexcept {
    return saturatingScale(millis, kMicrosPerMilli);
}
constexpr double microsToSeconds(Micros us) noexcept {
    return static_cast<double>(us) / static_cast<double>(kMicrosPerSecond);
}

// Seconds as a double to Micros, rounded half away from zero. nullopt for NaN, infinities and values outside
// the Micros range. Display and parsing helper only: sample times are always derived from Rate or an I64
// column.
std::optional<Micros> microsFromSecondsF(double seconds) noexcept;

// "[-]H:MM:SS.ffffff" (hours are not wrapped at 24). Developer-facing; UI formatting lives above layer 3.
std::string formatMicros(Micros us);

} // namespace esses
