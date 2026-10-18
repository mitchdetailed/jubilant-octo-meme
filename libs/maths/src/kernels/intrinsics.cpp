// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The intrinsic catalogue (03 section 5): names and kernel classes (the contract), one FunctionManifest per
// intrinsic (registerIntrinsics; 03 section 10.1, D-024) and the kernel of every non-elementwise call
// (makeIntrinsicKernel, dispatching to the kernel classes of src/kernels). The manifests take their argument
// lists and arities from the language's signature table (src/lang/signatures.hpp), so the type checker, the
// registry and `esses-cli maths functions` agree by construction; the descriptions are ours.

#include "../lang/signatures.hpp"
#include "kernels_internal.hpp"

#include <esses/maths/function_registry.hpp>
#include <esses/maths/intrinsics.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace esses::maths {

namespace {

struct IntrinsicRow {
    std::string_view name;
    KernelClass cls;
};

constexpr std::array<IntrinsicRow, static_cast<std::size_t>(IntrinsicId::Count)> kIntrinsics{{
    {"abs", KernelClass::Elementwise},        {"sqr", KernelClass::Elementwise},
    {"sqrt", KernelClass::Elementwise},       {"power", KernelClass::Elementwise},
    {"hypot", KernelClass::Elementwise},      {"exp", KernelClass::Elementwise},
    {"exp10", KernelClass::Elementwise},      {"ln", KernelClass::Elementwise},
    {"log", KernelClass::Elementwise},        {"int", KernelClass::Elementwise},
    {"frac", KernelClass::Elementwise},       {"round", KernelClass::Elementwise},
    {"round_down", KernelClass::Elementwise}, {"round_up", KernelClass::Elementwise},
    {"remainder", KernelClass::Elementwise},  {"sgn", KernelClass::Elementwise},
    {"min", KernelClass::Elementwise},        {"max", KernelClass::Elementwise},
    {"rand_val", KernelClass::Elementwise},   {"rand_chan", KernelClass::Elementwise},
    {"sin", KernelClass::Elementwise},        {"cos", KernelClass::Elementwise},
    {"tan", KernelClass::Elementwise},        {"asin", KernelClass::Elementwise},
    {"acos", KernelClass::Elementwise},       {"atan", KernelClass::Elementwise},
    {"atan2", KernelClass::Elementwise},      {"sinh", KernelClass::Elementwise},
    {"cosh", KernelClass::Elementwise},       {"tanh", KernelClass::Elementwise},
    {"asinh", KernelClass::Elementwise},      {"acosh", KernelClass::Elementwise},
    {"atanh", KernelClass::Elementwise},      {"bit_and", KernelClass::Elementwise},
    {"bit_or", KernelClass::Elementwise},     {"bit_xor", KernelClass::Elementwise},
    {"bit_not", KernelClass::Elementwise},    {"bit_decode", KernelClass::Elementwise},
    {"choose", KernelClass::Elementwise},     {"invalid", KernelClass::Elementwise},
    {"smooth", KernelClass::Windowed},        {"derivative", KernelClass::Windowed},
    {"filter_lp", KernelClass::IIR},          {"filter_hp", KernelClass::IIR},
    {"filter_cheby_lp", KernelClass::IIR},    {"filter_cheby_hp", KernelClass::IIR},
    {"filter_cheby_bp", KernelClass::IIR},    {"filter_cheby_bs", KernelClass::IIR},
    {"integrate", KernelClass::Stateful},     {"integrate_dist", KernelClass::Stateful},
    {"stat_min", KernelClass::Stateful},      {"stat_max", KernelClass::Stateful},
    {"stat_mean", KernelClass::Stateful},     {"stat_std_dev", KernelClass::Stateful},
    {"stat_start", KernelClass::Stateful},    {"stat_end", KernelClass::Stateful},
    {"running_min", KernelClass::Stateful},   {"running_max", KernelClass::Stateful},
    {"flip_flop", KernelClass::Stateful},     {"edge_delay", KernelClass::Stateful},
    {"time_valid", KernelClass::Stateful},    {"time_shift", KernelClass::Stateful},
    {"pulse_width", KernelClass::Stateful},   {"previous_sample", KernelClass::Stateful},
    {"range_is", KernelClass::Range},         {"range_change", KernelClass::Range},
    {"variance_dist", KernelClass::Variance}, {"variance_time", KernelClass::Variance},
}};

} // namespace

