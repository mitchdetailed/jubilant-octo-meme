// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M0-08: Rate property test (indexAt(timeAt(i)) == i for 10^6 random (num, den, i) including 33.333 Hz and
// 7.5 Hz; 24 h at 1 kHz has zero drift), plus the worked examples and ordering rules of 02 section 1.
#include "test_support.hpp"

#include <esses/base/int128.hpp>
#include <esses/base/rate.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <sstream>

using esses::Micros;
using esses::Rate;
using esses::SampleIdx;

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

// Reference timeAt for small operands: round-half-up of i * 1e6 * den / num in plain int64 (no overflow when
// |i| * 2e6 * den < 2^63).
Micros referenceTimeAt(const Rate& r, std::int64_t i) {
    const std::int64_t d2 = 2'000'000LL * static_cast<std::int64_t>(r.den);
    const std::int64_t n2 = 2LL * static_cast<std::int64_t>(r.num);
    return esses::floorDiv(i * d2 + static_cast<std::int64_t>(r.num), n2);
}

} // namespace

TEST_CASE("rate: worked examples of the data model", "[unit][rate]") {
    const Rate csv{512, 25}; // 20.48 Hz
    CHECK(csv.timeAt(1, 0) == 48'828);
    CHECK(csv.indexAt(48'828, 0) == 1);
    CHECK(csv.indexAt(48'827, 0) == 0);

    const Rate r30{30, 1};
    CHECK(r30.timeAt(1, 0) == 33'333);
    CHECK(r30.indexAt(33'333, 0) == 1);
    CHECK(r30.indexAt(33'332, 0) == 0);

    const Rate p = Rate::fromPeriodUs(33'333);
    CHECK(p.num == 1'000'000u);
    CHECK(p.den == 33'333u);

    const Rate r33{100, 3}; // 33.333 Hz: exactly 30 ms
    CHECK(r33.timeAt(1, 0) == 30'000);
    CHECK(r33.timeAt(3, 1'000) == 91'000);

    const Rate r75{15, 2}; // 7.5 Hz: 133'333.33 us
    CHECK(r75.timeAt(1, 0) == 133'333);
    CHECK(r75.timeAt(2, 0) == 266'667);
    CHECK(r75.timeAt(3, 0) == 400'000);
    CHECK(r75.indexAt(266'666, 0) == 1);
    CHECK(r75.indexAt(266'667, 0) == 2);
}

TEST_CASE("rate: value ordering and equality", "[unit][rate]") {
    CHECK(Rate{2, 2} == Rate{1, 1});
    CHECK(Rate{1, 2} < Rate{1, 1});
    CHECK(Rate{100, 3} > Rate{33, 1});
    CHECK(Rate{100, 3} < Rate{34, 1});
    CHECK((Rate{4'294'967'295u, 1} <=> Rate{4'294'967'294u, 1}) == std::strong_ordering::greater);
    CHECK(std::max(Rate{15, 2}, Rate{8, 1}) == Rate{8, 1});
    CHECK(Rate::fromHz(1000) == Rate{1000, 1});
    CHECK(Rate{6, 4}.normalised().num == 3u);
    CHECK(Rate{6, 4}.normalised().den == 2u);
    CHECK(Rate{100, 3}.toString() == "100/3 Hz");
    CHECK(Rate{2000, 2}.toString() == "1000 Hz");
    CHECK(Rate{512, 25}.hz() == Catch::Approx(20.48));
}

TEST_CASE("rate: invalid rates are inert", "[unit][rate]") {
    const Rate none{};
    CHECK_FALSE(none.valid());
    CHECK_FALSE(Rate{5, 0}.valid());
    CHECK(none.timeAt(42, 7) == 7);
    CHECK(none.indexAt(1'000, 0) == 0);
    CHECK(Rate{5, 0}.indexAt(1'000, 0) == 0);
    CHECK(none.toString() == "no rate");
    CHECK_FALSE(Rate::fromPeriodUs(0).valid());
    CHECK_FALSE(Rate::fromPeriodUs(4'294'967'297ull).valid());               // coprime to 1e6: den > 2^32 - 1
    CHECK(Rate::fromPeriodUs(10'000'000'000'000ull) == Rate{1, 10'000'000}); // gcd keeps it valid
    CHECK(Rate{5, 0}.normalised() == Rate{});
}

TEST_CASE("rate: fromPeriodUs is exact and reduced", "[unit][rate]") {
    CHECK(Rate::fromPeriodUs(1'000) == Rate{1000, 1});
    CHECK(Rate::fromPeriodUs(1'000).den == 1u);
    CHECK(Rate::fromPeriodUs(30'000).num == 100u);
    CHECK(Rate::fromPeriodUs(30'000).den == 3u);
    CHECK(Rate::fromPeriodUs(48'828).num == 250'000u);
    CHECK(Rate::fromPeriodUs(48'828).den == 12'207u);
    CHECK(Rate::fromPeriodUs(1).num == 1'000'000u);
}

TEST_CASE("rate: negative indices before t0 floor toward minus infinity", "[unit][rate]") {
    const Rate r{3, 1}; // 333'333.33 us
    CHECK(r.timeAt(-1, 0) == -333'333);
    CHECK(r.timeAt(-2, 0) == -666'667);
    CHECK(r.indexAt(-1, 0) == -1);
    CHECK(r.indexAt(-333'333, 0) == -1);
    CHECK(r.indexAt(-333'334, 0) == -2);
    CHECK(r.indexAt(0, 0) == 0);
    CHECK(r.indexAt(-5'000'000, -5'000'000) == 0);
    CHECK(r.indexAt(-5'000'001, -5'000'000) == -1);
}

TEST_CASE("rate: extreme operands saturate instead of overflowing", "[unit][rate]") {
    const Rate slow{1, 4'294'967'295u}; // one sample every ~136 years
    CHECK(slow.timeAt(kI64Max, 0) == kI64Max);
    CHECK(slow.timeAt(kI64Min, 0) == kI64Min);
    CHECK(slow.indexAt(kI64Max, kI64Min) == 4294); // floor((2^65 - 2) / (2 * 1e6 * (2^32 - 1)))
    const Rate fast{4'294'967'295u, 1};
    CHECK(fast.indexAt(kI64Max, kI64Min) == kI64Max); // (2^64 - 1) us * 4.29 GHz saturates
    CHECK(fast.indexAt(kI64Min, kI64Max) == kI64Min);
    CHECK(Rate{1000, 1}.timeAt(kI64Max / 10, kI64Max / 2) == kI64Max);
}

TEST_CASE("rate: indexAt inverts timeAt for 10^6 random (num, den, i)", "[property][rate]") {
    std::mt19937_64 rng(esses::test::testSeed());
    // Special rates first, including 33.333 Hz (100/3) and 7.5 Hz (15/2).
    constexpr std::array<Rate, 14> kSpecial{{{100, 3},
                                             {15, 2},
                                             {512, 25},
                                             {30, 1},
                                             {1000, 1},
                                             {1, 1},
                                             {1'000'000, 33'333},
                                             {1'000'000, 1},
                                             {60, 1},
                                             {20, 1},
                                             {4'294'967'295u, 4'295u},
                                             {1, 4'294'967'295u},
                                             {4'294'967'295u, 4'294'967'295u},
                                             {999'999, 1}}};
    std::uniform_int_distribution<std::uint32_t> any32(1, std::numeric_limits<std::uint32_t>::max());
    std::uniform_int_distribution<std::uint32_t> small(1, 100'000);
    std::uniform_int_distribution<int> pick(0, 9);
    constexpr int kIterations = 1'000'000;
    int failures = 0;
    std::string firstFailure;
    for (int k = 0; k < kIterations; ++k) {
        Rate r;
        const int mode = pick(rng);
        if (k < static_cast<int>(kSpecial.size()) * 1000) {
            r = kSpecial[static_cast<std::size_t>(k) % kSpecial.size()];
        } else if (mode < 4) {
            r = Rate{small(rng), small(rng)};
        } else {
            r = Rate{any32(rng), any32(rng)};
        }
        // The inverse holds when the period is at least 1 us (num <= 1e6 * den); fold faster rates into
        // range.
        const std::uint64_t d = 1'000'000ull * r.den;
        if (r.num > d) {
            r.num = static_cast<std::uint32_t>(1u + (r.num % d));
        }
        // Keep |timeAt| well inside int64: |i| <= 2^62 * num / D, and at most 2^40.
        const std::int64_t bound = std::min<std::int64_t>(esses::muldivFloor(std::int64_t{1} << 62, r.num, d),
                                                          std::int64_t{1} << 40);
        std::uniform_int_distribution<std::int64_t> idx(-bound, bound);
        std::uniform_int_distribution<std::int64_t> origin(-(std::int64_t{1} << 40), std::int64_t{1} << 40);
        const SampleIdx i = idx(rng);
        const Micros t0 = mode == 9 ? 0 : origin(rng);
        const Micros t = r.timeAt(i, t0);
        const SampleIdx back = r.indexAt(t, t0);
        const SampleIdx before = r.indexAt(t - 1, t0); // step >= 1 us, so t - 1 belongs to sample i - 1
        if (back != i || before != i - 1) {
            if (failures++ == 0) {
                std::ostringstream os;
                os << "rate " << r.num << "/" << r.den << " i=" << i << " t0=" << t0 << " t=" << t
                   << " indexAt(t)=" << back << " indexAt(t-1)=" << before;
                firstFailure = os.str();
            }
        }
    }
    INFO(firstFailure);
    CHECK(failures == 0);
}

TEST_CASE("rate: timeAt equals exact round-half-up of i times the period", "[property][rate]") {
    std::mt19937_64 rng(esses::test::testSeed() ^ 0xABCDu);
    std::uniform_int_distribution<std::uint32_t> num(1, 1'000'000);
    std::uniform_int_distribution<std::uint32_t> den(1, 1'000);
    std::uniform_int_distribution<std::int64_t> idx(-1'000'000, 1'000'000);
    int failures = 0;
    for (int k = 0; k < 200'000; ++k) {
        const Rate r{num(rng), den(rng)};
        const std::int64_t i = idx(rng);
        if (r.timeAt(i, 0) != referenceTimeAt(r, i)) {
            ++failures;
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("rate: 24 h at 1 kHz has zero drift", "[property][rate]") {
    const Rate r{1000, 1};
    constexpr std::int64_t kSamples = 86'400'000; // 24 h at 1 kHz
    CHECK(r.timeAt(kSamples, 0) == 86'400'000'000LL);
    CHECK(r.indexAt(86'400'000'000LL, 0) == kSamples);
    std::int64_t bad = 0;
    const auto check = [&](std::int64_t i) {
        if (r.timeAt(i, 0) != i * 1000 || r.indexAt(i * 1000, 0) != i || r.indexAt(i * 1000 + 999, 0) != i) {
            ++bad;
        }
    };
    constexpr std::int64_t kDense = 1'800'000; // every sample of the first and last 30 minutes
    for (std::int64_t i = 0; i < kDense; ++i) {
        check(i);
    }
    for (std::int64_t i = kSamples - kDense; i <= kSamples; ++i) {
        check(i);
    }
    for (std::int64_t i = kDense; i < kSamples - kDense; i += 997) { // the rest, strided
        check(i);
    }
    CHECK(bad == 0);

    // The same day at 33.333 Hz, 7.5 Hz and 20.48 Hz equals the exact rounded reference at every 7th sample.
    for (const Rate odd : {Rate{100, 3}, Rate{15, 2}, Rate{512, 25}}) {
        const std::int64_t n = r.indexAt(86'400'000'000LL, 0) / 1000 * static_cast<std::int64_t>(odd.num) /
                               static_cast<std::int64_t>(odd.den);
        std::int64_t mismatches = 0;
        for (std::int64_t i = 0; i <= n; i += 7) {
            if (odd.timeAt(i, 0) != referenceTimeAt(odd, i) || odd.indexAt(odd.timeAt(i, 0), 0) != i) {
                ++mismatches;
            }
        }
        INFO("rate " << odd.toString());
        CHECK(mismatches == 0);
        CHECK(odd.timeAt(n, 0) == referenceTimeAt(odd, n));
    }
}
