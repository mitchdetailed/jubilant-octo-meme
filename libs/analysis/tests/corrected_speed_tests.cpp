// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeed and CorrectedDistance (03 section 9.1 items 1-6, D-016, D-084; AC-M1-11): fastestWheel Corr
// Speed equals a hand-computed reference on a four-wheel fixture with a locking wheel and a clamp-invalidated
// span; Corr Dist equals the trapezoid integral within 1e-9 relative and holds through invalid speed samples.
#include "analysis_test_support.hpp"

#include <esses/units/unit_registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// Not named `signal`: that is ::signal of <signal.h>, which some standard libraries include transitively
// (MinGW's <pthread.h> does), and the call below with `{}` is then ambiguous.
UniformSignal uniformSignal(std::uint32_t hz, std::vector<double> values) {
    UniformSignal s;
    s.rate = Rate::fromHz(hz);
    s.values = std::move(values);
    return s;
}

model::OpaqueJson json(const std::string& text) {
    auto j = model::OpaqueJson::parse(text);
    REQUIRE(j.ok());
    return std::move(j).value();
}

void checkSignal(const UniformSignal& got, const std::vector<double>& want, double relTol = 0.0) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        INFO("sample " << i);
        if (std::isnan(want[i])) {
            CHECK_FALSE(got.validAt(i));
            CHECK(std::isnan(got.values[i]));
        } else {
            CHECK(got.validAt(i));
            if (relTol == 0.0) {
                CHECK(got.values[i] == want[i]);
            } else {
                CHECK(got.values[i] == Catch::Approx(want[i]).epsilon(relTol).margin(1e-300));
            }
        }
    }
}

// The AC-M1-11 four-wheel fixture: FL and the rears at 10 Hz, FR at 5 Hz (resampled Linear to 10 Hz). FL
// locks at 0.6-0.9 s; the fronts peak at 33 m/s at 0.8 s, above the 28 m/s clamp (maxInvalidate).
const std::vector<double> kFL = {10, 12, 14, 16, 18, 20, 0, 0, 0, 0, 20, 18, 16, 14, 12, 10};
const std::vector<double> kFR = {9, 13, 17, 21, 33, 29, 13, 9}; // 0, 0.2, ..., 1.4 s
const double kX = std::nan("");
// Hand-computed: FR at 10 Hz is 9 11 13 15 17 19 21 27 33 31 29 21 13 11 9 (none at 1.5 s); per sample the
// larger wheel; 33, 31 and 29 exceed 28 and are invalidated; 1.5 s takes FL alone.
const std::vector<double> kCorrSpeed = {10, 12, 14, 16, 18, 20, 21, 27, kX, kX, kX, 21, 16, 14, 12, 10};
// Trapezoid with dt = 0.1 s; a segment touching an invalid sample adds nothing.
const std::vector<double> kCorrDist = {0,     1.1,   2.4,   3.9,   5.6,  7.5,  9.55, 11.95,
                                       11.95, 11.95, 11.95, 11.95, 13.8, 15.3, 16.6, 17.7};

model::DataLayer fourWheelLayer(TestCache& tc) {
    model::DataLayer layer;
    layer.addChannel(channel("Wheel Speed FL", tc.uniform(Rate::fromHz(10), kFL)));
    layer.addChannel(channel("Wheel Speed FR", tc.uniform(Rate::fromHz(5), kFR)));
    std::vector<double> rear = kFL;
    rear[6] = rear[7] = rear[8] = rear[9] = 45.0; // the rears are not inputs of the default settings
    layer.addChannel(channel("Wheel Speed RL", tc.uniform(Rate::fromHz(10), rear)));
    layer.addChannel(channel("Wheel Speed RR", tc.uniform(Rate::fromHz(10), rear)));
    return layer;
}

} // namespace

