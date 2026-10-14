// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportEngine::statistic (04 section 12 item 3, AC-M3-03): every statistic against values computed by hand
// in the comments below, including the 5 s probe, the boundary interpolation by each Interp policy,
// timestamped series, chunk-crossing ranges with invalid runs and F32 storage.
#include "report_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

StatValue measure(const store::Series& s, ReportStat which, double startS, double endS,
                  const ReportOptions& options = {}, store::Interp interp = store::Interp::Linear) {
    Result<StatValue> v = ReportEngine::statistic(s, which, sec(startS), sec(endS), options, {}, interp);
    REQUIRE(v.ok());
    return *v;
}

// 10 Hz from t = 0: samples 0..19. Inside [0.5 s, 1.5 s) (samples 5..14) sample 5 and sample 14 are invalid
// and samples 6..13 hold 4, -10, 3, 1, 5, 2, 3, 8. Outside the range every sample is 9.
//   n = 8, sum = 16, mean = 2
//   deviations 2, -12, 1, -1, 3, 0, 1, 6 -> squares 4 + 144 + 1 + 1 + 9 + 0 + 1 + 36 = 196 -> variance 24.5
//   min -10 at 0.7 s, max 8 at 1.3 s, absMax |-10| = 10 at 0.7 s, delta 18
//   start: sample 5 (0.5 s) is invalid, the probe finds sample 6 (0.6 s, 0.1 s in) = 4
//   end: sample 14 (1.4 s) is invalid, the probe finds sample 13 (1.3 s, 0.2 s before the end) = 8
//   change = 8 - 4 = 4
store::UniformSeries fixtureSeries(TestCache& tc) {
    std::vector<double> v(20, 9.0);
    const double inside[] = {4, -10, 3, 1, 5, 2, 3, 8};
    for (std::size_t i = 0; i < 8; ++i) {
        v[6 + i] = inside[i];
    }
    std::vector<bool> valid(20, true);
    valid[5] = false;
    valid[14] = false;
    return tc.uniform(Rate::fromHz(10), v, valid);
}

} // namespace

TEST_CASE("report_stats: every statistic equals the hand-computed values", "[unit]") {
    TestCache tc;
    const store::Series s = fixtureSeries(tc);
    const StatValue avg = measure(s, ReportStat::Avg, 0.5, 1.5);
    CHECK(avg.valid);
    CHECK(avg.value == 2.0);
    const StatValue sd = measure(s, ReportStat::StdDev, 0.5, 1.5);
    CHECK(sd.value == Catch::Approx(std::sqrt(24.5)).epsilon(1e-12));
    CHECK(measure(s, ReportStat::Min, 0.5, 1.5) == StatValue{-10.0, sec(0.7), true});
    CHECK(measure(s, ReportStat::Max, 0.5, 1.5) == StatValue{8.0, sec(1.3), true});
    CHECK(measure(s, ReportStat::AbsMax, 0.5, 1.5) == StatValue{10.0, sec(0.7), true});
    CHECK(measure(s, ReportStat::Delta, 0.5, 1.5).value == 18.0);
    CHECK(measure(s, ReportStat::Start, 0.5, 1.5) == StatValue{4.0, sec(0.6), true});
    CHECK(measure(s, ReportStat::End, 0.5, 1.5) == StatValue{8.0, sec(1.3), true});
    const StatValue change = measure(s, ReportStat::Change, 0.5, 1.5);
    CHECK(change.valid);
    CHECK(change.value == 4.0);
}

TEST_CASE("report_stats: the start and end probe looks inward at most probeSeconds", "[unit]") {
    TestCache tc;
    const store::Series s = fixtureSeries(tc);
    // The first valid sample is 0.1 s after the range start, the last valid one 0.2 s before its end.
    ReportOptions narrow;
    narrow.probeSeconds = 0.05;
    CHECK_FALSE(measure(s, ReportStat::Start, 0.5, 1.5, narrow).valid);
    CHECK_FALSE(measure(s, ReportStat::End, 0.5, 1.5, narrow).valid);
    CHECK_FALSE(measure(s, ReportStat::Change, 0.5, 1.5, narrow).valid);
    // The other statistics ignore the probe.
    CHECK(measure(s, ReportStat::Avg, 0.5, 1.5, narrow).value == 2.0);

    ReportOptions middle;
    middle.probeSeconds = 0.15;
    CHECK(measure(s, ReportStat::Start, 0.5, 1.5, middle) == StatValue{4.0, sec(0.6), true});
    CHECK_FALSE(measure(s, ReportStat::End, 0.5, 1.5, middle).valid);

    ReportOptions wide;
    wide.probeSeconds = 0.2;
    CHECK(measure(s, ReportStat::End, 0.5, 1.5, wide) == StatValue{8.0, sec(1.3), true});

    // A valid boundary sample needs no probe: [0.6 s, 1.4 s) starts and ends on valid samples.
    ReportOptions none;
    none.probeSeconds = 0.0;
    CHECK(measure(s, ReportStat::Start, 0.6, 1.4, none) == StatValue{4.0, sec(0.6), true});
    CHECK(measure(s, ReportStat::End, 0.6, 1.4, none) == StatValue{8.0, sec(1.3), true});
}

