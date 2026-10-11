// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Rational sample rate (D-008 as amended by D-129, 02 section 1 behaviours 1-2).
//
// With D = 1'000'000 * den (formed in 64 bits):
//   timeAt(i)  = t0 + floorDiv(2*i*D + num, 2*num)        round-half-up of i * period (D-008's llround_div)
//   indexAt(t) = floorDiv(num*(2*(t - t0) + 1) - 1, 2*D)  the largest i with timeAt(i) <= t
// Every product is formed in 128 bits and every division floors toward -infinity, so t < t0 maps to negative
// indices and there is no drift at any rate or length. indexAt(timeAt(i)) == i whenever the period is at
// least one microsecond (num <= 1'000'000 * den); faster rates map several samples onto one microsecond and
// indexAt returns the last of them. Results outside the int64 range saturate.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/time.hpp>

#include <compare>
#include <cstdint>
#include <string>

namespace esses {

struct Rate {              // rational hertz
    std::uint32_t num = 0; // num/den Hz; num == 0 means "no rate" (timestamped series)
    std::uint32_t den = 1;

    constexpr bool valid() const noexcept { return num != 0 && den != 0; }
    // Display and the maths "Auto" rate inference only.
    double hz() const noexcept { return static_cast<double>(num) / static_cast<double>(den); }

    // t0 + round-half-up(i * period). Returns t0 for an invalid rate.
    Micros timeAt(SampleIdx i, Micros t0) const noexcept;
    // Largest i with timeAt(i, t0) <= t. Returns 0 for an invalid rate.
    SampleIdx indexAt(Micros t, Micros t0) const noexcept;

    static constexpr Rate fromHz(std::uint32_t hz) noexcept { return Rate{hz, 1}; }
    // Exact rational 1e6 / periodUs reduced by gcd, e.g. 33'333 us -> {1'000'000, 33'333}. Returns the
    // invalid Rate{0, 1} for periodUs == 0 or when the reduced denominator does not fit in 32 bits (period >
    // ~71 min).
    static Rate fromPeriodUs(std::uint64_t periodUs) noexcept;
    // gcd-reduced; an invalid rate normalises to Rate{0, 1}.
    Rate normalised() const noexcept;

    std::string toString() const; // "100/3 Hz", "1000 Hz", "no rate"

    // Value order on 64-bit cross products, not memberwise: {2,2} == {1,1}, {1,2} < {1,1}.
    friend constexpr std::strong_ordering operator<=>(const Rate& a, const Rate& b) noexcept {
        return static_cast<std::uint64_t>(a.num) * b.den <=> static_cast<std::uint64_t>(b.num) * a.den;
    }
    friend constexpr bool operator==(const Rate& a, const Rate& b) noexcept {
        return static_cast<std::uint64_t>(a.num) * b.den == static_cast<std::uint64_t>(b.num) * a.den;
    }
};

} // namespace esses