TEST_CASE("corrected_speed: fastestWheel equals the hand-computed four-wheel reference", "[unit][AC-M1-11]") {
    SpeedClamp clamp;
    clamp.maxMps = 28.0;
    clamp.maxInvalidate = true;
    auto speed = CorrectedSpeed::fastestWheel(uniformSignal(10, kFL), uniformSignal(5, kFR), clamp);
    REQUIRE(speed.ok());
    CHECK(speed->rate == Rate::fromHz(10));
    CHECK(speed->t0 == 0);
    checkSignal(*speed, kCorrSpeed);
    // Argument order does not matter: the grid is the higher rate either way.
    auto swapped = CorrectedSpeed::fastestWheel(uniformSignal(5, kFR), uniformSignal(10, kFL), clamp);
    REQUIRE(swapped.ok());
    checkSignal(*swapped, kCorrSpeed);

    auto dist = CorrectedDistance::integrate(*speed);
    REQUIRE(dist.ok());
    CHECK(dist->valid.empty()); // Corr Dist has no invalid samples
    checkSignal(*dist, kCorrDist, 1e-9);
}

TEST_CASE("corrected_speed: compute on a four-wheel layer with the default channels", "[unit][AC-M1-11]") {
    TestCache tc;
    const model::DataLayer layer = fourWheelLayer(tc);
    auto settings = CorrectedSpeedSettings::fromJson(
        json(R"({"clamp": {"max": {"v": 100.8, "unit": "km/h"}, "maxInvalidate": true}})"), registry());
    REQUIRE(settings.ok());
    CHECK(settings->clamp.maxMps == Catch::Approx(28.0).epsilon(1e-12));
    CHECK(settings->clamp.minMps == 0.0); // the default min stays
    settings->clamp.maxMps = 28.0;        // exactly, for the hand reference
    auto result = CorrectedSpeed::compute(*settings, layer, registry(), {});
    REQUIRE(result.ok());
    CHECK(result->inputs == std::vector<std::string>{"Wheel Speed FL", "Wheel Speed FR"});
    CHECK(result->diagnostics.empty());
    checkSignal(result->speedMps, kCorrSpeed);
    auto dist = CorrectedDistance::integrate(result->speedMps, settings->integration);
    REQUIRE(dist.ok());
    checkSignal(*dist, kCorrDist, 1e-9);

    // Without the max clamp the locking wheel is still covered by FR.
    auto open = CorrectedSpeed::compute(CorrectedSpeedSettings{}, layer, registry(), {});
    REQUIRE(open.ok());
    CHECK(open->speedMps.values[8] == 33.0);
    CHECK(open->speedMps.values[6] == 21.0);
    CHECK(open->speedMps.valid.empty());

    // All four wheels (an N-wheel list built in code): the rears' 45 m/s wins at 0.6-0.9 s.
    CorrectedSpeedSettings four;
    four.fastestWheelChannels = {"Wheel Speed FL", "Wheel Speed FR", "Wheel Speed RL", "Wheel Speed RR"};
    auto all = CorrectedSpeed::compute(four, layer, nullptr, {});
    REQUIRE(all.ok());
    CHECK(all->speedMps.values[7] == 45.0);
    CHECK(all->inputs.size() == 4);
}