TEST_CASE("report_stats: a range without a valid sample is invalid for every statistic", "[unit]") {
    TestCache tc;
    const store::Series s = fixtureSeries(tc);
    for (const ReportStat which :
         {ReportStat::Avg, ReportStat::Min, ReportStat::Max, ReportStat::AbsMax, ReportStat::Start,
          ReportStat::End, ReportStat::Change, ReportStat::StdDev, ReportStat::Delta}) {
        CHECK_FALSE(measure(s, which, 0.45, 0.55).valid); // only sample 5, invalid
        CHECK_FALSE(measure(s, which, 5.0, 6.0).valid);   // after the last sample
        CHECK_FALSE(measure(s, which, 1.0, 1.0).valid);   // empty
    }
}

TEST_CASE("report_stats: interpolateBoundaries evaluates the edges by the channel's Interp", "[unit]") {
    // A 10 Hz ramp v(i) = 10 i. Range [0.25 s, 1.05 s): samples 3..10 (30 .. 100).
    TestCache tc;
    std::vector<double> ramp(20);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = 10.0 * static_cast<double>(i);
    }
    const store::Series s = tc.uniform(Rate::fromHz(10), ramp);
    // Without interpolation: the first and last samples.
    CHECK(measure(s, ReportStat::Start, 0.25, 1.05).value == 30.0);
    CHECK(measure(s, ReportStat::End, 0.25, 1.05).value == 100.0);
    CHECK(measure(s, ReportStat::Change, 0.25, 1.05).value == 70.0);

    ReportOptions interp;
    interp.interpolateBoundaries = true;
    // Linear: 0.25 s lies halfway between 20 and 30, 1.05 s halfway between 100 and 110.
    CHECK(measure(s, ReportStat::Start, 0.25, 1.05, interp) == StatValue{25.0, sec(0.25), true});
    CHECK(measure(s, ReportStat::End, 0.25, 1.05, interp) == StatValue{105.0, sec(1.05), true});
    CHECK(measure(s, ReportStat::Change, 0.25, 1.05, interp).value == 80.0);
    // Previous, Next and Closest (the earlier sample on a tie).
    CHECK(measure(s, ReportStat::Start, 0.25, 1.05, interp, store::Interp::Previous).value == 20.0);
    CHECK(measure(s, ReportStat::End, 0.25, 1.05, interp, store::Interp::Previous).value == 100.0);
    CHECK(measure(s, ReportStat::Start, 0.25, 1.05, interp, store::Interp::Next).value == 30.0);
    CHECK(measure(s, ReportStat::End, 0.25, 1.05, interp, store::Interp::Next).value == 110.0);
    CHECK(measure(s, ReportStat::Start, 0.27, 1.05, interp, store::Interp::Closest).value == 30.0);
    CHECK(measure(s, ReportStat::End, 0.25, 1.05, interp, store::Interp::Closest).value == 100.0);
    // The other statistics are unchanged by the option.
    CHECK(measure(s, ReportStat::Min, 0.25, 1.05, interp).value == 30.0);
    // An edge outside the series falls back to the probe: the last sample 190 at 1.9 s.
    CHECK(measure(s, ReportStat::End, 1.85, 2.5, interp) == StatValue{190.0, sec(1.9), true});
}

TEST_CASE("report_stats: an invalid edge neighbour falls back to the probe", "[unit]") {
    TestCache tc;
    const store::Series s = fixtureSeries(tc);
    ReportOptions interp;
    interp.interpolateBoundaries = true;
    // 0.55 s lies between sample 5 (invalid) and sample 6: Linear is invalid there, so the probe gives 4.
    CHECK(measure(s, ReportStat::Start, 0.55, 1.5, interp) == StatValue{4.0, sec(0.6), true});
    // 1.35 s lies between samples 13 (8) and 14 (invalid): the probe gives 8 at 1.3 s.
    CHECK(measure(s, ReportStat::End, 0.55, 1.35, interp) == StatValue{8.0, sec(1.3), true});
    // Previous at 1.35 s reads sample 13 (valid): 8 at the edge itself.
    CHECK(measure(s, ReportStat::End, 0.55, 1.35, interp, store::Interp::Previous) ==
          StatValue{8.0, sec(1.35), true});
}

