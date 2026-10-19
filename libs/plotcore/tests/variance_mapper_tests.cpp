// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M3 item 29 (D-210 (f)): analysis::VarianceTable agrees with what the graph's traces show. The expected
// variance of every column comes from plot::DomainMapper::xOf, the mapping time_distance draws a slot's trace
// with (alignment offset and lap stretch applied through the slot's DistanceChain), never from
// DistanceIndex's lookups that VarianceTable reads:
//   distance mode  dt(x) = tK(x) - tM(x), where tS(x) is the lap time at which slot S's trace reaches x
//                  (found by bisection on xOf over the lap's microseconds, interpolated within the last one)
//   time mode      dd(x) = xOfK(tK) - xOfM(tM) with the time-domain mapper's sample time tS of x and a
//                  distance-domain mapper without an offset
// and a column is blank exactly where x lies outside [xOfM(M.start), xOfM(M.end)) or [xOfK(K.start),
// xOfK(K.end)]. Two overlay pairs with different speed profiles, stretches and offsets.
#include "plotcore_test_support.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/variance_table.hpp>
#include <esses/plotcore/domain_mapper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

constexpr Micros kS = 1'000'000;

// A lap [start, start + seconds) at speed base + swing sin(2 pi s / period), sampled at 10 Hz, its distance
// by the trapezoid rule; stretched by `stretch` (the index scales nothing, the chain does, D-016).
struct Lap {
    Micros start = 0;
    Micros end = 0;
    double stretch = 1.0;
    std::shared_ptr<const analysis::DistanceIndex> index;
};

