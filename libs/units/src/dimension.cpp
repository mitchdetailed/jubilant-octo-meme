// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/units/dimension.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace esses::units {

namespace {

constexpr std::int64_t kMaxExp = 127;

bool fits(std::int64_t v) noexcept {
    return v >= -kMaxExp && v <= kMaxExp;
}

// Unicode superscripts for exponents: U+2070, U+00B9, U+00B2, U+00B3, U+2074 ... U+2079; minus U+207B.
constexpr std::array<std::string_view, 10> kSuperscriptDigits = {"⁰", "¹", "²", "³", "⁴",
                                                                 "⁵", "⁶", "⁷", "⁸", "⁹"};
constexpr std::array<std::string_view, Dimension::kCount> kUnicodeSymbols = {"L", "M", "T", "I",
                                                                             "Θ", "N", "J"};
constexpr std::array<std::string_view, Dimension::kCount> kAsciiSymbols = {"L",     "M", "T", "I",
                                                                           "Theta", "N", "J"};

void appendSuperscript(std::string& out, int e) {
    if (e < 0) {
        out += "⁻";
        e = -e;
    }
    const std::string digits = std::to_string(e);
    for (const char c : digits) {
        out += kSuperscriptDigits[static_cast<std::size_t>(c - '0')];
    }
}

template <class Exponent>
std::string format(const Dimension& d, std::string_view separator, Exponent&& exponent,
                   const std::array<std::string_view, Dimension::kCount>& symbols) {
    if (d.isUnknown()) {
        return "?";
    }
    if (d.isDimensionless()) {
        return "1";
    }
    std::string out;
    for (std::size_t i = 0; i < Dimension::kCount; ++i) {
        const int e = int{d.exp[i]};
        if (e == 0) {
            continue;
        }
        if (!out.empty()) {
            out += separator;
        }
        out += symbols[i];
        if (e != 1) {
            exponent(out, e);
        }
    }
    return out;
}

} // namespace

Dimension Dimension::operator*(const Dimension& rhs) const noexcept {
    if (isUnknown() || rhs.isUnknown()) {
        return unknown();
    }
    Dimension out;
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::int64_t v = std::int64_t{exp[i]} + std::int64_t{rhs.exp[i]};
        if (!fits(v)) {
            return unknown();
        }
        out.exp[i] = static_cast<std::int8_t>(v);
    }
    return out;
}

Dimension Dimension::operator/(const Dimension& rhs) const noexcept {
    if (isUnknown() || rhs.isUnknown()) {
        return unknown();
    }
    Dimension out;
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::int64_t v = std::int64_t{exp[i]} - std::int64_t{rhs.exp[i]};
        if (!fits(v)) {
            return unknown();
        }
        out.exp[i] = static_cast<std::int8_t>(v);
    }
    return out;
}

Dimension Dimension::inverse() const noexcept {
    return none() / *this;
}

Dimension Dimension::pow(int n) const noexcept {
    return pow(Rational{n, 1});
}

Dimension Dimension::pow(Rational r) const noexcept {
    if (isUnknown() || r.den == 0) {
        return unknown();
    }
    if (isDimensionless()) {
        return none();
    }
    std::int64_t num = r.num;
    std::int64_t den = r.den;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    Dimension out;
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::int64_t p = std::int64_t{exp[i]} * num;
        if (p % den != 0) {
            return unknown();
        }
        const std::int64_t v = p / den;
        if (!fits(v)) {
            return unknown();
        }
        out.exp[i] = static_cast<std::int8_t>(v);
    }
    return out;
}

std::string Dimension::toString() const {
    return format(*this, "·", appendSuperscript, kUnicodeSymbols);
}

std::string Dimension::toAsciiString() const {
    return format(*this, ".", [](std::string& out, int e) { out += "^" + std::to_string(e); }, kAsciiSymbols);
}

} // namespace esses::units
