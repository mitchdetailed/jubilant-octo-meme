// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DistanceIndex (D-016, D-099, D-140; AC-M1-08): distAt(timeAt(d)) == d +- 0.25 m on 1,000 random distances
// per lap, and a stopped car returns the first time each distance is reached.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

DistanceIndexKey keyFor(Micros start, Micros end) {
    DistanceIndexKey k;
    k.start = start;
    k.end = end;
    k.corrDistVersion = 1;
    return k;
}

// Micros of a lap-relative time in seconds, as DomainMapper converts it (D-099).
Micros lapTime(const DistanceIndex& idx, double seconds) {
    return idx.lapStart() + static_cast<Micros>(std::llround(seconds * 1e6));
}

// Linear interpolation of the samples (the reference distAt), clamped at both ends.
double reference(const std::vector<Micros>& t, const std::vector<double>& d, Micros at) {
    if (at <= t.front()) {
        return d.front();
    }
    if (at >= t.back()) {
        return d.back();
    }
    const auto hi = static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), at) - t.begin());
    const std::size_t lo = hi - 1;
    return d[lo] + static_cast<double>(at - t[lo]) / static_cast<double>(t[hi] - t[lo]) * (d[hi] - d[lo]);
}

} // namespace

TEST_CASE("distance_index: distAt(timeAt(d)) == d +- 0.25 m on 1,000 random distances per lap",
          "[property][AC-M1-08]") {
    std::mt19937_64 rng(testSeed());
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const std::uint32_t rates[] = {10, 20, 50, 100, 1000, 3};
    for (int lapNo = 0; lapNo < 24; ++lapNo) {
        const Rate rate = Rate::fromHz(rates[static_cast<std::size_t>(lapNo) % std::size(rates)]);
        // A speed profile of random segments: accelerations, cruising, braking and stops (plateaus).
        const std::size_t n = 2000 + static_cast<std::size_t>(unit(rng) * 3000);
        UniformSignal speed;
        speed.rate = rate;
        speed.values.resize(n);
        double v = 30.0 * unit(rng);
        double maxV = 0.0;
        for (std::size_t i = 0; i < n;) {
            const std::size_t len = 1 + static_cast<std::size_t>(unit(rng) * 300);
            const double kind = unit(rng);
            const double target = kind < 0.2 ? 0.0 : 80.0 * unit(rng);
            for (std::size_t k = 0; k < len && i < n; ++k, ++i) {
                v += (target - v) * (kind < 0.2 ? 1.0 : 0.05);
                speed.values[i] = std::max(0.0, v);
                maxV = std::max(maxV, speed.values[i]);
            }
        }
        auto dist = CorrectedDistance::integrate(speed);
        REQUIRE(dist);
        std::vector<Micros> times(n);
        for (std::size_t i = 0; i < n; ++i) {
            times[i] = rate.timeAt(static_cast<SampleIdx>(i), 0);
        }
        // A lap between two random times that need not be sample times.
        const Micros span = times.back();
        const auto a = static_cast<Micros>(unit(rng) * 0.3 * static_cast<double>(span));
        const Micros b = span - static_cast<Micros>(unit(rng) * 0.3 * static_cast<double>(span));
        auto idx = DistanceIndex::fromSamples(times, dist->values, keyFor(a, b));
        REQUIRE(idx);
        INFO("lap " << lapNo << " at " << rate.toString() << ", length " << idx->lengthM() << " m");
        CHECK(idx->lengthM() ==
              Catch::Approx(reference(times, dist->values, b) - reference(times, dist->values, a))
                  .margin(1e-9));
        // D-099 rounds timeAt to whole microseconds: allow the distance covered in half a microsecond.
        const double tolerance = DistanceIndex::kStepM + 0.5e-6 * maxV + 1e-9;
        for (int k = 0; k < 1000; ++k) {
            const double d = unit(rng) * idx->lengthM();
            const double t = idx->timeAt(d);
            REQUIRE(t >= 0.0);
            REQUIRE(t <= static_cast<double>(b - a) / 1e6 + 1e-9);
            const double back = idx->distAt(lapTime(*idx, t));
            if (std::abs(back - d) > tolerance) {
                FAIL_CHECK("d = " << d << ", timeAt = " << t << " s, distAt = " << back);
            }
        }
        // The table steps are the first times each 0.25 m is reached, so they never decrease.
        for (double d = 0.0; d + DistanceIndex::kStepM <= idx->lengthM(); d += DistanceIndex::kStepM) {
            REQUIRE(idx->timeAt(d) <= idx->timeAt(d + DistanceIndex::kStepM));
        }
        CHECK(idx->steps() ==
              static_cast<std::size_t>(std::floor(idx->lengthM() / DistanceIndex::kStepM)) + 1);
    }
}

