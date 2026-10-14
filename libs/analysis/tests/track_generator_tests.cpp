// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackGenerator (AC-M3-04, 06 section 13.5): shape against ground truth, never self-consistency. The
// synthetic circuit (fixtures/track/synthetic-circuit, tools/gen_track_fixtures.py) carries the true
// centreline, inner and outer lines and corner apexes; its lap has GPS (sigma 1.5 m white noise per axis, a
// 2 s dropout through a corner) and IMU (speed 0.8 % high with noise, lateral acceleration with noise and a
// bias). (a) GPS: within 2 m RMS of the true centreline. (b) Speed+LatG: within 15 m RMS of the GPS map after
// a best-fit rigid alignment, and every default-section apex within 20 m of its GPS counterpart (lap
// distances scaled to the GPS map's length, as lap stretching does); the closure error before the correction
// is reported, not gated. Thresholds are provisional until the first real fixture (07 section 6.2).
#include "track_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

TrackGeneratorOptions withOrigin(const CircuitFixture& f) {
    TrackGeneratorOptions o;
    o.origin = f.origin; // the fixture's ENU frame (the default origin is the first, noisy fix)
    return o;
}

bool hasDiagnostic(const TrackGenerationReport& r, std::string_view code) {
    return std::find(r.diagnostics.begin(), r.diagnostics.end(), code) != r.diagnostics.end();
}

// The corner of `pool` whose apex (scaled by `scale`) is nearest to `apexM`.
const TrackSection* nearestCorner(const TrackMap& map, const std::vector<TrackSection>& pool, double apexM,
                                  double scale, double* foundApex) {
    const TrackSection* best = nullptr;
    double bestD = std::numeric_limits<double>::infinity();
    for (const TrackSection& s : pool) {
        const double a = apexOf(map, s) * scale;
        if (std::abs(a - apexM) < bestD) {
            bestD = std::abs(a - apexM);
            best = &s;
            *foundApex = a;
        }
    }
    return best;
}

} // namespace

TEST_CASE("track generator: toEnu is the local tangent plane of WGS-84", "[unit][AC-M3-04]") {
    const TrackOrigin o{45.0612, 7.4287, 250.0};
    const TrackPoint at = TrackGenerator::toEnu(o, o.lat, o.lon);
    CHECK(at.x == Catch::Approx(0.0).margin(1e-9));
    CHECK(at.y == Catch::Approx(0.0).margin(1e-9));
    // One arc second north is the meridian arc: M(phi) = a (1 - e2) / (1 - e2 sin^2 phi)^1.5 per radian.
    const double e2 = (1.0 / 298.257223563) * (2.0 - 1.0 / 298.257223563);
    const double sp = std::sin(o.lat * std::numbers::pi / 180.0);
    const double m = 6378137.0 * (1.0 - e2) / std::pow(1.0 - e2 * sp * sp, 1.5);
    const double arcSecond = std::numbers::pi / 180.0 / 3600.0;
    const TrackPoint north = TrackGenerator::toEnu(o, o.lat + 1.0 / 3600.0, o.lon);
    CHECK(north.x == Catch::Approx(0.0).margin(1e-6));
    CHECK(north.y == Catch::Approx((m + o.alt) * arcSecond).epsilon(1e-6));
    // One arc second east is the parallel's arc: N(phi) cos(phi) per radian.
    const double nn = 6378137.0 / std::sqrt(1.0 - e2 * sp * sp);
    const TrackPoint east = TrackGenerator::toEnu(o, o.lat, o.lon + 1.0 / 3600.0);
    CHECK(east.x ==
          Catch::Approx((nn + o.alt) * std::cos(o.lat * std::numbers::pi / 180.0) * arcSecond).epsilon(1e-6));
    CHECK(east.y == Catch::Approx(0.0).margin(1e-3)); // the parallel curves away from the tangent plane
}

