// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The intrinsic signature table (03 section 5). Arities follow the catalogue: smooth (x, n|t); derivative
// (x), (x, n|t), (x, y), (x, y, n|t); the RC filters (x, fc[, order]); the Chebyshev low/high pass (x, fc[,
// order[, ripple]]) and band pass/stop (x, f_lo, f_hi[, order[, ripple]]); integrate*, stat_* and running_*
// (x[, cond[, reset]]); bit_decode (x, bit) and (x, lowBit, count) (D-082).

#include "signatures.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace esses::maths::lang {

namespace {

using R = Role;

constexpr Param p(std::string_view name, Role role) {
    return Param{name, role};
}

constexpr Signature sig(IntrinsicId id, std::initializer_list<Param> params,
                        std::initializer_list<int> arities, Rule rule) {
    Signature s;
    s.id = id;
    for (const Param& q : params) {
        s.params[s.paramCount++] = q;
    }
    for (const int a : arities) {
        s.arities[s.arityCount++] = static_cast<std::uint8_t>(a);
    }
    s.rule = rule;
    return s;
}

using I = IntrinsicId;

constexpr Param kX = {"x", R::Value};
constexpr Param kY = {"y", R::Value};
constexpr Param kChan = {"x", R::Channel};
constexpr Param kCond = {"cond", R::Cond};
constexpr Param kReset = {"reset", R::Reset};

const std::array<Signature, static_cast<std::size_t>(IntrinsicId::Count)>& table() {
    static const std::array<Signature, static_cast<std::size_t>(IntrinsicId::Count)> t{{
        sig(I::Abs, {kX}, {1}, Rule::SameAs0),
        sig(I::Sqr, {kX}, {1}, Rule::Sqr0),
        sig(I::Sqrt, {kX}, {1}, Rule::Sqrt0),
        sig(I::Power, {kX, kY}, {2}, Rule::Pow01),
        sig(I::Hypot, {kX, kY}, {2}, Rule::Unify01),
        sig(I::Exp, {kX}, {1}, Rule::UnitlessFromDim),
        sig(I::Exp10, {kX}, {1}, Rule::UnitlessFromDim),
        sig(I::Ln, {kX}, {1}, Rule::UnitlessFromDim),
        sig(I::Log, {kX}, {1}, Rule::UnitlessFromDim),
        sig(I::Int, {kX}, {1}, Rule::Integer0),
        sig(I::Frac, {kX}, {1}, Rule::SameAs0),
        sig(I::Round, {kX}, {1}, Rule::SameAs0),
        sig(I::RoundDown, {kX}, {1}, Rule::SameAs0),
        sig(I::RoundUp, {kX}, {1}, Rule::SameAs0),
        sig(I::Remainder, {kX, kY}, {2}, Rule::SameAs0),
        sig(I::Sgn, {kX}, {1}, Rule::Sign),
        sig(I::Min, {kX, kY}, {2}, Rule::Unify01),
        sig(I::Max, {kX, kY}, {2}, Rule::Unify01),
        sig(I::RandVal, {}, {0}, Rule::RandomScalar),
        sig(I::RandChan, {}, {0}, Rule::RandomSeries),
        sig(I::Sin, {kX}, {1}, Rule::AngleIn),
        sig(I::Cos, {kX}, {1}, Rule::AngleIn),
        sig(I::Tan, {kX}, {1}, Rule::AngleIn),
        sig(I::Asin, {kX}, {1}, Rule::AngleOut),
        sig(I::Acos, {kX}, {1}, Rule::AngleOut),
        sig(I::Atan, {kX}, {1}, Rule::AngleOut),
        sig(I::Atan2, {p("y", R::Value), p("x", R::Value)}, {2}, Rule::Atan2),
        sig(I::Sinh, {kX}, {1}, Rule::Unitless),
        sig(I::Cosh, {kX}, {1}, Rule::Unitless),
        sig(I::Tanh, {kX}, {1}, Rule::Unitless),
        sig(I::Asinh, {kX}, {1}, Rule::Unitless),
        sig(I::Acosh, {kX}, {1}, Rule::Unitless),
        sig(I::Atanh, {kX}, {1}, Rule::Unitless),
        sig(I::BitAnd, {kX, kY}, {2}, Rule::Bitwise),
        sig(I::BitOr, {kX, kY}, {2}, Rule::Bitwise),
        sig(I::BitXor, {kX, kY}, {2}, Rule::Bitwise),
        sig(I::BitNot, {kX}, {1}, Rule::Bitwise),
        sig(I::BitDecode, {kX, p("bit", R::Value), p("count", R::Value)}, {2, 3}, Rule::BitDecode),
        sig(I::Choose, {p("cond", R::Value), p("a", R::Value), p("b", R::Value)}, {3}, Rule::Choose),
        sig(I::Invalid, {}, {0}, Rule::Invalid),
        sig(I::Smooth, {kChan, p("n", R::Window)}, {2}, Rule::SameAs0),
        sig(I::Derivative, {kChan, p("n", R::WindowOrChannel), p("n", R::Window)}, {1, 2, 3},
            Rule::Derivative),
        sig(I::FilterLp, {kChan, p("fc", R::Hertz), p("order", R::Order)}, {2, 3}, Rule::Series0),
        sig(I::FilterHp, {kChan, p("fc", R::Hertz), p("order", R::Order)}, {2, 3}, Rule::Series0),
        sig(I::FilterChebyLp, {kChan, p("fc", R::Hertz), p("order", R::Order), p("ripple", R::Ripple)},
            {2, 3, 4}, Rule::Series0),
        sig(I::FilterChebyHp, {kChan, p("fc", R::Hertz), p("order", R::Order), p("ripple", R::Ripple)},
            {2, 3, 4}, Rule::Series0),
        sig(I::FilterChebyBp,
            {kChan, p("f_lo", R::Hertz), p("f_hi", R::Hertz), p("order", R::Order), p("ripple", R::Ripple)},
            {3, 4, 5}, Rule::Series0),
        sig(I::FilterChebyBs,
            {kChan, p("f_lo", R::Hertz), p("f_hi", R::Hertz), p("order", R::Order), p("ripple", R::Ripple)},
            {3, 4, 5}, Rule::Series0),
        sig(I::Integrate, {kX, kCond, kReset}, {1, 2, 3}, Rule::TimesTime0),
        sig(I::IntegrateDist, {kX, kCond, kReset}, {1, 2, 3}, Rule::TimesLength0),
        sig(I::StatMin, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::StatMax, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::StatMean, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::StatStdDev, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::StatStart, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::StatEnd, {kX, kCond, kReset}, {1, 2, 3}, Rule::Stat),
        sig(I::RunningMin, {kX, kCond, kReset}, {1, 2, 3}, Rule::Series0),
        sig(I::RunningMax, {kX, kCond, kReset}, {1, 2, 3}, Rule::Series0),
        sig(I::FlipFlop, {p("set", R::Value), p("reset", R::Value)}, {2}, Rule::Bool),
        sig(I::EdgeDelay, {kX, p("t", R::Seconds), p("edge", R::Edge)}, {3}, Rule::Bool),
        sig(I::TimeValid, {kX, p("t", R::Seconds)}, {2}, Rule::Bool),
        sig(I::TimeShift, {kX, p("t", R::Seconds)}, {2}, Rule::Series0),
        sig(I::PulseWidth, {kX}, {1}, Rule::Time),
        sig(I::PreviousSample, {kX, p("n", R::Count)}, {1, 2}, Rule::Series0),
        sig(I::RangeIs, {p("path", R::RangePath)}, {1}, Rule::Bool),
        sig(I::RangeChange, {p("path", R::RangePath)}, {1}, Rule::RangeCount),
        sig(I::VarianceDist, {kChan}, {1}, Rule::Series0),
        sig(I::VarianceTime, {kChan}, {1}, Rule::Series0),
    }};
    return t;
}

} // namespace

std::string Signature::aritiesText() const {
    std::string out;
    for (std::uint8_t i = 0; i < arityCount; ++i) {
        if (i > 0) {
            out += i + 1 == arityCount ? " or " : ", ";
        }
        out += std::to_string(arities[i]);
    }
    return out;
}

const Signature& signatureOf(IntrinsicId id) noexcept {
    const auto i = static_cast<std::size_t>(id);
    static constexpr Signature kNone{};
    return i < table().size() ? table()[i] : kNone;
}

std::optional<IntrinsicId> intrinsicByName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < static_cast<std::size_t>(IntrinsicId::Count); ++i) {
        const auto id = static_cast<IntrinsicId>(i);
        if (intrinsicName(id) == name) {
            return id;
        }
    }
    return std::nullopt;
}

} // namespace esses::maths::lang
