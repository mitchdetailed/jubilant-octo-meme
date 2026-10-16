// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Maths types (03 section 3.2, D-021): the kind of a value (Scalar, Series, LapScalar, Bool, Text, RangeRef,
// Invalid) with its unit and dimension tracking. TypeChecker::check infers them bottom-up; no type warning
// ever blocks evaluation (the one hard type error is E-INTEGER-REQUIRED for the bitwise operators).
#pragma once

#include <esses/base/rate.hpp>
#include <esses/units/dimension.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace esses::maths {

enum class Kind : std::uint8_t { Scalar, Series, LapScalar, Bool, Text, RangeRef, Invalid };

struct Type {
    Kind kind = Kind::Scalar;
    units::UnitId unit = units::kNone; // registry unit when the value is in one unit; kNone for composites
    units::Dimension dim;              // Dimension::unknown() after fractional powers
    double scaleToBase = 1.0; // factor of the current numbers relative to SI base of `dim`; NaN unknown
    std::optional<Rate> rate; // Series/Bool/LapScalar only
    bool integer = false;     // IntLit, int(), I32 channels, bit_* results
    std::optional<std::string> lapGroup; // LapScalar: range group path
};

} // namespace esses::maths
