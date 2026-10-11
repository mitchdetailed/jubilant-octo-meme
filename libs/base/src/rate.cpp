// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/int128.hpp>
#include <esses/base/rate.hpp>

#include <limits>
#include <numeric>

namespace esses {

namespace {
constexpr std::uint64_t kMicrosPerSecondU = 1'000'000ull;
}

Micros Rate::timeAt(SampleIdx i, Micros t0) const noexcept {
    if (!valid()) {
        return t0;
    }
    const std::uint64_t d = kMicrosPerSecondU * den; // 64-bit: never the wrapping 32-bit 1'000'000 * den
    const detail::S128 x = detail::S128::fromInt64(i).mulU64(2u * d).add(detail::S128::fromUint64(num));
    return saturatingAdd(t0, x.floorDivU64(2ull * num));
}

SampleIdx Rate::indexAt(Micros t, Micros t0) const noexcept {
    if (!valid()) {
        return 0;
    }
    const std::uint64_t d = kMicrosPerSecondU * den;
    const detail::S128 delta = detail::S128::fromInt64(t).sub(detail::S128::fromInt64(t0)); // exact, 65 bits
    const detail::S128 x =
        delta.mulU64(2u).add(detail::S128::fromUint64(1u)).mulU64(num).sub(detail::S128::fromUint64(1u));
    return x.floorDivU64(2u * d);
}

Rate Rate::fromPeriodUs(std::uint64_t periodUs) noexcept {
    if (periodUs == 0) {
        return Rate{};
    }
    const std::uint64_t g = std::gcd(kMicrosPerSecondU, periodUs);
    const std::uint64_t n = kMicrosPerSecondU / g;
    const std::uint64_t dd = periodUs / g;
    if (dd > std::numeric_limits<std::uint32_t>::max()) {
        return Rate{};
    }
    return Rate{static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(dd)};
}

Rate Rate::normalised() const noexcept {
    if (!valid()) {
        return Rate{};
    }
    const std::uint32_t g = std::gcd(num, den);
    return Rate{num / g, den / g};
}

std::string Rate::toString() const {
    if (!valid()) {
        return "no rate";
    }
    const Rate r = normalised();
    if (r.den == 1) {
        return std::to_string(r.num) + " Hz";
    }
    return std::to_string(r.num) + "/" + std::to_string(r.den) + " Hz";
}

} // namespace esses