std::string_view intrinsicName(IntrinsicId id) noexcept {
    const auto i = static_cast<std::size_t>(id);
    return i < kIntrinsics.size() ? kIntrinsics[i].name : std::string_view{};
}

KernelClass intrinsicClass(IntrinsicId id) noexcept {
    const auto i = static_cast<std::size_t>(id);
    return i < kIntrinsics.size() ? kIntrinsics[i].cls : KernelClass::External;
}

namespace {

// One line per intrinsic, in IntrinsicId order (our wording of 03 section 5).
constexpr std::array<std::string_view, static_cast<std::size_t>(IntrinsicId::Count)> kDescriptions{{
    "absolute value",
    "square",
    "square root; a negative argument gives an invalid sample",
    "x raised to the power y (y may be a channel)",
    "length of the hypotenuse, sqrt(x^2 + y^2)",
    "e raised to the power x",
    "10 raised to the power x",
    "natural logarithm; x <= 0 gives an invalid sample",
    "base-10 logarithm; x <= 0 gives an invalid sample",
    "integer part, truncated toward zero",
    "fractional part, x - int(x)",
    "nearest whole number, halves away from zero",
    "largest whole number not above x",
    "smallest whole number not below x",
    "remainder of x / y with the sign of x; y = 0 gives an invalid sample",
    "sign: -1, 0 or +1",
    "smaller of x and y; the valid one when only one is valid",
    "larger of x and y; the valid one when only one is valid",
    "one random value in [0, 1) per evaluation, reproducible",
    "a random value in [0, 1) per sample, reproducible",
    "sine of an angle in radians",
    "cosine of an angle in radians",
    "tangent of an angle in radians",
    "arc sine in radians",
    "arc cosine in radians",
    "arc tangent in radians",
    "four-quadrant arc tangent of y / x in radians",
    "hyperbolic sine",
    "hyperbolic cosine",
    "hyperbolic tangent",
    "inverse hyperbolic sine",
    "inverse hyperbolic cosine",
    "inverse hyperbolic tangent",
    "bitwise AND of integers",
    "bitwise OR of integers",
    "bitwise exclusive OR of integers",
    "bitwise complement of a 32-bit integer",
    "one bit of x, or count bits from bit upward",
    "per sample: a where cond is non-zero, else b",
    "a channel of invalid samples",
    "moving average over n points (integer) or t seconds (decimal)",
    "least-squares slope per second over n points, or against the channel y",
    "first-order RC low pass at fc Hz; order 1-3 forward passes, -1 backward, -2 zero phase",
    "first-order RC high pass at fc Hz; order 1-3 forward passes, -1 backward, -2 zero phase",
    "Chebyshev type I low pass (Butterworth for ripple 0); order 2, 4, 6 or zero phase -4, -8, -12",
    "Chebyshev type I high pass (Butterworth for ripple 0); order 2, 4, 6 or zero phase -4, -8, -12",
    "Chebyshev type I band pass between f_lo and f_hi Hz",
    "Chebyshev type I band stop between f_lo and f_hi Hz",
    "integral of x over time while cond holds, restarting when reset increases",
    "integral of x over Corr Dist while cond holds, restarting when reset increases",
    "minimum of x over the segment (the math window without reset)",
    "maximum of x over the segment (the math window without reset)",
    "mean of x over the segment (the math window without reset)",
    "population standard deviation of x over the segment",
    "first value of x in the segment",
    "last value of x in the segment",
    "lowest value of x so far in the segment",
    "highest value of x so far in the segment",
    "set/reset latch on the rising edges of set and reset",
    "delays the rising or falling edges of a Bool by t seconds",
    "1 once x has been true for at least t seconds",
    "x shifted later by t seconds",
    "width in seconds of the last completed high pulse of x",
    "the value of x n samples earlier",
    "1 inside any range of the group, else 0",
    "number of range starts of the group so far",
    "x on the Main lap minus x on the reference lap at the same distance",
    "x on the Main lap minus x on the reference lap at the same time from the start",
}};

ArgSpec::Kind argKind(lang::Role role) noexcept {
    switch (role) {
    case lang::Role::Value:
    case lang::Role::Channel:
    case lang::Role::Cond:
    case lang::Role::Reset:
    case lang::Role::WindowOrChannel:
        return ArgSpec::Kind::Series;
    case lang::Role::RangePath:
        return ArgSpec::Kind::RangeRef;
    case lang::Role::Edge:
        return ArgSpec::Kind::Keyword;
    default:
        return ArgSpec::Kind::Scalar;
    }
}

std::string_view argDim(lang::Role role) noexcept {
    switch (role) {
    case lang::Role::Seconds:
        return "time";
    case lang::Role::Hertz:
        return "frequency";
    case lang::Role::Order:
    case lang::Role::Ripple:
    case lang::Role::Count:
        return "dimensionless";
    default:
        return "any";
    }
}

// The result's unit rule in the vocabulary of 03 section 3.2 rule 9.
std::string_view unitRule(lang::Rule rule) noexcept {
    switch (rule) {
    case lang::Rule::SameAs0:
    case lang::Rule::Integer0:
    case lang::Rule::Bitwise:
    case lang::Rule::Stat:
    case lang::Rule::Series0:
        return "same_as:x";
    case lang::Rule::AngleOut:
    case lang::Rule::Atan2:
        return "angle";
    case lang::Rule::Sqrt0:
        return "sqrt:x";
    case lang::Rule::Sqr0:
        return "pow:x,2";
    case lang::Rule::Pow01:
        return "pow:x,y";
    case lang::Rule::Unify01:
        return "unify:x,y";
    case lang::Rule::Choose:
        return "unify:a,b";
    case lang::Rule::Derivative:
        return "per_time:x";
    case lang::Rule::TimesTime0:
        return "times_time:x";
    case lang::Rule::TimesLength0:
        return "times_length:x";
    case lang::Rule::Time:
        return "declared:s";
    default:
        return "unitless";
    }
}

ArgSpec::Kind resultKind(lang::Rule rule) noexcept {
    return rule == lang::Rule::RandomScalar ? ArgSpec::Kind::Scalar : ArgSpec::Kind::Series;
}

} // namespace

