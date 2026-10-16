// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The argument signatures of the intrinsics as the type checker and compile() read them (03 section 5, D-020,
// D-080, D-082): accepted arities, the role of every parameter and the result rule of 03 section 3.2 rule 9.
// The FunctionRegistry manifests (registerIntrinsics, src/kernels) describe the same functions for the editor
// and `esses-cli maths functions`; a unit test checks that both agree on names and arities. Internal to
// esses_maths.
//
// Parameter roles and the plan: a "series" role (Value, Channel, Cond, Reset, and WindowOrChannel when the
// argument is per-sample) becomes an input of the call's stage; every other role is a parameter that
// compile() folds into a literal node (IntLit, DecLit, StrLit, Keyword) of the AST it hands to
// makeIntrinsicKernel (plan.hpp, "Stage inputs of an intrinsic call").
#pragma once

#include <esses/maths/intrinsics.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace esses::maths::lang {

enum class Role : std::uint8_t {
    Value,           // numeric operand: per-sample or scalar (a constant is broadcast)
    Channel,         // numeric operand that must vary per sample (smooth, derivative, filters, variance)
    Cond,            // gate: any numeric, non-zero = true, invalid = false
    Reset,           // segment reset: any numeric, a new segment when its integer part increases
    Window,          // points (integer) or seconds (decimal or a time), 03 section 2.4 rule 2
    WindowOrChannel, // derivative's second argument: a per-sample y (with respect to y) or a window
    Seconds,         // a duration: a time quantity or a plain number of seconds
    Hertz,           // a frequency > 0: a frequency quantity or a plain number of hertz
    Order,           // filter order, an integer from the function's list
    Ripple,          // pass-band ripple in percent, >= 0
    Count,           // a non-negative integer (bit index, bit count, sample count)
    RangePath,       // "Group:Path" text, typed RangeRef
    Edge             // the keyword rise or fall
};

enum class Rule : std::uint8_t {
    SameAs0,         // same_as:x
    Unitless,        // unitless
    UnitlessFromDim, // unitless; a dimensioned argument is W-DIM-MISMATCH (exp, ln, ...)
    AngleIn,         // argument in radians, result unitless (sin, cos, tan)
    AngleOut,        // result an angle in radians (asin, acos, atan)
    Atan2,           // unify:y,x, result an angle
    Sqrt0,           // sqrt:x
    Sqr0,            // pow:x,2
    Pow01,           // pow:x,y
    Unify01,         // unify:x,y (min, max, hypot)
    Integer0,        // same_as:x, integer (int)
    Sign,            // unitless, integer (sgn)
    Bitwise,         // integer operands, same_as:x, integer
    BitDecode,       // integer operands, unitless integer
    Choose,          // unify:a,b over the 2nd and 3rd argument
    Invalid,         // invalid(): every sample invalid
    RandomScalar,    // rand_val: one value per evaluation
    RandomSeries,    // rand_chan: one value per sample
    Derivative,      // per_time:x, or dim(x) / dim(y) with respect to y
    TimesTime0,      // times_time:x
    TimesLength0,    // times_length:x
    Stat,            // same_as:x; Scalar (math window) without reset, LapScalar with reset (D-080)
    Series0,         // same_as:x, a series (filters, running_*, time_shift, previous_sample, variance_*)
    Bool,            // unitless 0/1 series
    Time,            // seconds (pulse_width)
    RangeCount       // unitless integer series (range_change)
};

struct Param {
    std::string_view name;
    Role role = Role::Value;
};

struct Signature {
    IntrinsicId id = IntrinsicId::Count;
    std::array<Param, 5> params{};
    std::uint8_t paramCount = 0;
    std::array<std::uint8_t, 3> arities{};
    std::uint8_t arityCount = 0;
    Rule rule = Rule::SameAs0;

    bool accepts(std::size_t argc) const noexcept {
        for (std::uint8_t i = 0; i < arityCount; ++i) {
            if (arities[i] == argc) {
                return true;
            }
        }
        return false;
    }
    // "1", "2 or 3", "2, 3 or 4" (E-ARITY).
    std::string aritiesText() const;
};

const Signature& signatureOf(IntrinsicId id) noexcept;
// The intrinsic with this canonical (lower-case) name.
std::optional<IntrinsicId> intrinsicByName(std::string_view name) noexcept;

// True when the argument role makes the argument an input of the call's stage (see the file comment);
// `perSample` tells WindowOrChannel apart.
constexpr bool isSeriesRole(Role role, bool perSample) noexcept {
    switch (role) {
    case Role::Value:
    case Role::Channel:
    case Role::Cond:
    case Role::Reset:
        return true;
    case Role::WindowOrChannel:
        return perSample;
    default:
        return false;
    }
}

} // namespace esses::maths::lang
