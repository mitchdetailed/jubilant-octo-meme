// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeed::alternateList and CorrectedSpeed::gCorrection (03 section 9.1 items 2-3, D-084; 07 section
// 6.1): the first engaged candidate with the clamp; the lock/spin correction against a hand-computed
// reference (prediction while the wheel leaves the band, a linear blend back over recoverSeconds), and both
// through CorrectedSpeed::compute.
#include "analysis_test_support.hpp"

#include <esses/units/unit_registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

const units::UnitRegistry& registry() {
    static const units::UnitRegistry r = [] {
        auto loaded = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        REQUIRE(loaded.ok());
        return std::move(loaded).value();
    }();
    return r;
}

UniformSignal uniformSignal(std::uint32_t hz, std::vector<double> values, Micros t0 = 0) {
    UniformSignal s;
    s.rate = Rate::fromHz(hz);
    s.t0 = t0;
    s.values = std::move(values);
    return s;
}

const double kX = std::nan("");

void checkSignal(const UniformSignal& got, const std::vector<double>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        INFO("sample " << i);
        if (std::isnan(want[i])) {
            CHECK_FALSE(got.validAt(i));
            CHECK(std::isnan(got.values[i]));
        } else {
            CHECK(got.validAt(i));
            CHECK(got.values[i] == Catch::Approx(want[i]).epsilon(1e-12).margin(1e-12));
        }
    }
}

// 10 Hz wheel braking at 5 m/s^2 (0.5 m/s per sample): it locks at 0.3 s (v_pred carries on), comes back at
// 0.6 s (blend over 0.2 s), spins at 1.0 s and comes back at 1.1 s.
const std::vector<double> kWheel = {30, 29.5, 29, 20, 10, 5, 26, 25.5, 25, 24.5, 27, 23.6, 23.1, 22.6};
// Hand-computed with lockThreshold 2 m/s, recoverSeconds 0.2 s:
//   0-2 follow the wheel; 3-5 v_pred = 28.5, 28, 27.5 (the wheel is more than 2 below);
//   6 v_pred 27 is within 1 of the wheel 26: the blend starts at v_pred (offset +1);
//   7 v_pred 26.5 vs 25.5 within the band: 25.5 + 1 * (1 - 0.5) = 26; 8 the blend is over: the wheel 25;
//   9 the wheel; 10 v_pred 24 vs 27: spin, follow v_pred 24; 11 v_pred 23.5 vs 23.6: blend (offset -0.1);
//   12 23.1 - 0.1 * 0.5 = 23.05; 13 the wheel 22.6.
const std::vector<double> kCorrected = {30, 29.5, 29,   28.5, 28,   27.5,  27,
                                        26, 25,   24.5, 24,   23.5, 23.05, 22.6};

GCorrectionSettings settings() {
    GCorrectionSettings g;
    g.lockThresholdMps = 2.0;
    g.recoverSeconds = 0.2;
    return g;
}

} // namespace

TEST_CASE("corrected_speed_methods: alternateList takes the first engaged candidate and clamps it",
          "[unit]") {
    std::vector<std::optional<UniformSignal>> candidates(4);
    candidates[1] = uniformSignal(10, {-2, 5, 50});
    candidates[2] = uniformSignal(10, {1, 1, 1});
    SpeedClamp clamp; // min 0 (clamps), no max
    clamp.maxMps = 40.0;
    clamp.maxInvalidate = true;
    auto chosen = CorrectedSpeed::alternateList(candidates, clamp);
    REQUIRE(chosen.ok());
    CHECK(chosen->index == 1);
    checkSignal(chosen->speedMps, {0, 5, kX});

    const std::vector<std::optional<UniformSignal>> none(3);
    auto missing = CorrectedSpeed::alternateList(none, clamp);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(missing.error().context.detail == "E-CSD-NO-SPEED");
    CHECK(CorrectedSpeed::alternateList({}, clamp).error().code == ErrorCode::NotFound);
}

TEST_CASE("corrected_speed_methods: gCorrection equals the hand-computed lock and spin reference", "[unit]") {
    // The acceleration at 5 Hz, resampled Linear onto the wheel's 10 Hz grid.
    const UniformSignal accel = uniformSignal(5, std::vector<double>(8, -5.0));
    auto speed = CorrectedSpeed::gCorrection(uniformSignal(10, kWheel), accel, settings(), SpeedClamp{});
    REQUIRE(speed.ok());
    CHECK(speed->rate == Rate::fromHz(10));
    CHECK(speed->valid.empty());
    checkSignal(*speed, kCorrected);

    // A consistent wheel (the integral of the acceleration) is followed exactly.
    std::vector<double> clean(20);
    for (std::size_t i = 0; i < clean.size(); ++i) {
        clean[i] = 40.0 - 0.5 * static_cast<double>(i);
    }
    auto same = CorrectedSpeed::gCorrection(uniformSignal(10, clean), accel, settings(), SpeedClamp{});
    REQUIRE(same.ok());
    checkSignal(*same, std::vector<double>(clean.begin(), clean.end()));

    // recoverSeconds 0: straight back to the wheel when it returns into the band.
    GCorrectionSettings instant = settings();
    instant.recoverSeconds = 0.0;
    auto snap = CorrectedSpeed::gCorrection(uniformSignal(10, kWheel), accel, instant, SpeedClamp{});
    REQUIRE(snap.ok());
    CHECK(snap->values[6] == 26.0);
    CHECK(snap->values[7] == Catch::Approx(25.5));
}

