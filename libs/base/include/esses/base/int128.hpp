// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Exact 128-bit helpers (02 section 1 behaviour 1, D-129). Every product is formed in 128 bits and every
// division rounds toward -infinity. GCC/Clang use unsigned __int128; MSVC x64 uses _umul128/_udiv128; other
// targets use a portable schoolbook path (also exposed for cross-checking in tests).
//
// Results that do not fit in int64 saturate to INT64_MIN/INT64_MAX (the design leaves overflow unspecified;
// saturation keeps the functions total and noexcept). A zero divisor is a precondition violation: it asserts
// in debug builds and saturates toward the sign of the dividend otherwise.
#pragma once

#include <esses/base/config.hpp>

#include <cstdint>
#include <limits>

namespace esses {

// floor(a * b / c), c > 0.
std::int64_t muldivFloor(std::int64_t a, std::uint64_t b, std::uint64_t c) noexcept;

// floor(v / d) where v is the signed (two's complement) 128-bit value hi * 2^64 + lo, d > 0.
std::int64_t floorDiv128(std::int64_t hi, std::uint64_t lo, std::uint64_t d) noexcept;

// floor(a / b) for int64, b != 0; INT64_MIN / -1 saturates to INT64_MAX.
constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    if (b == -1) {
        return a == std::numeric_limits<std::int64_t>::min() ? std::numeric_limits<std::int64_t>::max() : -a;
    }
    const std::int64_t q = a / b;
    const std::int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

namespace detail {

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    friend constexpr bool operator==(const U128&, const U128&) = default;
};

// Sign-magnitude 128-bit integer used by Rate. Precondition for every operation: no result exceeds 2^127.
struct S128 {
    bool neg = false; // never true for a zero magnitude
    U128 mag;

    static S128 fromInt64(std::int64_t v) noexcept;
    static S128 fromUint64(std::uint64_t v) noexcept;
    S128 add(const S128& o) const noexcept;
    S128 sub(const S128& o) const noexcept;
    S128 mulU64(std::uint64_t m) const noexcept;
    std::int64_t floorDivU64(std::uint64_t d) const noexcept; // saturating
};

U128 mulU64(std::uint64_t a, std::uint64_t b) noexcept;                              // full 64x64 product
U128 divModU128(U128 n, std::uint64_t d, std::uint64_t& remainder) noexcept;         // d != 0
U128 mulU64Portable(std::uint64_t a, std::uint64_t b) noexcept;                      // reference path
U128 divModU128Portable(U128 n, std::uint64_t d, std::uint64_t& remainder) noexcept; // reference path

} // namespace detail
} // namespace esses
