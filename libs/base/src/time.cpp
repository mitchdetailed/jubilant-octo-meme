// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/time.hpp>

#include <cmath>
#include <cstdio>

namespace esses {

std::optional<Micros> microsFromSecondsF(double seconds) noexcept {
    if (!std::isfinite(seconds)) {
        return std::nullopt;
    }
    const double us = std::round(seconds * static_cast<double>(kMicrosPerSecond)); // half away from zero
    constexpr double kTwoPow63 = 9223372036854775808.0;
    if (us >= kTwoPow63 || us < -kTwoPow63) {
        return std::nullopt;
    }
    return static_cast<Micros>(us);
}

std::string formatMicros(Micros us) {
    const bool negative = us < 0;
    // Magnitude as unsigned so that kMicrosMin is representable.
    const std::uint64_t mag =
        negative ? ~static_cast<std::uint64_t>(us) + 1u : static_cast<std::uint64_t>(us);
    const auto perSecond = static_cast<std::uint64_t>(kMicrosPerSecond);
    const unsigned long long hours = mag / (3600u * perSecond);
    const unsigned long long minutes = (mag / (60u * perSecond)) % 60u;
    const unsigned long long seconds = (mag / perSecond) % 60u;
    const unsigned long long fraction = mag % perSecond;
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%s%llu:%02llu:%02llu.%06llu", negative ? "-" : "", hours,
                                minutes, seconds, fraction);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

} // namespace esses