Lap makeLap(Micros start, int seconds, double base, double swing, double period, double stretch) {
    std::vector<Micros> t;
    std::vector<double> d;
    double covered = 0.0;
    double previous = base;
    for (int i = 0; i <= seconds * 10; ++i) {
        const double s = static_cast<double>(i) / 10.0;
        const double v = base + swing * std::sin(2.0 * std::numbers::pi * s / period);
        if (i > 0) {
            covered += 0.5 * (previous + v) * 0.1;
        }
        previous = v;
        t.push_back(start + static_cast<Micros>(i) * 100'000);
        d.push_back(500.0 + covered); // Corr Dist is cumulative over the outing
    }
    analysis::DistanceIndexKey key;
    key.start = start;
    key.end = t.back();
    auto idx = analysis::DistanceIndex::fromSamples(t, d, key);
    REQUIRE(idx.ok());
    analysis::DistanceIndex stretched = std::move(idx).value();
    stretched.setStretch(stretch);
    Lap lap;
    lap.start = start;
    lap.end = key.end;
    lap.stretch = stretch;
    lap.index = std::make_shared<const analysis::DistanceIndex>(std::move(stretched));
    return lap;
}

// The mapper a trace of this lap is drawn with: one-lap outing, the slot's offset.
DomainMapper mapperOf(const Lap& lap, Domain domain, Micros offsetUs, double offsetM) {
    std::vector<LapPiece> pieces(1);
    pieces[0].start = lap.start;
    pieces[0].end = lap.end;
    pieces[0].idx = lap.index;
    pieces[0].stretch = lap.stretch;
    auto chain = std::make_shared<const DistanceChain>(std::move(pieces), lap.start);
    return DomainMapper(domain, RangeGeom{lap.start, lap.end, offsetUs, offsetM, lap.stretch},
                        OutingGeom{lap.start, lap.end}, chain, Rate{}, 0);
}

// Seconds from the lap start at which the distance-domain trace reaches x: the first microsecond whose xOf
// is >= x (xOf increases: every speed is positive), then linear within the microsecond before it.
double secondsToReach(const DomainMapper& m, const Lap& lap, double x) {
    Micros lo = lap.start;
    Micros hi = lap.end;
    if (m.xOf(lo) >= x) {
        return 0.0;
    }
    while (hi - lo > 1) {
        const Micros mid = lo + (hi - lo) / 2;
        (m.xOf(mid) >= x ? hi : lo) = mid;
    }
    const double a = m.xOf(lo);
    const double b = m.xOf(hi);
    const double frac = b > a ? (x - a) / (b - a) : 0.0;
    return (static_cast<double>(lo - lap.start) + frac) / 1e6;
}

struct Pair {
    Lap main;
    Lap overlay;
    double mainOffset = 0.0;    // metres (distance mode) or seconds (time mode)
    double overlayOffset = 0.0; //
};

void checkDistanceMode(const Pair& p) {
    const DomainMapper m = mapperOf(p.main, Domain::Distance, 0, p.mainOffset);
    const DomainMapper k = mapperOf(p.overlay, Domain::Distance, 0, p.overlayOffset);
    const double x0 = -20.0;
    const double x1 = m.extent() + 40.0; // both ends reach past the laps
    constexpr int kColumns = 600;
    analysis::VarianceRequest request;
    request.mode = analysis::VarianceMode::TimeAtDistance;
    request.x0 = x0;
    request.x1 = x1;
    request.columns = kColumns;
    request.mainOffset = p.mainOffset;
    request.overlayOffset = p.overlayOffset;
    const Result<analysis::VarianceTable> table =
        analysis::VarianceTable::build(*p.main.index, *p.overlay.index, request);
    REQUIRE(table.ok());
    REQUIRE(table->size() == static_cast<std::size_t>(kColumns));
    int compared = 0;
    int blank = 0;
    for (int c = 0; c < kColumns; ++c) {
        const double x = x0 + (x1 - x0) * (static_cast<double>(c) + 0.5) / kColumns;
        INFO("column " << c << " at " << x << " m");
        CHECK(std::abs(table->xAt(static_cast<std::size_t>(c)) - x) < 1e-9);
        const double got = table->values()[static_cast<std::size_t>(c)];
        const bool onMain = x >= m.xOf(p.main.start) && x < m.xOf(p.main.end);
        const bool onOverlay = x >= k.xOf(p.overlay.start) && x <= k.xOf(p.overlay.end);
        if (!onMain || !onOverlay) {
            CHECK(std::isnan(got));
            ++blank;
            continue;
        }
        const double want = secondsToReach(k, p.overlay, x) - secondsToReach(m, p.main, x);
        CHECK(std::abs(got - want) < 2e-6);
        ++compared;
    }
    CHECK(compared > kColumns / 2);
    CHECK(blank > 0);
}

void checkTimeMode(const Pair& p) {
    const auto offM = static_cast<Micros>(std::llround(p.mainOffset * 1e6));
    const auto offK = static_cast<Micros>(std::llround(p.overlayOffset * 1e6));
    const DomainMapper mt = mapperOf(p.main, Domain::Time, offM, 0.0);
    const DomainMapper kt = mapperOf(p.overlay, Domain::Time, offK, 0.0);
    const DomainMapper md = mapperOf(p.main, Domain::Distance, 0, 0.0);
    const DomainMapper kd = mapperOf(p.overlay, Domain::Distance, 0, 0.0);
    const double x0 = -3.0;
    const double x1 = mt.extent() / 1e6 + 4.0;
    constexpr int kColumns = 500;
    analysis::VarianceRequest request;
    request.mode = analysis::VarianceMode::DistanceAtTime;
    request.x0 = x0;
    request.x1 = x1;
    request.columns = kColumns;
    request.mainOffset = p.mainOffset;
    request.overlayOffset = p.overlayOffset;
    const Result<analysis::VarianceTable> table =
        analysis::VarianceTable::build(*p.main.index, *p.overlay.index, request);
    REQUIRE(table.ok());
    int compared = 0;
    int blank = 0;
    for (int c = 0; c < kColumns; ++c) {
        const double x = x0 + (x1 - x0) * (static_cast<double>(c) + 0.5) / kColumns;
        INFO("column " << c << " at " << x << " s");
        const double got = table->values()[static_cast<std::size_t>(c)];
        const double us = x * 1e6; // time-domain x is in microseconds from the Main lap start
        const bool onMain = us >= mt.xOf(p.main.start) && us < mt.xOf(p.main.end);
        const bool onOverlay = us >= kt.xOf(p.overlay.start) && us <= kt.xOf(p.overlay.end);
        if (!onMain || !onOverlay) {
            CHECK(std::isnan(got));
            ++blank;
            continue;
        }
        const double want = kd.xOf(kt.timeAt(us)) - md.xOf(mt.timeAt(us));
        CHECK(std::abs(got - want) < 1e-4); // one microsecond of rounding at ~35 m/s
        ++compared;
    }
    CHECK(compared > kColumns / 2);
    CHECK(blank > 0);
}

} // namespace

TEST_CASE("variance: the table equals what the traces' DomainMapper::xOf shows (M3 item 29)", "[unit]") {
    // Main 60 s at 30 +- 5 m/s; overlay A slower on average and stretched up, overlay B faster with another
    // period and stretched down, each with alignment offsets on both slots.
    const Lap main = makeLap(10 * kS, 60, 30.0, 5.0, 20.0, 1.0);
    const Pair a{main, makeLap(200 * kS, 62, 29.0, 4.0, 20.0, 1.02), 2.0, 5.0};
    const Pair b{makeLap(10 * kS, 60, 30.0, 5.0, 20.0, 0.99), makeLap(400 * kS, 57, 32.0, 6.0, 15.0, 0.98),
                 -1.5, -4.0};
    SECTION("distance mode, overlay A") {
        checkDistanceMode(a);
    }
    SECTION("distance mode, overlay B") {
        checkDistanceMode(b);
    }
    SECTION("time mode, overlay A") {
        checkTimeMode(Pair{a.main, a.overlay, 0.5, -1.25});
    }
    SECTION("time mode, overlay B") {
        checkTimeMode(Pair{b.main, b.overlay, -0.75, 0.4});
    }
}