void registerIntrinsics(FunctionRegistry& registry) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(IntrinsicId::Count); ++i) {
        const auto id = static_cast<IntrinsicId>(i);
        const lang::Signature& sig = lang::signatureOf(id);
        FunctionManifest m;
        m.name = std::string(intrinsicName(id));
        m.description = std::string(kDescriptions[i]);
        std::uint8_t minArity = sig.arityCount > 0 ? sig.arities[0] : 0;
        for (std::uint8_t k = 0; k < sig.arityCount; ++k) {
            minArity = std::min(minArity, sig.arities[k]);
            m.arities.push_back(sig.arities[k]);
        }
        for (std::uint8_t k = 0; k < sig.paramCount; ++k) {
            ArgSpec a;
            a.name = std::string(sig.params[k].name);
            a.kind = argKind(sig.params[k].role);
            a.dim = std::string(argDim(sig.params[k].role));
            a.optional = k >= minArity;
            m.args.push_back(std::move(a));
        }
        m.returns.kind = resultKind(sig.rule);
        m.returns.unitRule = std::string(unitRule(sig.rule));
        m.cls = intrinsicClass(id);
        // Chunk-independent with no context: the elementwise and range functions (03 section 6.1).
        m.blockCapable = m.cls == KernelClass::Elementwise || m.cls == KernelClass::Range;
        m.telemetry = false;
        m.provenance = "intrinsic";
        registry.addIntrinsic(m, id);
    }
}

Result<std::unique_ptr<Kernel>> makeIntrinsicKernel(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    if (call >= ast.nodes.size() || ast.nodes[call].kind != NodeKind::Call) {
        return fail(ErrorCode::Invalid, "makeIntrinsicKernel: the node is not a call");
    }
    switch (intrinsicClass(id)) {
    case KernelClass::Elementwise:
        return fail(ErrorCode::Unsupported,
                    "makeIntrinsicKernel: " + std::string(intrinsicName(id)) + " is fused into the EwKernel");
    case KernelClass::Windowed:
        return kern::makeWindowed(id, ast, call);
    case KernelClass::IIR:
        return kern::makeIir(id, ast, call);
    case KernelClass::Stateful:
        return kern::makeStateful(id, ast, call);
    case KernelClass::Range:
        return kern::makeRange(id, ast, call);
    case KernelClass::Variance:
        return kern::makeVariance(id, ast, call);
    case KernelClass::Table:
    case KernelClass::External:
        break;
    }
    return fail(ErrorCode::Invalid, "makeIntrinsicKernel: not an intrinsic");
}

} // namespace esses::maths
