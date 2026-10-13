// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The per-sample semantics of every elementwise operator and function (03 sections 3.2 and 5.1), shared by
// the type checker's constant folding and the fused EwKernel, so a folded constant and an evaluated sample
// can never differ. Values are doubles and NaN is "invalid": every operation propagates an invalid operand
// unless 03 section 5.1 says otherwise (choose takes the validity of the chosen branch; min/max return the
// valid operand when exactly one is invalid). Division by zero, a negative square root, a logarithm of a
// non-positive number and every other domain error give an invalid sample, an overflow to infinity of a
// quotient, a power, exp, exp10, sinh, cosh or tan included (D-185; evaluateWhole also clears any other
// non-finite result).
//
// Each operation is a template on its operator (unaryOp<Op>, binaryOp<Op>, callOp<Id>) so that the kernel's
// per-operator loops compile to straight-line code; unary(), binary() and call() dispatch at run time for
// constant folding. Internal to esses_maths.
#pragma once

#include <esses/maths/ast.hpp>
#include <esses/maths/intrinsics.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace esses::maths::ew {

inline constexpr double kInvalid = std::numeric_limits<double>::quiet_NaN();

// NaN test without a library call (std::isnan is an out-of-line call on some runtimes).
constexpr bool invalid(double v) noexcept {
    return v != v; // NOLINT(misc-redundant-expression): true only for NaN
}

constexpr double truth(bool b) noexcept {
    return b ? 1.0 : 0.0;
}

// v when it is finite, else invalid (an overflow to +-infinity is a domain error, 03 sections 5.1 and 6.6).
constexpr double finite(double v) noexcept {
    return v - v == 0.0 ? v : kInvalid; // NOLINT(misc-redundant-expression): 0 unless v is +-inf or NaN
}

// Integer view of a value for the bitwise operations: truncated toward zero, exact in 64 bits (03
// section 6.1: integer registers are F64 lanes). False for invalid or out-of-range values.
inline bool toInt(double v, std::int64_t& out) noexcept {
    if (!(v > -9.2e18 && v < 9.2e18)) {
        return false;
    }
    out = static_cast<std::int64_t>(v);
    return true;
}

enum class BitOp : std::uint8_t { And, Or, Xor };

template <BitOp Op> inline double bitwiseOp(double a, double b) noexcept {
    std::int64_t x = 0;
    std::int64_t y = 0;
    if (!toInt(a, x) || !toInt(b, y)) {
        return kInvalid;
    }
    if constexpr (Op == BitOp::And) {
        return static_cast<double>(x & y);
    } else if constexpr (Op == BitOp::Or) {
        return static_cast<double>(x | y);
    } else {
        return static_cast<double>(x ^ y);
    }
}

template <UnaryOp Op> inline double unaryOp(double a) noexcept {
    if constexpr (Op == UnaryOp::Neg) {
        return -a;
    } else if constexpr (Op == UnaryOp::Plus) {
        return a;
    } else if constexpr (Op == UnaryOp::Not) {
        return invalid(a) ? kInvalid : truth(a == 0.0);
    } else {
        std::int64_t i = 0;
        if (!toInt(a, i)) {
            return kInvalid;
        }
        // 32-bit two's complement complement [compat] (03 section 5.1 bit_not).
        const auto low = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i));
        return static_cast<double>(static_cast<std::int32_t>(~low));
    }
}

template <BinaryOp Op> inline double binaryOp(double a, double b) noexcept {
    if constexpr (Op == BinaryOp::Add) {
        return a + b;
    } else if constexpr (Op == BinaryOp::Sub) {
        return a - b;
    } else if constexpr (Op == BinaryOp::Mul) {
        return a * b;
    } else if constexpr (Op == BinaryOp::Div) {
        return b == 0.0 ? kInvalid : finite(a / b);
    } else if constexpr (Op == BinaryOp::Pow) {
        return invalid(a) || invalid(b) ? kInvalid : finite(std::pow(a, b));
    } else if constexpr (Op == BinaryOp::Lt) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a < b);
    } else if constexpr (Op == BinaryOp::Le) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a <= b);
    } else if constexpr (Op == BinaryOp::Gt) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a > b);
    } else if constexpr (Op == BinaryOp::Ge) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a >= b);
    } else if constexpr (Op == BinaryOp::Eq) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a == b);
    } else if constexpr (Op == BinaryOp::Ne) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a != b);
    } else if constexpr (Op == BinaryOp::BitAnd) {
        return bitwiseOp<BitOp::And>(a, b);
    } else if constexpr (Op == BinaryOp::BitOr) {
        return bitwiseOp<BitOp::Or>(a, b);
    } else if constexpr (Op == BinaryOp::And) {
        return invalid(a) || invalid(b) ? kInvalid : truth(a != 0.0 && b != 0.0);
    } else {
        return invalid(a) || invalid(b) ? kInvalid : truth(a != 0.0 || b != 0.0);
    }
}

