// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Converter (02 section 2.1): the affine map y = a * x + b between two units of one family, obtained from
// UnitRegistry::converter / toBase / fromBase. Arithmetic is done in double; the float overload rounds once.
#pragma once

#include <esses/base/config.hpp>

#include <span>

namespace esses::units {

struct Converter {
    double a = 1.0;
    double b = 0.0;

    bool identity() const noexcept { return a == 1.0 && b == 0.0; }
    float operator()(float x) const noexcept { return static_cast<float>(a * static_cast<double>(x) + b); }
    double operator()(double x) const noexcept { return a * x + b; }

    // In-place conversion of a block of samples (importers convert to base once at decode, D-010).
    void applyInPlace(std::span<float> values) const noexcept;
    void applyInPlace(std::span<double> values) const noexcept;

    // x = (y - b) / a. Every converter the registry hands out has a != 0.
    Converter inverse() const noexcept;
    // `next` applied after this one: then(next)(x) == next(this(x)) up to rounding.
    Converter then(const Converter& next) const noexcept;

    friend constexpr bool operator==(const Converter&, const Converter&) = default;
};

} // namespace esses::units
