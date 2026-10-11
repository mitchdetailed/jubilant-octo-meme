// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>
#include <esses/base/int128.hpp>

#include <limits>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#define ESSES_INT128_MSVC_X64 1
#endif

namespace esses {
namespace detail {
namespace {

constexpr std::uint64_t kLow32 = 0xFFFF'FFFFull;
constexpr std::uint64_t kInt64MaxU = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t kInt64MinMagnitude = kInt64MaxU + 1u; // 2^63

#if defined(ESSES_HAVE_INT128) && !defined(ESSES_INT128_MSVC_X64)
// __extension__ keeps -Wpedantic quiet about __int128 and needs the typedef form.
// NOLINTNEXTLINE(modernize-use-using)
__extension__ typedef unsigned __int128 NativeU128;

NativeU128 toNative(U128 v) noexcept {
    return (static_cast<NativeU128>(v.hi) << 64) | v.lo;
}
U128 fromNative(NativeU128 v) noexcept {
    return U128{static_cast<std::uint64_t>(v >> 64), static_cast<std::uint64_t>(v)};
}
#endif

U128 add128(U128 a, U128 b) noexcept {
    U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
}

U128 sub128(U128 a, U128 b) noexcept { // a >= b
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u);
    return r;
}

bool less128(U128 a, U128 b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}

bool isZero(U128 v) noexcept {
    return v.hi == 0 && v.lo == 0;
}

} // namespace

U128 mulU64Portable(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t aLo = a & kLow32;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & kLow32;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t p0 = aLo * bLo;
    const std::uint64_t p1 = aLo * bHi;
    const std::uint64_t p2 = aHi * bLo;
    const std::uint64_t p3 = aHi * bHi;
    const std::uint64_t mid = (p0 >> 32) + (p1 & kLow32) + (p2 & kLow32);
    U128 r;
    r.lo = (mid << 32) | (p0 & kLow32);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return r;
}

U128 divModU128Portable(U128 n, std::uint64_t d, std::uint64_t& remainder) noexcept {
    if (n.hi == 0) {
        remainder = n.lo % d;
        return U128{0, n.lo / d};
    }
    U128 q;
    q.hi = n.hi / d;
    std::uint64_t r = n.hi % d; // r < d, so the low quotient word fits in 64 bits
    std::uint64_t qlo = 0;
    for (int bit = 63; bit >= 0; --bit) {
        const bool carry = (r >> 63) != 0;
        r = (r << 1) | ((n.lo >> bit) & 1u);
        qlo <<= 1;
        if (carry ||
            r >= d) { // with carry the true value is r + 2^64 >= d; the wrap-around gives the remainder
            r -= d;
            qlo |= 1u;
        }
    }
    q.lo = qlo;
    remainder = r;
    return q;
}

U128 mulU64(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(ESSES_INT128_MSVC_X64)
    U128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
#elif defined(ESSES_HAVE_INT128)
    return fromNative(static_cast<NativeU128>(a) * b);
#else
    return mulU64Portable(a, b);
#endif
}

U128 divModU128(U128 n, std::uint64_t d, std::uint64_t& remainder) noexcept {
#if defined(ESSES_INT128_MSVC_X64)
    if (n.hi == 0) {
        remainder = n.lo % d;
        return U128{0, n.lo / d};
    }
    U128 q;
    q.hi = n.hi / d;
    const std::uint64_t r = n.hi % d; // r < d: _udiv128 cannot overflow
    q.lo = _udiv128(r, n.lo, d, &remainder);
    return q;
#elif defined(ESSES_HAVE_INT128)
    const NativeU128 nn = toNative(n);
    remainder = static_cast<std::uint64_t>(nn % d);
    return fromNative(nn / d);
#else
    return divModU128Portable(n, d, remainder);
#endif
}

S128 S128::fromInt64(std::int64_t v) noexcept {
    S128 r;
    if (v < 0) {
        r.neg = true;
        r.mag.lo = ~static_cast<std::uint64_t>(v) + 1u; // also correct for INT64_MIN
    } else {
        r.mag.lo = static_cast<std::uint64_t>(v);
    }
    return r;
}

S128 S128::fromUint64(std::uint64_t v) noexcept {
    S128 r;
    r.mag.lo = v;
    return r;
}

S128 S128::add(const S128& o) const noexcept {
    S128 r;
    if (neg == o.neg) {
        r.neg = neg;
        r.mag = add128(mag, o.mag);
    } else if (less128(mag, o.mag)) {
        r.neg = o.neg;
        r.mag = sub128(o.mag, mag);
    } else {
        r.neg = neg;
        r.mag = sub128(mag, o.mag);
    }
    if (isZero(r.mag)) {
        r.neg = false;
    }
    return r;
}

S128 S128::sub(const S128& o) const noexcept {
    S128 negated = o;
    negated.neg = !isZero(o.mag) && !o.neg;
    return add(negated);
}

S128 S128::mulU64(std::uint64_t m) const noexcept {
    S128 r;
    const U128 low = detail::mulU64(mag.lo, m);
    r.mag.lo = low.lo;
    r.mag.hi = low.hi + mag.hi * m; // precondition: the product fits in 128 bits
    r.neg = neg && !isZero(r.mag);
    return r;
}

std::int64_t S128::floorDivU64(std::uint64_t d) const noexcept {
    if (d == 0) {
        ESSES_ASSERT_MSG(d != 0, "floor division by zero");
        if (isZero(mag)) {
            return 0;
        }
        return neg ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
    }
    std::uint64_t rem = 0;
    U128 q = divModU128(mag, d, rem);
    if (!neg) {
        if (q.hi != 0 || q.lo > kInt64MaxU) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(q.lo);
    }
    if (rem != 0) { // floor of a negative non-integer quotient: one further from zero
        q = add128(q, U128{0, 1});
    }
    if (q.hi != 0 || q.lo >= kInt64MinMagnitude) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(q.lo);
}

} // namespace detail

std::int64_t muldivFloor(std::int64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    return detail::S128::fromInt64(a).mulU64(b).floorDivU64(c);
}

std::int64_t floorDiv128(std::int64_t hi, std::uint64_t lo, std::uint64_t d) noexcept {
    detail::S128 v;
    if (hi < 0) { // two's complement negate of the 128-bit value
        v.neg = true;
        v.mag.lo = ~lo + 1u;
        v.mag.hi = ~static_cast<std::uint64_t>(hi) + (v.mag.lo == 0 ? 1u : 0u);
    } else {
        v.mag.hi = static_cast<std::uint64_t>(hi);
        v.mag.lo = lo;
    }
    return v.floorDivU64(d);
}

} // namespace esses