// splitmix64 (D-087): rand_val() draws once per evaluation from the plan's seed, rand_chan() once per sample
// index, so cached results are reproducible.
inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline double unitInterval(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11) * 0x1.0p-53; // [0, 1)
}

inline double bitDecode(double x, double low, double count) noexcept {
    std::int64_t v = 0;
    std::int64_t lo = 0;
    std::int64_t n = 0;
    if (!toInt(x, v) || !toInt(low, lo) || !toInt(count, n) || lo < 0 || n < 0) {
        return kInvalid;
    }
    if (lo >= 64 || n == 0) {
        return 0.0;
    }
    const auto u = static_cast<std::uint64_t>(v) >> lo;
    const std::uint64_t mask = n >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << n) - 1);
    return static_cast<double>(u & mask);
}

// An elementwise intrinsic over 1-3 arguments (the arguments that do not exist are ignored; bit_decode(x,
// bit) passes count = 1). invalid() and the two random functions are not covered: the kernel and the checker
// treat them on their own; every other id gives an invalid sample.
template <IntrinsicId Id> inline double callOp(double a, double b, double c) noexcept {
    using I = IntrinsicId;
    if constexpr (Id == I::Abs) {
        return std::fabs(a);
    } else if constexpr (Id == I::Sqr) {
        return a * a;
    } else if constexpr (Id == I::Sqrt) {
        return a < 0.0 ? kInvalid : std::sqrt(a);
    } else if constexpr (Id == I::Power) {
        return binaryOp<BinaryOp::Pow>(a, b);
    } else if constexpr (Id == I::Hypot) {
        return invalid(a) || invalid(b) ? kInvalid : std::hypot(a, b);
    } else if constexpr (Id == I::Exp) {
        return finite(std::exp(a));
    } else if constexpr (Id == I::Exp10) {
        return invalid(a) ? kInvalid : finite(std::pow(10.0, a));
    } else if constexpr (Id == I::Ln) {
        return a > 0.0 ? std::log(a) : kInvalid;
    } else if constexpr (Id == I::Log) {
        return a > 0.0 ? std::log10(a) : kInvalid;
    } else if constexpr (Id == I::Int) {
        return std::trunc(a);
    } else if constexpr (Id == I::Frac) {
        return a - std::trunc(a);
    } else if constexpr (Id == I::Round) {
        return std::round(a); // halves away from zero [compat]
    } else if constexpr (Id == I::RoundDown) {
        return std::floor(a);
    } else if constexpr (Id == I::RoundUp) {
        return std::ceil(a);
    } else if constexpr (Id == I::Remainder) {
        return b == 0.0 || invalid(a) || invalid(b) ? kInvalid : std::fmod(a, b);
    } else if constexpr (Id == I::Sgn) {
        return invalid(a) ? kInvalid : (a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0));
    } else if constexpr (Id == I::Min) {
        return invalid(a) ? b : (invalid(b) ? a : (b < a ? b : a)); // [compat] one invalid operand
    } else if constexpr (Id == I::Max) {
        return invalid(a) ? b : (invalid(b) ? a : (b > a ? b : a));
    } else if constexpr (Id == I::Sin) {
        return std::sin(a);
    } else if constexpr (Id == I::Cos) {
        return std::cos(a);
    } else if constexpr (Id == I::Tan) {
        return finite(std::tan(a));
    } else if constexpr (Id == I::Asin) {
        return a < -1.0 || a > 1.0 ? kInvalid : std::asin(a);
    } else if constexpr (Id == I::Acos) {
        return a < -1.0 || a > 1.0 ? kInvalid : std::acos(a);
    } else if constexpr (Id == I::Atan) {
        return std::atan(a);
    } else if constexpr (Id == I::Atan2) {
        return invalid(a) || invalid(b) ? kInvalid : std::atan2(a, b);
    } else if constexpr (Id == I::Sinh) {
        return finite(std::sinh(a));
    } else if constexpr (Id == I::Cosh) {
        return finite(std::cosh(a));
    } else if constexpr (Id == I::Tanh) {
        return std::tanh(a);
    } else if constexpr (Id == I::Asinh) {
        return std::asinh(a);
    } else if constexpr (Id == I::Acosh) {
        return a < 1.0 ? kInvalid : std::acosh(a);
    } else if constexpr (Id == I::Atanh) {
        return a <= -1.0 || a >= 1.0 ? kInvalid : std::atanh(a);
    } else if constexpr (Id == I::BitAnd) {
        return bitwiseOp<BitOp::And>(a, b);
    } else if constexpr (Id == I::BitOr) {
        return bitwiseOp<BitOp::Or>(a, b);
    } else if constexpr (Id == I::BitXor) {
        return bitwiseOp<BitOp::Xor>(a, b);
    } else if constexpr (Id == I::BitNot) {
        return unaryOp<UnaryOp::BitNot>(a);
    } else if constexpr (Id == I::BitDecode) {
        return bitDecode(a, b, c);
    } else if constexpr (Id == I::Choose) {
        return invalid(a) ? kInvalid : (a != 0.0 ? b : c);
    } else {
        return kInvalid;
    }
}