TEST_CASE("corrected_speed_methods: gCorrection catches a slip that builds over several samples", "[unit]") {
    // The per-sample test of 03 section 9.1 item 3 alone never fires on these (D-203 (a)).
    const auto maxError = [](const UniformSignal& got, const std::vector<double>& car) {
        double worst = 0.0;
        for (std::size_t i = 0; i < car.size(); ++i) {
            worst = std::max(worst, std::abs(got.values[i] - car[i]));
        }
        return worst;
    };
    GCorrectionSettings g;
    g.lockThresholdMps = 2.0;
    g.recoverSeconds = 0.5;
    SECTION("a lock building at 100 Hz: 48 m/s^2 more braking than the car, 0.48 m/s per sample") {
        std::vector<double> car(200);
        std::vector<double> wheel(200);
        for (std::size_t i = 0; i < car.size(); ++i) {
            const double t = static_cast<double>(i) * 0.01;
            car[i] = 40.0 - 10.0 * t;
            const double slip = t < 0.5   ? 0.0
                                : t < 0.8 ? 48.0 * (t - 0.5)
                                : t < 1.0 ? 14.4 * (1.0 - (t - 0.8) / 0.2)
                                          : 0.0;
            wheel[i] = car[i] - slip;
        }
        auto out = CorrectedSpeed::gCorrection(
            uniformSignal(100, wheel), uniformSignal(100, std::vector<double>(200, -10.0)), g, SpeedClamp{});
        REQUIRE(out.ok());
        // The wheel is up to 14.4 m/s slow; the corrected speed stays within the lock threshold (and a bit).
        CHECK(maxError(*out, car) < 2.5);
    }
    SECTION("a driven wheel spinning up at 10 Hz: 5 m/s^2 more than the car, 0.5 m/s per sample") {
        std::vector<double> car(60);
        std::vector<double> wheel(60);
        for (std::size_t i = 0; i < car.size(); ++i) {
            const double t = static_cast<double>(i) * 0.1;
            car[i] = 10.0 + 3.0 * t;
            const double spin = t < 1.0   ? 0.0
                                : t < 3.0 ? 5.0 * (t - 1.0)
                                : t < 3.5 ? 10.0 * (1.0 - (t - 3.0) / 0.5)
                                          : 0.0;
            wheel[i] = car[i] + spin;
        }
        auto out = CorrectedSpeed::gCorrection(
            uniformSignal(10, wheel), uniformSignal(10, std::vector<double>(60, 3.0)), g, SpeedClamp{});
        REQUIRE(out.ok());
        // The wheel runs up to 10 m/s fast.
        CHECK(maxError(*out, car) < 2.5);
    }
    SECTION("an accelerometer offset alone is no slip") {
        std::vector<double> wheel(300);
        for (std::size_t i = 0; i < wheel.size(); ++i) {
            wheel[i] = 30.0 + 2.0 * static_cast<double>(i) * 0.01;
        }
        // Logged acceleration 2.3 m/s^2 for a true 2 m/s^2: the wheel is followed exactly.
        auto out = CorrectedSpeed::gCorrection(
            uniformSignal(100, wheel), uniformSignal(100, std::vector<double>(300, 2.3)), g, SpeedClamp{});
        REQUIRE(out.ok());
        checkSignal(*out, wheel);
    }
}