TEST_CASE("corrected_speed: Corr Dist equals the trapezoid integral within 1e-9 relative",
          "[unit][AC-M1-11]") {
    // 1 kHz for 120 s of v(t) = 5 + 0.75 t (linear: the trapezoid rule is exact), with an invalid span at
    // 50-60 s through which the distance holds.
    constexpr std::size_t kHz = 1000;
    UniformSignal speed = uniformSignal(static_cast<std::uint32_t>(kHz), {});
    const std::size_t n = 120 * kHz + 1;
    speed.values.resize(n);
    speed.valid.assign(n, 1);
    for (std::size_t i = 0; i < n; ++i) {
        speed.values[i] = 5.0 + 0.75 * (static_cast<double>(i) / kHz);
    }
    for (std::size_t i = 50 * kHz + 1; i < 60 * kHz; ++i) {
        speed.valid[i] = 0;
        speed.values[i] = std::nan("");
    }
    auto dist = CorrectedDistance::integrate(speed);
    REQUIRE(dist.ok());
    const auto exact = [](double a, double b) { // integral of 5 + 0.75 t over [a, b]
        return 5.0 * (b - a) + 0.375 * (b * b - a * a);
    };
    // Up to 50.001 s the samples are valid; [50.001, 60.0] contributes nothing (segments touch invalid
    // samples from 50.000-50.001 to 59.999-60.000).
    CHECK(dist->values[30 * kHz] == Catch::Approx(exact(0, 30)).epsilon(1e-9));
    CHECK(dist->values[50 * kHz] == Catch::Approx(exact(0, 50)).epsilon(1e-9));
    for (std::size_t i = 50 * kHz; i <= 60 * kHz; ++i) {
        REQUIRE(dist->values[i] == dist->values[50 * kHz]); // holds
    }
    CHECK(dist->values[n - 1] == Catch::Approx(exact(0, 50) + exact(60, 120)).epsilon(1e-9));
    CHECK(dist->rate == speed.rate);

    // The rectangle rule (maths integrate()): v[i] dt from the first sample.
    auto rect = CorrectedDistance::integrate(uniformSignal(10, {1, 2, 3, 4}), Integration::Rectangle);
    REQUIRE(rect.ok());
    checkSignal(*rect, {0.1, 0.3, 0.6, 1.0}, 1e-12);
    // An odd rate: dt is den / num exactly.
    UniformSignal third = uniformSignal(1, {3, 3, 3});
    third.rate = Rate{3, 1};
    auto d3 = CorrectedDistance::integrate(third);
    REQUIRE(d3.ok());
    CHECK(d3->values[2] == Catch::Approx(2.0).epsilon(1e-15));
    CHECK(CorrectedDistance::integrate(UniformSignal{}).error().code == ErrorCode::Invalid);
}

TEST_CASE("corrected_speed: the clamp replaces or invalidates", "[unit][AC-M1-11]") {
    UniformSignal s = uniformSignal(10, {-2, 0, 5, 50, 70});
    SpeedClamp c; // min 0, no max
    CorrectedSpeed::applyClamp(s, c);
    checkSignal(s, {0, 0, 5, 50, 70});
    c.minMps = 1.0;
    c.minInvalidate = true;
    c.maxMps = 60.0;
    c.maxInvalidate = true;
    CorrectedSpeed::applyClamp(s, c);
    checkSignal(s, {kX, kX, 5, 50, kX});
    c.maxInvalidate = false;
    c.maxMps = 40.0;
    CorrectedSpeed::applyClamp(s, c);
    checkSignal(s, {kX, kX, 5, 40, kX});
    UniformSignal untouched = uniformSignal(10, {-5, 500});
    CorrectedSpeed::applyClamp(untouched, SpeedClamp{std::nullopt, false, std::nullopt, false});
    checkSignal(untouched, {-5, 500});
}

TEST_CASE("corrected_speed: fastestWheel resampling at the edges and errors", "[unit][AC-M1-11]") {
    // b starts later (t0 = 0.25 s) and is invalid in the middle: one invalid -> the other; both -> invalid.
    UniformSignal a = uniformSignal(4, {1, 2, 3, 4, 5});
    a.valid = {1, 1, 0, 1, 1};
    a.values[2] = std::nan("");
    UniformSignal b = uniformSignal(4, {10, 10, 10});
    b.t0 = 250'000;
    b.valid = {1, 0, 1};
    b.values[1] = std::nan("");
    auto m = CorrectedSpeed::fastestWheel(a, b, SpeedClamp{std::nullopt, false, std::nullopt, false});
    REQUIRE(m.ok());
    // Grid: a's (4 Hz from 0) up to the later last sample (a's, 1 s): 1, 10, X (both invalid), 10, 5 (b
    // ended).
    checkSignal(*m, {1, 10, kX, 10, 5});
    CHECK(CorrectedSpeed::fastestWheel(std::span<const UniformSignal>{}, SpeedClamp{}).error().code ==
          ErrorCode::Invalid);
    CHECK(CorrectedSpeed::fastestWheel(a, UniformSignal{}, SpeedClamp{}).error().code == ErrorCode::Invalid);
}