TEST_CASE("track generator: AC-M3-04 (a) GPS lies within 2 m RMS of the true centreline",
          "[unit][AC-M3-04]") {
    const CircuitFixture f = loadCircuit();
    TrackGeneratorOptions options = withOrigin(f);
    options.innerLap = f.innerGps;
    options.outerLap = f.outerGps;
    auto made = TrackGenerator::fromGps(f.gps, options, {});
    REQUIRE(made);
    const TrackMap& map = made->map;
    REQUIRE(map.centre.size() > 1000);
    CHECK(map.closed);
    CHECK(map.generatedFrom.method == TrackMethod::Gps);
    CHECK(map.direction == (f.ccw ? TrackDirection::CounterClockwise : TrackDirection::Clockwise));
    CHECK(map.origin == f.origin);
    CHECK(map.validate());
    CHECK(map.centre.front().dist == 0.0);
    CHECK(map.centre.back().dist == map.lengthM);
    CHECK(map.centre.back().x == map.centre.front().x); // the closing row
    CHECK(hasDiagnostic(made->report, "W-TRACK-DROPOUT"));

    const double rms = rmsToTruth(map.centre, f.centre.pts);
    const double innerRms = rmsToTruth(map.inner, f.inner.pts);
    const double outerRms = rmsToTruth(map.outer, f.outer.pts);
    const double lengthError = (map.lengthM - f.lengthM) / f.lengthM;
    WARN("AC-M3-04 (a): centreline RMS "
         << rms << " m (inner " << innerRms << ", outer " << outerRms << "); length " << map.lengthM
         << " m vs " << f.lengthM << " m (" << lengthError * 100.0 << " %); fit RMS " << made->report.rmsM
         << " m; closure " << made->report.closureErrorM << " m; " << made->report.validSamples << " fixes");
    CHECK(rms <= 2.0);
    CHECK(innerRms <= 2.0);
    CHECK(outerRms <= 2.0);
    CHECK(std::abs(lengthError) < 0.01);
    CHECK(made->report.rmsM > 0.5); // the fixes scatter about the line (sigma 1.5 m per axis)
    CHECK(made->report.rmsM < 3.0);
    CHECK(made->report.closureErrorM < 10.0);

    // The inner and outer lines carry the centreline distance of their nearest point.
    REQUIRE(!map.inner.empty());
    for (const TrackPoint& p : map.inner) {
        CHECK((p.dist >= 0.0 && p.dist < map.lengthM));
    }

    // The default sections of the GPS map find the seven corners, not the kink, each apex near the truth.
    TrackMap sectioned = map;
    REQUIRE(SectionDetector::defaults(sectioned));
    CHECK(sectioned.validate());
    const std::vector<TrackSection> corners = cornersOf(sectioned);
    CHECK(corners.size() == f.corners.size());
    const double scale = f.lengthM / map.lengthM;
    for (const TruthCorner& c : f.corners) {
        double apex = 0.0;
        const TrackSection* found = nearestCorner(sectioned, corners, c.apexM, scale, &apex);
        REQUIRE(found != nullptr);
        INFO(c.name << " truth apex " << c.apexM << " m, found " << apex << " m (" << found->name << ")");
        CHECK(std::abs(apex - c.apexM) <= 20.0);
    }
    for (const TruthCorner& k : f.kinks) {
        for (const TrackSection& s : corners) {
            INFO(k.name << " lies inside " << s.name);
            CHECK_FALSE((s.startM * scale < k.apexM && k.apexM < s.endM * scale));
        }
    }
}

