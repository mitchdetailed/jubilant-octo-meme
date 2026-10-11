// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "test_support.hpp"

#include <esses/base/int128.hpp>
#include <esses/base/time.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <random>

using esses::detail::S128;
using esses::detail::U128;

namespace {
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();
} // namespace

TEST_CASE("int128: platform paths agree with the portable reference", "[property][int128]") {
    std::mt19937_64 rng(esses::test::testSeed());
    std::uniform_int_distribution<int> shift(0, 63);
    int mismatches = 0;
    for (int k = 0; k < 200'000; ++k) {
        const std::uint64_t a = rng() >> shift(rng);
        const std::uint64_t b = rng() >> shift(rng);
        if (!(esses::detail::mulU64(a, b) == esses::detail::mulU64Portable(a, b))) {
            ++mismatches;
        }
        std::uint64_t d = rng() >> shift(rng);
        if (d == 0) {
            d = 1;
        }
        const U128 n{rng() >> shift(rng), rng()};
        std::uint64_t r1 = 0;
        std::uint64_t r2 = 0;
        const U128 q1 = esses::detail::divModU128(n, d, r1);
        const U128 q2 = esses::detail::divModU128Portable(n, d, r2);
        if (!(q1 == q2) || r1 != r2 || r1 >= d) {
            ++mismatches;
        }
        // q * d + r == n
        const U128 lo = esses::detail::mulU64(q1.lo, d);
        U128 back{lo.hi + q1.hi * d, lo.lo};
        const std::uint64_t sumLo = back.lo + r1;
        back.hi += sumLo < back.lo ? 1u : 0u;
        back.lo = sumLo;
        if (!(back == n)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("int128: known products and quotients", "[unit][int128]") {
    const U128 p = esses::detail::mulU64(kU64Max, kU64Max); // (2^64 - 1)^2 = 2^128 - 2^65 + 1
    CHECK(p.hi == kU64Max - 1);
    CHECK(p.lo == 1u);
    std::uint64_t r = 0;
    const U128 q = esses::detail::divModU128(U128{1, 0}, 3, r); // 2^64 / 3
    CHECK(q.hi == 0u);
    CHECK(q.lo == 6'148'914'691'236'517'205ull);
    CHECK(r == 1u);
}

TEST_CASE("int128: muldivFloor rounds toward minus infinity and saturates", "[unit][int128]") {
    CHECK(esses::muldivFloor(7, 3, 2) == 10);
    CHECK(esses::muldivFloor(-7, 3, 2) == -11);
    CHECK(esses::muldivFloor(-6, 3, 2) == -9);
    CHECK(esses::muldivFloor(0, 5, 3) == 0);
    CHECK(esses::muldivFloor(kI64Min, 1, 1) == kI64Min);
    CHECK(esses::muldivFloor(kI64Max, kU64Max, kU64Max) == kI64Max);
    CHECK(esses::muldivFloor(kI64Min, kU64Max, kU64Max) == kI64Min);
    CHECK(esses::muldivFloor(kI64Max, 4, 1) == kI64Max);
    CHECK(esses::muldivFloor(kI64Min, 2, 1) == kI64Min);
    CHECK(esses::muldivFloor(-1, 1, kU64Max) == -1);
    CHECK(esses::muldivFloor(1, 1, kU64Max) == 0);
}

TEST_CASE("int128: floorDiv128 and floorDiv", "[unit][int128]") {
    CHECK(esses::floorDiv128(0, 10, 3) == 3);
    CHECK(esses::floorDiv128(-1, kU64Max, 2) == -1);                     // -1 / 2
    CHECK(esses::floorDiv128(-1, ~std::uint64_t{9}, 3) == -4);           // -10 / 3
    CHECK(esses::floorDiv128(-1, ~std::uint64_t{8}, 3) == -3);           // -9 / 3
    CHECK(esses::floorDiv128(1, 0, 2) == kI64Max);                       // 2^63 saturates
    CHECK(esses::floorDiv128(0, std::uint64_t{1} << 63, 1) == kI64Max);  // 2^63 saturates
    CHECK(esses::floorDiv128(-1, std::uint64_t{1} << 63, 1) == kI64Min); // -2^63 fits
    CHECK(esses::floorDiv128(kI64Min, 0, 1) == kI64Min);                 // -2^127 saturates
    CHECK(esses::floorDiv(7, 2) == 3);
    CHECK(esses::floorDiv(-7, 2) == -4);
    CHECK(esses::floorDiv(7, -2) == -4);
    CHECK(esses::floorDiv(-7, -2) == 3);
    CHECK(esses::floorDiv(kI64Min, -1) == kI64Max);
    static_assert(esses::floorDiv(-1, 1'000'000) == -1);
}

TEST_CASE("int128: muldivFloor satisfies q*c <= a*b < (q+1)*c", "[property][int128]") {
    std::mt19937_64 rng(esses::test::testSeed() + 1);
    std::uniform_int_distribution<std::int64_t> anyA(kI64Min, kI64Max);
    int failures = 0;
    for (int k = 0; k < 200'000; ++k) {
        const std::int64_t a = anyA(rng);
        std::uint64_t c = rng() | 1u;
        std::uint64_t b = rng() % c; // b < c keeps |a*b/c| < 2^63
        const std::int64_t q = esses::muldivFloor(a, b, c);
        const S128 ab = S128::fromInt64(a).mulU64(b);
        const S128 qc = S128::fromInt64(q).mulU64(c);
        const S128 diff = ab.sub(qc); // must be in [0, c)
        if (diff.neg || diff.mag.hi != 0 || diff.mag.lo >= c) {
            ++failures;
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("time: saturating helpers and conversions", "[unit][time]") {
    using namespace esses;
    CHECK(saturatingAdd(kI64Max, 1) == kI64Max);
    CHECK(saturatingAdd(kI64Min, -1) == kI64Min);
    CHECK(saturatingAdd(5, -7) == -2);
    CHECK(saturatingSub(kI64Min, 1) == kI64Min);
    CHECK(saturatingSub(kI64Max, -1) == kI64Max);
    CHECK(microsFromSeconds(3) == 3'000'000);
    CHECK(microsFromSeconds(kI64Max / 10) == kI64Max);
    CHECK(microsFromMillis(-2) == -2'000);
    CHECK(microsToSeconds(1'500'000) == Catch::Approx(1.5));
    CHECK(microsFromSecondsF(0.1) == std::optional<Micros>(100'000));
    CHECK(microsFromSecondsF(-1.5) == std::optional<Micros>(-1'500'000));
    CHECK(microsFromSecondsF(0.0000014999) == std::optional<Micros>(1));
    CHECK(microsFromSecondsF(-0.0000015001) == std::optional<Micros>(-2));
    CHECK_FALSE(microsFromSecondsF(std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(microsFromSecondsF(1e300).has_value());
    CHECK(formatMicros(0) == "0:00:00.000000");
    CHECK(formatMicros(kMicrosPerDay + 61 * kMicrosPerSecond + 5) == "24:01:01.000005");
    CHECK(formatMicros(-1) == "-0:00:00.000001");
    CHECK(formatMicros(kI64Min) == "-2562047788:00:54.775808");
}