TEST_CASE("corrected_speed: fastestWheel starts with the earliest wheel", "[unit][AC-M1-11]") {
    // a: 4 Hz from 0.5 s (the highest rate, so the grid); b: 2 Hz from 0 s, so b's first samples come before
    // a's. The grid moves back to 0 s in a's period: 0, 0.25 (b interpolated alone), then the maximum.
    UniformSignal a = uniformSignal(4, {1, 1, 1, 1});
    a.t0 = 500'000;
    UniformSignal b = uniformSignal(2, {5, 6, 7});
    auto m = CorrectedSpeed::fastestWheel(a, b, SpeedClamp{std::nullopt, false, std::nullopt, false});
    REQUIRE(m.ok());
    CHECK(m->t0 == 0);
    CHECK(m->rate == Rate::fromHz(4));
    // 0: b 5; 0.25: b 5.5; 0.5: max(a 1, b 6) = 6; 0.75: max(1, 6.5); 1.0: max(1, 7); 1.25: a 1 (b ended).
    checkSignal(*m, {5, 5.5, 6, 6.5, 7, 1});
    // A wheel on the grid's rate but not on its points is interpolated, not gathered.
    UniformSignal c = uniformSignal(4, {2, 4});
    c.t0 = 125'000;
    auto mc = CorrectedSpeed::fastestWheel(a, c, SpeedClamp{std::nullopt, false, std::nullopt, false});
    REQUIRE(mc.ok());
    CHECK(mc->t0 == 0); // the grid point at or before c's first sample (0.125 s)
    checkSignal(*mc, {kX, 3, 1, 1, 1, 1});
}

