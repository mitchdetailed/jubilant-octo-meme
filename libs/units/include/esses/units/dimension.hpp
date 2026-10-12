// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dimension (02 section 2.1, D-010, D-085): the exponent vector [L, M, T, I, Theta, N, J] of a quantity in
// the seven SI base dimensions. Plane angle and ratios are dimensionless; quantities that share a dimension
// stay apart through their display family (Quantity::family), not through the dimension.
//
// unknown() is the dimension of a value whose dimension cannot be represented with integer exponents, such as
// a fractional power of a dimensioned value (03 section 3.2 rule 5). It propagates through every operation;
// exponents outside [-127, 127] also give unknown(). -128 is the sentinel and never a real exponent.
#pragma once

#include <esses/base/config.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esses::units {

// A rational exponent num/den for Dimension::pow (den != 0; the sign may sit on either part).
struct Rational {
    std::int32_t num = 0;
    std::int32_t den = 1;
};

struct Dimension {
    // Index of each base dimension in exp.
    // NOLINTNEXTLINE(performance-enum-size): an index into exp, so it needs no cast.
    enum Base : std::size_t {
        Length = 0,      // L, metre
        Mass = 1,        // M, kilogram
        Time = 2,        // T, second
        Current = 3,     // I, ampere
        Temperature = 4, // Theta, kelvin
        Amount = 5,      // N, mole
        Luminosity = 6   // J, candela
    };
    static constexpr std::size_t kCount = 7;
    static constexpr std::int8_t kUnknownExp = -128;

    std::array<std::int8_t, kCount> exp{};

    // The dimension of a pure number (angle, ratio, count).
    static constexpr Dimension none() noexcept { return Dimension{}; }
    static constexpr Dimension unknown() noexcept {
        Dimension d;
        d.exp.fill(kUnknownExp);
        return d;
    }
    // One base dimension raised to `power`: basis(Length) is L, basis(Time, -1) is T^-1.
    static constexpr Dimension basis(Base b, std::int8_t power = 1) noexcept {
        Dimension d;
        d.exp[b] = power;
        return d;
    }

    constexpr bool isUnknown() const noexcept { return exp[0] == kUnknownExp; }
    constexpr bool isKnown() const noexcept { return !isUnknown(); }
    constexpr bool isDimensionless() const noexcept {
        for (const std::int8_t e : exp) {
            if (e != 0) {
                return false;
            }
        }
        return true;
    }

    // Product and quotient of two quantities: exponents add or subtract.
    Dimension operator*(const Dimension& rhs) const noexcept;
    Dimension operator/(const Dimension& rhs) const noexcept;
    Dimension inverse() const noexcept;
    // Integer power: exponents are multiplied.
    Dimension pow(int n) const noexcept;
    // Rational power: unknown() unless every exponent times num is divisible by den (sqrt of L^2 is L, sqrt
    // of L is unknown). A dimensionless value stays dimensionless; den == 0 gives unknown().
    Dimension pow(Rational r) const noexcept;

    // "L·T⁻²", "M·L⁻¹·T⁻²"; "1" when dimensionless, "?" when unknown. Order L, M, T, I, Θ, N, J.
    std::string toString() const;
    // ASCII form for logs and file names: "L.T^-2", "1", "?".
    std::string toAsciiString() const;

    friend constexpr bool operator==(const Dimension&, const Dimension&) = default;
};

} // namespace esses::units