// ---- run-time dispatch (constant folding)
// ------------------------------------------------------------------

inline double unary(UnaryOp op, double a) noexcept {
    switch (op) {
    case UnaryOp::Neg:
        return unaryOp<UnaryOp::Neg>(a);
    case UnaryOp::Plus:
        return unaryOp<UnaryOp::Plus>(a);
    case UnaryOp::Not:
        return unaryOp<UnaryOp::Not>(a);
    case UnaryOp::BitNot:
        return unaryOp<UnaryOp::BitNot>(a);
    }
    return kInvalid;
}

inline double binary(BinaryOp op, double a, double b) noexcept {
    switch (op) {
    case BinaryOp::Add:
        return binaryOp<BinaryOp::Add>(a, b);
    case BinaryOp::Sub:
        return binaryOp<BinaryOp::Sub>(a, b);
    case BinaryOp::Mul:
        return binaryOp<BinaryOp::Mul>(a, b);
    case BinaryOp::Div:
        return binaryOp<BinaryOp::Div>(a, b);
    case BinaryOp::Pow:
        return binaryOp<BinaryOp::Pow>(a, b);
    case BinaryOp::Lt:
        return binaryOp<BinaryOp::Lt>(a, b);
    case BinaryOp::Le:
        return binaryOp<BinaryOp::Le>(a, b);
    case BinaryOp::Gt:
        return binaryOp<BinaryOp::Gt>(a, b);
    case BinaryOp::Ge:
        return binaryOp<BinaryOp::Ge>(a, b);
    case BinaryOp::Eq:
        return binaryOp<BinaryOp::Eq>(a, b);
    case BinaryOp::Ne:
        return binaryOp<BinaryOp::Ne>(a, b);
    case BinaryOp::BitAnd:
        return binaryOp<BinaryOp::BitAnd>(a, b);
    case BinaryOp::BitOr:
        return binaryOp<BinaryOp::BitOr>(a, b);
    case BinaryOp::And:
        return binaryOp<BinaryOp::And>(a, b);
    case BinaryOp::Or:
        return binaryOp<BinaryOp::Or>(a, b);
    }
    return kInvalid;
}

namespace detail {
using CallFn = double (*)(double, double, double) noexcept;
template <std::size_t... K> constexpr auto callTable(std::index_sequence<K...> /*unused*/) {
    return std::array<CallFn, sizeof...(K)>{&callOp<static_cast<IntrinsicId>(K)>...};
}
inline constexpr auto kCallTable =
    callTable(std::make_index_sequence<static_cast<std::size_t>(IntrinsicId::Count)>{});
} // namespace detail

inline double call(IntrinsicId id, double a, double b, double c) noexcept {
    const auto i = static_cast<std::size_t>(id);
    return i < detail::kCallTable.size() ? detail::kCallTable[i](a, b, c) : kInvalid;
}

} // namespace esses::maths::ew