TEST_CASE("corrected_speed: fallbacks, unit conversion and no speed", "[unit][AC-M1-11]") {
    TestCache tc;
    // Only one front wheel: fastestWheel falls back to alternateList (W-CSD-FALLBACK) and finds Ground Speed.
    model::DataLayer layer;
    layer.addChannel(channel("Wheel Speed FR", tc.uniform(Rate::fromHz(10), {1, 2, 3})));
    model::Channel ground = channel("Ground Speed", tc.uniform(Rate::fromHz(10), {36, 72, -3.6}));
    ground.desc.unit = *registry().find("km/h");
    ground.desc.quantity = registry().quantityOf(ground.desc.unit);
    layer.addChannel(std::move(ground));
    auto r = CorrectedSpeed::compute(CorrectedSpeedSettings{}, layer, registry(), {});
    REQUIRE(r.ok());
    CHECK(r->diagnostics == std::vector<std::string>{"W-CSD-FALLBACK"});
    CHECK(r->inputs == std::vector<std::string>{"Ground Speed"});
    checkSignal(r->speedMps, {10, 20, 0}, 1e-12); // km/h -> m/s, clamped at 0
    // Without a registry nothing is converted.
    auto raw = CorrectedSpeed::compute(CorrectedSpeedSettings{}, layer, nullptr, {});
    REQUIRE(raw.ok());
    CHECK(raw->speedMps.values[0] == 36.0);

    // The sim-exporter dialect's road speed (SPEED, km/h) is the default list's last entry, matched
    // case-insensitively (M1 integration: the public ACC stand-in gets Corr Speed and distance mode).
    model::DataLayer sim;
    model::Channel simSpeed = channel("SPEED", tc.uniform(Rate::fromHz(10), {36, 72}));
    simSpeed.desc.unit = *registry().find("km/h");
    simSpeed.desc.quantity = registry().quantityOf(simSpeed.desc.unit);
    sim.addChannel(std::move(simSpeed));
    auto viaSpeed = CorrectedSpeed::compute(CorrectedSpeedSettings{}, sim, registry(), {});
    REQUIRE(viaSpeed.ok());
    CHECK(viaSpeed->diagnostics == std::vector<std::string>{"W-CSD-FALLBACK"});
    CHECK(viaSpeed->inputs == std::vector<std::string>{"SPEED"});
    checkSignal(viaSpeed->speedMps, {10, 20}, 1e-12);

    // A channel whose unit is not a speed is unusable; logged names resolve too.
    model::DataLayer odd;
    model::Channel metres = channel("Ground Speed", tc.uniform(Rate::fromHz(10), {1, 2}));
    metres.desc.unit = *registry().find("m");
    odd.addChannel(std::move(metres));
    model::Channel logged = channel("GPS Speed", tc.uniform(Rate::fromHz(10), {7, 8}));
    logged.desc.loggedName = "GPS_SPEED";
    logged.desc.name = "Gps Velocity";
    odd.addChannel(std::move(logged));
    CorrectedSpeedSettings alt;
    alt.method = SpeedMethod::AlternateList;
    alt.alternateListChannels = {"Ground Speed", "GPS_SPEED"};
    auto viaLogged = CorrectedSpeed::compute(alt, odd, registry(), {});
    REQUIRE(viaLogged.ok());
    CHECK(viaLogged->speedMps.values[1] == 8.0);
    CHECK(viaLogged->diagnostics.empty());

    // Nothing usable: NotFound with E-CSD-NO-SPEED; gCorrection waits for M3.
    model::DataLayer none;
    none.addChannel(channel("Throttle Pos", tc.uniform(Rate::fromHz(10), {1})));
    auto missing = CorrectedSpeed::compute(CorrectedSpeedSettings{}, none, registry(), {});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(missing.error().context.detail == "E-CSD-NO-SPEED");
    CorrectedSpeedSettings g;
    g.method = SpeedMethod::GCorrection;
    CHECK(CorrectedSpeed::compute(g, layer, registry(), {}).error().code == ErrorCode::Unsupported);

    // A timestamped speed channel is not usable yet.
    model::DataLayer stamped;
    stamped.addChannel(channel("Ground Speed", tc.timestamped({0, 100'000}, {1, 2})));
    CHECK(CorrectedSpeed::compute(CorrectedSpeedSettings{}, stamped, registry(), {}).error().code ==
          ErrorCode::NotFound);
    // Cancellation propagates.
    CancelSource cancel;
    cancel.cancel();
    CHECK(CorrectedSpeed::compute(CorrectedSpeedSettings{}, layer, registry(), cancel.token()).error().code ==
          ErrorCode::Cancelled);
}

TEST_CASE("corrected_speed: Corr Dist from a resetting logged lap distance", "[unit]") {
    // Lap distance restarts at each beacon (3,900 m lap); a 3 m step back is noise and passes through.
    UniformSignal lap = uniformSignal(1, {3500, 3700, 3890, 5, 105, 102, 300, 3895, 2, 50});
    lap.valid = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    auto d = CorrectedDistance::fromLogged(lap);
    REQUIRE(d.ok());
    checkSignal(*d, {3700, 3700, 3890, 3895, 3995, 3992, 4190, 7785, 7787, 7835});
    UniformSignal dead = uniformSignal(1, {1, 2});
    dead.valid = {0, 0};
    CHECK(CorrectedDistance::fromLogged(dead).error().code == ErrorCode::NotFound);
}

TEST_CASE("corrected_speed: settings parse the 03 section 9.1 object and its schema examples", "[unit]") {
    auto s = CorrectedSpeedSettings::fromJson(json(R"({
        "method": "alternateList",
        "fastestWheel": {"channels": ["WS FL", "WS FR"]},
        "alternateList": {"channels": ["Ground Speed"]},
        "gCorrection": {"speedChannel": "WS FL", "longGChannel": "G Long",
                        "lockThreshold": {"v": 7.2, "unit": "km/h"}, "recoverSeconds": 0.25},
        "clamp": {"min": null, "minInvalidate": true, "max": {"v": 300, "unit": "km/h"}, "maxInvalidate": true},
        "distance": {"source": "logged", "loggedChannel": "Lap Dist", "integration": "rectangle"},
        "outputs": {"speed": "CS", "distance": "CD"}})"),
                                              registry());
    REQUIRE(s.ok());
    CHECK(s->method == SpeedMethod::AlternateList);
    CHECK(s->fastestWheelChannels == std::vector<std::string>{"WS FL", "WS FR"});
    CHECK(s->alternateListChannels == std::vector<std::string>{"Ground Speed"});
    CHECK(s->gCorrection.lockThresholdMps == Catch::Approx(2.0).epsilon(1e-12));
    CHECK(s->gCorrection.recoverSeconds == 0.25);
    CHECK_FALSE(s->clamp.minMps.has_value());
    CHECK(s->clamp.minInvalidate);
    CHECK(*s->clamp.maxMps == Catch::Approx(300 / 3.6).epsilon(1e-12));
    CHECK(s->distanceSource == DistanceSource::Logged);
    CHECK(s->loggedDistanceChannel == "Lap Dist");
    CHECK(s->integration == Integration::Rectangle);
    CHECK(s->speedOutput == "CS");
    CHECK(s->distanceOutput == "CD");

    auto defaults = CorrectedSpeedSettings::fromJson(model::OpaqueJson{}, registry());
    REQUIRE(defaults.ok());
    CHECK(defaults->clamp.minMps == 0.0);
    CHECK(defaults->speedOutput == "Corr Speed");

    const auto pointerOf = [](const std::string& text) {
        auto r = CorrectedSpeedSettings::fromJson(json(text), registry());
        REQUIRE_FALSE(r.ok());
        return r.error().context.detail.value_or("<none>");
    };
    CHECK(pointerOf(R"({"method": "magic"})") == "/method");
    CHECK(pointerOf(R"({"fastestWheel": {"channels": ["A", "B", "C"]}})") == "/fastestWheel/channels");
    CHECK(pointerOf(R"({"clamp": {"max": {"v": 1, "unit": "bar"}}})") == "/clamp/max/unit");
    CHECK(pointerOf(R"({"clamp": {"min": {"v": 10, "unit": "m/s"}, "max": {"v": 5, "unit": "m/s"}}})") ==
          "/clamp/max");
    CHECK(pointerOf(R"({"outputs": {"speed": " "}})") == "/outputs/speed");
    CHECK(pointerOf(R"({"outputs": {"speed": "X", "distance": "X"}})") == "/outputs");
    CHECK(pointerOf(R"({"colour": 1})") == "/colour");

    // Every example of the settings schema: valid ones parse, invalid ones fail.
    const std::filesystem::path examples =
        repoRoot() / "schemas" / "examples" / "builtin-esses.corrected_speed_distance-1";
    for (const char* kind : {"valid", "invalid"}) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(examples / kind, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            INFO(pathToUtf8(it->path()));
            auto parsed = CorrectedSpeedSettings::fromJson(json(text.str()), registry());
            CHECK(parsed.ok() == (std::string(kind) == "valid"));
        }
    }
}

TEST_CASE("corrected_speed: toUniformSeries round-trips through a store column", "[unit]") {
    TestCache tc;
    UniformSignal s = uniformSignal(10, {1.5, std::nan(""), 3.5});
    s.t0 = 1'000;
    s.valid = {1, 0, 1};
    const store::UniformSeries series = toUniformSeries(s, tc.options(), "test:speed");
    CHECK(series.rate == s.rate);
    CHECK(series.t0 == 1'000);
    REQUIRE(series.size() == 3);
    auto pinned = series.values.require(0, 2, Priority::Interactive, {});
    REQUIRE(pinned.ok());
    CHECK(pinned->at(0) == 1.5);
    CHECK_FALSE(pinned->validAt(1));
    CHECK(pinned->at(2) == 3.5);
}