TEST_CASE("distance_index: a stopped car returns the first time each distance is reached",
          "[unit][AC-M1-08]") {
    // 10 Hz: 20 m/s for 10 s (200 m), stopped 30 s, 20 m/s for 10 s (400 m), as an exact cumulative distance.
    std::vector<Micros> times;
    std::vector<double> dist;
    for (int i = 0; i <= 500; ++i) {
        const double t = i / 10.0;
        times.push_back(sec(t));
        dist.push_back(t <= 10.0 ? 20.0 * t : t <= 40.0 ? 200.0 : 200.0 + 20.0 * (t - 40.0));
    }
    auto idx = DistanceIndex::fromSamples(times, dist, keyFor(0, sec(50)));
    REQUIRE(idx);
    CHECK(idx->lengthM() == Catch::Approx(400.0));
    CHECK(idx->timeAt(0.0) == 0.0);
    CHECK(idx->timeAt(100.0) == Catch::Approx(5.0).margin(1e-12));
    CHECK(idx->timeAt(200.0) == Catch::Approx(10.0).margin(1e-12)); // the first time, not 40 s
    CHECK(idx->timeAt(199.75) == Catch::Approx(9.9875).margin(1e-12));
    CHECK(idx->timeAt(200.25) == Catch::Approx(40.0125).margin(1e-9));
    CHECK(idx->timeAt(400.0) == Catch::Approx(50.0).margin(1e-12));
    CHECK(idx->distAt(sec(25)) == Catch::Approx(200.0));
    CHECK(idx->distAt(sec(40)) == Catch::Approx(200.0));
    // A distance inside the plateau step is first reached after the restart (40 s + 0.1 m at 20 m/s), not on
    // the straight line between the step's ends; one just below the plateau before the stop.
    const double t = idx->timeAt(200.1);
    CHECK(t == Catch::Approx(40.005).margin(1e-9));
    CHECK(std::abs(idx->distAt(lapTime(*idx, t)) - 200.1) <= 1e-6);
    CHECK(idx->timeAt(199.9) == Catch::Approx(9.995).margin(1e-9));
    // Between two grid points away from a stop the result is the same as the step table's interpolation.
    CHECK(idx->timeAt(100.1) == Catch::Approx(5.005).margin(1e-12));
    // A noisy logged distance that steps back is still indexed by first reach.
    const std::vector<Micros> nt = {0, sec(1), sec(2), sec(3), sec(4)};
    const std::vector<double> nd = {0.0, 10.0, 9.0, 10.5, 20.0};
    auto noisy = DistanceIndex::fromSamples(nt, nd, keyFor(0, sec(4)));
    REQUIRE(noisy);
    CHECK(noisy->timeAt(10.0) == Catch::Approx(1.0));
    CHECK(noisy->timeAt(10.25) == Catch::Approx(2.0 + 1.25 / 1.5));
    CHECK(noisy->distAt(sec(2)) == Catch::Approx(9.0));
    CHECK(noisy->lengthM() == Catch::Approx(20.0));
}

TEST_CASE("distance_index: distAt is relative to the lap start and interpolated at the bounds",
          "[unit][AC-M1-08]") {
    // 1 Hz samples of a cumulative distance; the lap runs from 2.5 s to 6.25 s.
    const std::vector<Micros> t = {0, sec(1), sec(2), sec(3), sec(4), sec(5), sec(6), sec(7), sec(8)};
    const std::vector<double> d = {0, 10, 20, 40, 60, 80, 100, 120, 140};
    auto idx = DistanceIndex::fromSamples(t, d, keyFor(sec(2.5), sec(6.25)));
    REQUIRE(idx);
    CHECK(idx->lapStart() == sec(2.5));
    CHECK(idx->lapEnd() == sec(6.25));
    CHECK(idx->distAt(sec(2.5)) == 0.0);
    CHECK(idx->distAt(sec(3)) == Catch::Approx(10.0));   // 40 - 30
    CHECK(idx->distAt(sec(3.5)) == Catch::Approx(20.0)); // 50 - 30
    CHECK(idx->distAt(sec(6.25)) == Catch::Approx(75.0));
    CHECK(idx->distAt(0) == 0.0); // clamped to the lap
    CHECK(idx->distAt(sec(9)) == Catch::Approx(75.0));
    CHECK(idx->lengthM() == Catch::Approx(75.0));
    CHECK(idx->timeAt(10.0) == Catch::Approx(0.5));
    CHECK(idx->timeAt(75.0) == Catch::Approx(3.75));
    CHECK(idx->timeAt(1000.0) == Catch::Approx(3.75)); // clamped to the length
    CHECK(idx->timeAt(-5.0) == 0.0);
    CHECK(idx->indexAt(10.0, Rate::fromHz(10)) == 5);  // 0.5 s at 10 Hz
    CHECK(idx->indexAt(10.05, Rate::fromHz(10)) == 5); // 0.5025 s: the sample at or before
    CHECK(idx->indexAt(10.0, Rate{}) == 0);
    CHECK(idx->steps() == 301); // 0 .. 75 m in 0.25 m steps
}