TEST_CASE("track generator: AC-M3-04 (b) Speed+LatG matches the GPS map after a rigid alignment",
          "[unit][AC-M3-04]") {
    const CircuitFixture f = loadCircuit();
    auto gpsMade = TrackGenerator::fromGps(f.gps, withOrigin(f), {});
    REQUIRE(gpsMade);
    auto drMade = TrackGenerator::fromSpeedLatG(f.lap, f.speed, f.latAccel, TrackGeneratorOptions{}, {});
    REQUIRE(drMade);
    const TrackMap& gps = gpsMade->map;
    const TrackMap& dr = drMade->map;
    CHECK(dr.closed);
    CHECK(dr.generatedFrom.method == TrackMethod::SpeedLatG);
    CHECK(dr.direction == TrackDirection::CounterClockwise);
    CHECK(dr.validate());
    CHECK(drMade->report.validSamples > 3000);

    // Correspondence by the fraction of each map's length, then the best rotation and translation.
    constexpr int kPoints = 2000;
    std::vector<Xy2> a;
    std::vector<Xy2> b;
    for (int k = 0; k < kPoints; ++k) {
        const double u = static_cast<double>(k) / kPoints;
        const TrackPoint pa = dr.pointAt(u * dr.lengthM);
        const TrackPoint pb = gps.pointAt(u * gps.lengthM);
        a.push_back(Xy2{pa.x, pa.y});
        b.push_back(Xy2{pb.x, pb.y});
    }
    const double rms = rigidRms(a, b);

    TrackMap gpsSections = gps;
    TrackMap drSections = dr;
    REQUIRE(SectionDetector::defaults(gpsSections));
    REQUIRE(SectionDetector::defaults(drSections));
    CHECK(drSections.validate());
    const std::vector<TrackSection> gpsCorners = cornersOf(gpsSections);
    const std::vector<TrackSection> drCorners = cornersOf(drSections);
    WARN("AC-M3-04 (b): Speed+LatG RMS after rigid alignment "
         << rms << " m; length " << dr.lengthM << " m vs GPS " << gps.lengthM
         << " m; closure error before correction " << drMade->report.closureErrorM << " m; "
         << drCorners.size() << " corners vs " << gpsCorners.size());
    CHECK(rms <= 15.0);
    CHECK(drMade->report.closureErrorM > 0.0); // reported, not gated
    CHECK(drCorners.size() == gpsCorners.size());
    const double scale = gps.lengthM / dr.lengthM;
    double worst = 0.0;
    for (const TrackSection& g : gpsCorners) {
        const double target = apexOf(gpsSections, g);
        double apex = 0.0;
        const TrackSection* found = nearestCorner(drSections, drCorners, target, scale, &apex);
        REQUIRE(found != nullptr);
        INFO(g.name << " GPS apex " << target << " m, Speed+LatG apex " << apex << " m (" << found->name
                    << ")");
        CHECK(std::abs(apex - target) <= 20.0);
        worst = std::max(worst, std::abs(apex - target));
    }
    WARN("AC-M3-04 (b): worst apex difference " << worst << " m over " << gpsCorners.size() << " corners");
}