TEST_CASE("corrected_speed_methods: gCorrection with invalid samples, a late acceleration and the clamp",
          "[unit]") {
    UniformSignal wheel = uniformSignal(10, {30, 29.5, 29, 20, 10, 28, 27.5});
    wheel.valid = {1, 1, 1, 1, 0, 1, 1};
    // The acceleration starts at 0.2 s: before it there is nothing to predict from.
    const UniformSignal accel = uniformSignal(10, std::vector<double>(5, -5.0), 200'000);
    SpeedClamp clamp;
    clamp.maxMps = 29.0; // 29.5 and 30 clamp to 29
    auto out = CorrectedSpeed::gCorrection(wheel, accel, settings(), clamp);
    REQUIRE(out.ok());
    // 0, 1: the wheel (no acceleration yet), clamped; 2: v_pred 29.5 - 0.5 = 29 = the wheel; 3: lock,
    // v_pred 28.5; 4: invalid wheel -> invalid; 5: restart on the wheel 28; 6: the acceleration has ended
    // (last sample 0.6 s, so 0.6 s is still inside): v_pred 27.5 = the wheel.
    checkSignal(*out, {29, 29, 29, 28.5, kX, 28, 27.5});

    UniformSignal noRate = wheel;
    noRate.rate = Rate{0, 1};
    CHECK(CorrectedSpeed::gCorrection(noRate, accel, settings(), clamp).error().code == ErrorCode::Invalid);
    UniformSignal broken = wheel;
    broken.valid.pop_back();
    CHECK(CorrectedSpeed::gCorrection(broken, accel, settings(), clamp).error().code == ErrorCode::Invalid);
}

TEST_CASE("corrected_speed_methods: compute runs gCorrection on the layer's speed and long-G channels",
          "[unit]") {
    TestCache tc;
    model::DataLayer layer;
    layer.addChannel(channel("Wheel Speed FL", tc.uniform(Rate::fromHz(10), kWheel)));
    // No unit: taken in G (-5 m/s^2 is -0.50986 G).
    layer.addChannel(
        channel("G Force Long", tc.uniform(Rate::fromHz(5), std::vector<double>(8, -5.0 / 9.80665))));
    CorrectedSpeedSettings s;
    s.method = SpeedMethod::GCorrection;
    s.gCorrection = settings();
    auto r = CorrectedSpeed::compute(s, layer, registry(), {});
    REQUIRE(r.ok());
    CHECK(r->inputs == std::vector<std::string>{"Wheel Speed FL", "G Force Long"});
    CHECK(r->diagnostics.empty());
    checkSignal(r->speedMps, kCorrected);
    auto raw = CorrectedSpeed::compute(s, layer, nullptr, {}); // no registry: G as well
    REQUIRE(raw.ok());
    checkSignal(raw->speedMps, kCorrected);

    // With a unit the registry converts it (m/s/s here); a unit that is not an acceleration is unusable.
    model::DataLayer si;
    si.addChannel(channel("Wheel Speed FL", tc.uniform(Rate::fromHz(10), kWheel)));
    model::Channel g = channel("G Force Long", tc.uniform(Rate::fromHz(5), std::vector<double>(8, -5.0)));
    g.desc.unit = registry().find("m/s/s").value_or(units::kNoUnit);
    g.desc.quantity = registry().quantityOf(g.desc.unit);
    si.addChannel(std::move(g));
    auto bySi = CorrectedSpeed::compute(s, si, registry(), {});
    REQUIRE(bySi.ok());
    checkSignal(bySi->speedMps, kCorrected);

    model::DataLayer wrong;
    wrong.addChannel(channel("Wheel Speed FL", tc.uniform(Rate::fromHz(10), kWheel)));
    model::Channel metres =
        channel("G Force Long", tc.uniform(Rate::fromHz(5), std::vector<double>(8, -5.0)));
    metres.desc.unit = registry().find("m").value_or(units::kNoUnit);
    metres.desc.quantity = registry().quantityOf(metres.desc.unit);
    wrong.addChannel(std::move(metres));
    auto unusable = CorrectedSpeed::compute(s, wrong, registry(), {});
    REQUIRE_FALSE(unusable.ok());
    CHECK(unusable.error().code == ErrorCode::NotFound);
    CHECK(unusable.error().context.detail == "E-CSD-NO-SPEED");
    CHECK(unusable.error().context.channel == "G Force Long");

    // No long-G channel: NotFound, no fallback to the alternate list.
    model::DataLayer wheelOnly;
    wheelOnly.addChannel(channel("Wheel Speed FL", tc.uniform(Rate::fromHz(10), kWheel)));
    wheelOnly.addChannel(channel("Ground Speed", tc.uniform(Rate::fromHz(10), kWheel)));
    auto noG = CorrectedSpeed::compute(s, wheelOnly, registry(), {});
    REQUIRE_FALSE(noG.ok());
    CHECK(noG.error().context.detail == "E-CSD-NO-SPEED");
    CHECK(noG.error().context.channel == "G Force Long");
}

TEST_CASE("corrected_speed_methods: compute's alternate list reads up to the first usable channel",
          "[unit]") {
    TestCache tc;
    model::DataLayer layer;
    layer.addChannel(channel("Drive Speed", tc.uniform(Rate::fromHz(10), {3, 4})));
    layer.addChannel(channel("GPS Speed", tc.uniform(Rate::fromHz(10), {7, 8})));
    CorrectedSpeedSettings s;
    s.method = SpeedMethod::AlternateList;
    s.clamp.maxMps = 3.5; // clamps, does not invalidate
    auto r = CorrectedSpeed::compute(s, layer, registry(), {});
    REQUIRE(r.ok());
    CHECK(r->inputs == std::vector<std::string>{"Drive Speed"}); // listed before GPS Speed
    checkSignal(r->speedMps, {3, 3.5});
}