TEST_CASE("distance_index: build from a Corr Dist series equals fromSamples; invalid samples hold",
          "[unit][AC-M1-08]") {
    TestCache tc;
    // 20 Hz, 400 samples, with invalid samples inside the lap and at the start of the series.
    std::vector<double> d(400);
    std::vector<bool> valid(400, true);
    for (std::size_t i = 0; i < d.size(); ++i) {
        const double t = static_cast<double>(i) / 20.0;
        d[i] = 3.0 * t * t + 5.0 * t;
    }
    for (std::size_t i : {0u, 1u, 120u, 121u, 122u, 250u}) {
        valid[i] = false;
    }
    const auto series = tc.uniform(Rate::fromHz(20), d, valid);
    const DistanceIndexKey key = keyFor(sec(2.03), sec(15.98));
    auto built = DistanceIndex::build(series, key, Priority::Interactive, {});
    REQUIRE(built);

    // The reference: invalid samples replaced by the previous valid value (leading ones by the first valid).
    std::vector<double> held = d;
    held[0] = held[1] = d[2];
    held[120] = held[121] = held[122] = d[119];
    held[250] = d[249];
    std::vector<Micros> times(d.size());
    for (std::size_t i = 0; i < d.size(); ++i) {
        times[i] = Rate::fromHz(20).timeAt(static_cast<SampleIdx>(i), 0);
    }
    auto ref = DistanceIndex::fromSamples(times, held, key);
    REQUIRE(ref);
    CHECK(built->lengthM() == Catch::Approx(ref->lengthM()).epsilon(1e-12));
    CHECK(built->steps() == ref->steps());
    for (Micros t = key.start; t <= key.end; t += 12'345) {
        REQUIRE(built->distAt(t) == Catch::Approx(ref->distAt(t)).epsilon(1e-12));
    }
    for (double m = 0.0; m <= built->lengthM(); m += 0.37) {
        REQUIRE(built->timeAt(m) == Catch::Approx(ref->timeAt(m)).epsilon(1e-12));
    }
    // distAt with the uniform O(1) path matches the reference interpolation of the held samples.
    const double d0 = reference(times, held, key.start);
    for (Micros t = key.start; t <= key.end; t += 7'777) {
        REQUIRE(built->distAt(t) == Catch::Approx(reference(times, held, t) - d0).margin(1e-9));
    }
    CHECK(built->key() == key);

    // The same samples as a timestamped series.
    std::vector<std::int64_t> ts(times.begin(), times.end());
    const auto stamped = tc.timestamped(ts, held);
    auto fromStamped = DistanceIndex::build(stamped, key, Priority::Interactive, {});
    REQUIRE(fromStamped);
    CHECK(fromStamped->lengthM() == Catch::Approx(ref->lengthM()).epsilon(1e-12));
    CHECK(fromStamped->timeAt(100.0) == Catch::Approx(ref->timeAt(100.0)).epsilon(1e-12));
}

TEST_CASE("distance_index: errors and the empty index", "[unit]") {
    TestCache tc;
    const DistanceIndex empty;
    CHECK(empty.lengthM() == 0.0);
    CHECK(empty.distAt(123) == 0.0);
    CHECK(empty.timeAt(5.0) == 0.0);
    CHECK(empty.indexAt(5.0, Rate::fromHz(10)) == 0);
    CHECK(empty.steps() == 0);

    const std::vector<Micros> t = {0, 10, 20};
    const std::vector<double> d = {0, 1, 2};
    CHECK(DistanceIndex::fromSamples(t, d, keyFor(10, 10)).error().code == ErrorCode::Invalid);
    CHECK(DistanceIndex::fromSamples(t, std::vector<double>{0, 1}, keyFor(0, 20)).error().code ==
          ErrorCode::Invalid);
    CHECK(DistanceIndex::fromSamples(std::vector<Micros>{0, 10, 10}, d, keyFor(0, 20)).error().code ==
          ErrorCode::Invalid);
    CHECK(DistanceIndex::fromSamples(t, std::vector<double>{0, std::numeric_limits<double>::quiet_NaN(), 2},
                                     keyFor(0, 20))
              .error()
              .code == ErrorCode::Invalid);
    CHECK(DistanceIndex::fromSamples({}, {}, keyFor(0, 20)).error().code == ErrorCode::Invalid);

    const auto allInvalid = tc.uniform(Rate::fromHz(10), {1, 2, 3}, {false, false, false});
    CHECK(DistanceIndex::build(allInvalid, keyFor(0, sec(0.2)), Priority::Interactive, {}).error().code ==
          ErrorCode::NotFound);
    CancelSource cancel;
    cancel.cancel();
    const auto ok = tc.uniform(Rate::fromHz(10), {1, 2, 3});
    CHECK(DistanceIndex::build(ok, keyFor(0, sec(0.2)), Priority::Interactive, cancel.token()).error().code ==
          ErrorCode::Cancelled);
    // A car that never moves: length 0, every distance at the lap start.
    auto still = DistanceIndex::fromSamples(t, std::vector<double>{5, 5, 5}, keyFor(0, 20));
    REQUIRE(still);
    CHECK(still->lengthM() == 0.0);
    CHECK(still->timeAt(3.0) == 0.0);
    CHECK(still->steps() == 1);
}