TEST_CASE("track generator: options, invalid input and cancellation", "[unit][AC-M3-04]") {
    const CircuitFixture f = loadCircuit();
    TrackGeneratorOptions bad;
    bad.stepM = 0.0;
    CHECK(TrackGenerator::fromGps(f.gps, bad, {}).error().code == ErrorCode::Invalid);
    bad = TrackGeneratorOptions{};
    bad.smoothingM = -1.0;
    CHECK(TrackGenerator::fromSpeedLatG(f.lap, f.speed, f.latAccel, bad, {}).error().code ==
          ErrorCode::Invalid);

    UniformSignal norate = f.speed;
    norate.rate = Rate{};
    CHECK(TrackGenerator::fromSpeedLatG(f.lap, norate, f.latAccel, {}, {}).error().code ==
          ErrorCode::Invalid);
    CHECK(TrackGenerator::fromSpeedLatG(LapWindow{f.lap.end, f.lap.start}, f.speed, f.latAccel, {}, {})
              .error()
              .code == ErrorCode::Invalid);

    // A lap inside the dropout holds no fixes; all-(0, 0) fixes are no fixes either.
    GpsLap empty = f.gps;
    empty.lap = LapWindow{f.dropoutStart + 100'000, f.dropoutEnd - 100'000};
    CHECK(TrackGenerator::fromGps(empty, {}, {}).error().code == ErrorCode::NotFound);
    GpsLap zeros = f.gps;
    std::fill(zeros.latDeg.values.begin(), zeros.latDeg.values.end(), 0.0);
    std::fill(zeros.lonDeg.values.begin(), zeros.lonDeg.values.end(), 0.0);
    CHECK(TrackGenerator::fromGps(zeros, {}, {}).error().code == ErrorCode::NotFound);

    CancelSource cancel;
    cancel.cancel();
    CHECK(TrackGenerator::fromGps(f.gps, {}, cancel.token()).error().code == ErrorCode::Cancelled);
    CHECK(TrackGenerator::fromSpeedLatG(f.lap, f.speed, f.latAccel, {}, cancel.token()).error().code ==
          ErrorCode::Cancelled);

    // The default origin is the lap's first fix; an open path keeps its two ends; no smoothing still works.
    TrackGeneratorOptions open;
    open.closeLoop = false;
    open.smoothingM = 0.0;
    open.stepM = 2.0;
    auto made = TrackGenerator::fromGps(f.gps, open, {});
    REQUIRE(made);
    CHECK_FALSE(made->map.closed);
    CHECK(std::abs(made->map.origin.lat - f.origin.lat) < 1e-4);
    CHECK(made->map.centre.size() > 1000);
    CHECK(made->map.centre[1].dist == Catch::Approx(2.0));
    CHECK(made->map.validate());

    // A longitude channel on its own grid (20 Hz, offset by 30 ms) is interpolated at the latitude's times.
    GpsLap split = f.gps;
    split.lonDeg.rate = Rate::fromHz(20);
    split.lonDeg.t0 = f.gps.lonDeg.t0 + 30'000;
    split.lonDeg.values.clear();
    split.lonDeg.valid.clear();
    for (std::size_t i = 0; i + 1 < 2 * f.gps.lonDeg.size(); ++i) {
        // Linear between the 10 Hz samples around t = t0 + 30 ms + i * 50 ms.
        const double at = (30'000.0 + 50'000.0 * static_cast<double>(i)) / 100'000.0;
        const auto k = static_cast<std::size_t>(at);
        const double u = at - static_cast<double>(k);
        const bool ok = k + 1 < f.gps.lonDeg.size() && f.gps.lonDeg.validAt(k) && f.gps.lonDeg.validAt(k + 1);
        split.lonDeg.values.push_back(
            ok ? f.gps.lonDeg.values[k] + u * (f.gps.lonDeg.values[k + 1] - f.gps.lonDeg.values[k]) : 0.0);
        split.lonDeg.valid.push_back(ok ? 1 : 0);
    }
    auto splitMade = TrackGenerator::fromGps(split, withOrigin(f), {});
    REQUIRE(splitMade);
    CHECK(rmsToTruth(splitMade->map.centre, f.centre.pts) <= 2.0);

    // A lateral channel logged positive to the right mirrors the map unless it is inverted.
    UniformSignal mirrored = f.latAccel;
    for (double& v : mirrored.values) {
        v = -v;
    }
    auto cw = TrackGenerator::fromSpeedLatG(f.lap, f.speed, mirrored, {}, {});
    REQUIRE(cw);
    CHECK(cw->map.direction == TrackDirection::Clockwise);
    TrackGeneratorOptions inverted;
    inverted.invertLatAccel = true;
    auto ccw = TrackGenerator::fromSpeedLatG(f.lap, f.speed, mirrored, inverted, {});
    REQUIRE(ccw);
    CHECK(ccw->map.direction == TrackDirection::CounterClockwise);
}

TEST_CASE("track generator: a straight run without a loop reports it and stays open-shaped", "[unit]") {
    // 60 s at 20 m/s with no lateral acceleration: a straight line, not a loop.
    UniformSignal speed;
    speed.rate = Rate::fromHz(10);
    speed.values.assign(600, 20.0);
    UniformSignal lat = speed;
    std::fill(lat.values.begin(), lat.values.end(), 0.0);
    TrackGeneratorOptions o;
    o.closeLoop = false;
    auto made = TrackGenerator::fromSpeedLatG(LapWindow{0, 59'900'000}, speed, lat, o, {});
    REQUIRE(made);
    CHECK(made->map.lengthM == Catch::Approx(1198.0).epsilon(1e-6));
    CHECK(made->report.closureErrorM == Catch::Approx(1198.0).epsilon(1e-6));
    CHECK(made->map.centre.back().x == Catch::Approx(1198.0).epsilon(1e-6));
    CHECK(made->map.centre.back().y == Catch::Approx(0.0).margin(1e-6));
    o.closeLoop = true;
    auto closed = TrackGenerator::fromSpeedLatG(LapWindow{0, 59'900'000}, speed, lat, o, {});
    REQUIRE(closed);
    CHECK(std::find(closed->report.diagnostics.begin(), closed->report.diagnostics.end(),
                    "W-TRACK-NOT-A-LOOP") != closed->report.diagnostics.end());
}