TEST_CASE("report_stats: a timestamped series", "[unit]") {
    // Samples at 0, 0.13, 0.2, 0.71, 1.0, 1.31 s; [0.1 s, 1.2 s) holds 1, 3, -5, 9:
    //   mean 2, deviations -1, 1, -7, 7 -> 100 / 4 = 25 -> stdDev 5; min -5 at 0.71 s, max 9 at 1.0 s.
    TestCache tc;
    const store::Series s = tc.timestamped({0, 130'000, 200'000, 710'000, 1'000'000, 1'310'000},
                                           {50.0, 1.0, 3.0, -5.0, 9.0, 50.0});
    CHECK(measure(s, ReportStat::Avg, 0.1, 1.2).value == 2.0);
    CHECK(measure(s, ReportStat::StdDev, 0.1, 1.2).value == Catch::Approx(5.0).epsilon(1e-12));
    CHECK(measure(s, ReportStat::Min, 0.1, 1.2) == StatValue{-5.0, sec(0.71), true});
    CHECK(measure(s, ReportStat::Max, 0.1, 1.2) == StatValue{9.0, sec(1.0), true});
    CHECK(measure(s, ReportStat::AbsMax, 0.1, 1.2) == StatValue{9.0, sec(1.0), true});
    CHECK(measure(s, ReportStat::Start, 0.1, 1.2) == StatValue{1.0, sec(0.13), true});
    CHECK(measure(s, ReportStat::End, 0.1, 1.2) == StatValue{9.0, sec(1.0), true});
    CHECK(measure(s, ReportStat::Change, 0.1, 1.2).value == 8.0);
    CHECK(measure(s, ReportStat::Delta, 0.1, 1.2).value == 14.0);
    ReportOptions interp;
    interp.interpolateBoundaries = true;
    // Linear at 0.1 s between (0, 50) and (0.13 s, 1): 50 - 49 * 100/130.
    CHECK(measure(s, ReportStat::Start, 0.1, 1.2, interp).value ==
          Catch::Approx(50.0 - 49.0 * 100.0 / 130.0).epsilon(1e-12));
}

TEST_CASE("report_stats: ranges across chunks with an invalid run and F32 storage", "[unit]") {
    // 200,000 samples at 1 kHz of i % 10 (three chunk boundaries); samples [70,000, 80,000) invalid. Over
    // [0, 150 s) the 140,000 valid samples are whole cycles of 0..9: mean 4.5, population variance
    // (10^2 - 1) / 12 = 8.25; min 0 at 0 s, max 9 at 0.009 s.
    TestCache tc;
    const std::size_t n = 200'000;
    std::vector<double> v(n);
    std::vector<float> f(n);
    std::vector<bool> valid(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<double>(i % 10);
        f[i] = static_cast<float>(i % 10);
        valid[i] = i < 70'000 || i >= 80'000;
    }
    const store::Series s64 = tc.uniform(Rate::fromHz(1000), v, valid);
    const store::Series s32 = tc.uniformF32(Rate::fromHz(1000), f, valid);
    for (const store::Series* s : {&s64, &s32}) {
        CHECK(measure(*s, ReportStat::Avg, 0.0, 150.0).value == Catch::Approx(4.5).epsilon(1e-12));
        CHECK(measure(*s, ReportStat::StdDev, 0.0, 150.0).value ==
              Catch::Approx(std::sqrt(8.25)).epsilon(1e-12));
        CHECK(measure(*s, ReportStat::Min, 0.0, 150.0) == StatValue{0.0, 0, true});
        CHECK(measure(*s, ReportStat::Max, 0.0, 150.0) == StatValue{9.0, sec(0.009), true});
        // [69.995 s, 80.002 s): 69,995..69,999 = 5..9, then 80,000 and 80,001 = 0, 1 after the invalid run.
        CHECK(measure(*s, ReportStat::Avg, 69.995, 80.002).value ==
              Catch::Approx((5 + 6 + 7 + 8 + 9 + 0 + 1) / 7.0));
        CHECK(measure(*s, ReportStat::End, 69.995, 80.002) == StatValue{1.0, sec(80.001), true});
        // A start probe of 5 s cannot cross the 10 s invalid run.
        CHECK_FALSE(measure(*s, ReportStat::Start, 70.0, 90.0).valid);
        ReportOptions longProbe;
        longProbe.probeSeconds = 10.0;
        CHECK(measure(*s, ReportStat::Start, 70.0, 90.0, longProbe) == StatValue{0.0, sec(80.0), true});
    }
}

TEST_CASE("report_stats: a cancelled token", "[unit]") {
    TestCache tc;
    const store::Series s = fixtureSeries(tc);
    CancelSource cancel;
    cancel.cancel();
    const Result<StatValue> v = ReportEngine::statistic(s, ReportStat::Avg, 0, sec(2.0), {}, cancel.token());
    REQUIRE_FALSE(v.ok());
    CHECK(v.error().code == ErrorCode::Cancelled);
}
