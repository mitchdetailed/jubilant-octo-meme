// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The intrinsic catalogue (03 section 5): every built-in function with its kernel class. registerIntrinsics()
// adds one FunctionManifest per function (names, arities, argument kinds and unit rules of 03 section 5) to a
// FunctionRegistry; compile() turns a call into the kernel of its class (makeIntrinsicKernel).
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/kernel.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace esses::maths {

class FunctionRegistry;
struct Ast;

enum class IntrinsicId : std::uint8_t {
    // Elementwise (03 section 5.1)
    Abs,
    Sqr,
    Sqrt,
    Power,
    Hypot,
    Exp,
    Exp10,
    Ln,
    Log,
    Int,
    Frac,
    Round,
    RoundDown,
    RoundUp,
    Remainder,
    Sgn,
    Min,
    Max,
    RandVal,
    RandChan,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    BitDecode,
    Choose,
    Invalid,
    // Windowed (03 section 5.2)
    Smooth,
    Derivative,
    // IIR (03 section 5.3)
    FilterLp,
    FilterHp,
    FilterChebyLp,
    FilterChebyHp,
    FilterChebyBp,
    FilterChebyBs,
    // Stateful (03 section 5.4)
    Integrate,
    IntegrateDist,
    StatMin,
    StatMax,
    StatMean,
    StatStdDev,
    StatStart,
    StatEnd,
    RunningMin,
    RunningMax,
    FlipFlop,
    EdgeDelay,
    TimeValid,
    TimeShift,
    PulseWidth,
    PreviousSample,
    // Range (03 section 5.5)
    RangeIs,
    RangeChange,
    // Variance (03 section 5.6)
    VarianceDist,
    VarianceTime,
    Count // number of intrinsics, not a function
};

// The canonical (lower-case) function name: "abs", "round_down", "filter_cheby_lp", ...
std::string_view intrinsicName(IntrinsicId id) noexcept;
KernelClass intrinsicClass(IntrinsicId id) noexcept;

// Adds the manifest of every intrinsic (provenance "intrinsic").
void registerIntrinsics(FunctionRegistry& registry);

// The kernel of a non-elementwise intrinsic call node of `ast` (`call` indexes Ast::nodes; its arguments are
// already typed). Elementwise intrinsics are fused into the plan's EwKernel by compile() instead. Errors:
// Invalid (wrong node), Unsupported (an elementwise id), argument errors (E-ARG-VALUE in context.detail).
Result<std::unique_ptr<Kernel>> makeIntrinsicKernel(IntrinsicId id, const Ast& ast, std::uint32_t call);

} // namespace esses::maths
