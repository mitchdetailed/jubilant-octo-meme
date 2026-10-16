// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// VarianceTable (D-023, 04 section 8.8; AC-M3-05): the variance panel equals a brute-force reference within
// +-1 ms at every column for 3 overlay pairs (distance mode; the time mode within 0.1 mm), with alignment
// offsets and lap stretch; columns outside either lap are blank; an empty index refuses with the banner text.
// The reference reads the raw samples by linear search (the first sample reaching a distance, then linear
// interpolation on its segment), never the 0.25 m step table of DistanceIndex.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Lap {
    std::vector<Micros> t; // absolute sample times; t.front() is the lap start, t.back() its end
    std::vector<double> d; // cumulative distance (m)
    DistanceIndex idx;
    double lengthM = 0.0; // maximum distance reached since the lap start
    double durationS = 0.0;
};

// A lap of `seconds` at `hz` starting at `start`, speed v(s) = base + swing * sin(2 pi s / period), with an
// optional stop (speed 0) over [stopFrom, stopTo) seconds; distance by the trapezoid rule.
Lap makeLap(Micros start, std::uint32_t hz, double seconds, double base, double swing, double period,
            double stopFrom = -1.0, double stopTo = -1.0, double stretch = 1.0, double distOffset = 1234.5) {
    const Rate rate = Rate::fromHz(hz);
    const auto n = static_cast<std::size_t>(seconds * hz) + 1;
    UniformSignal speed;
    speed.rate = rate;
    speed.values.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double s = static_cast<double>(i) / hz;
        speed.values[i] = (s >= stopFrom && s < stopTo)
                              ? 0.0
                              : base + swing * std::sin(2.0 * std::numbers::pi * s / period);
    }
    auto dist = CorrectedDistance::integrate(speed);
    REQUIRE(dist.ok());
    Lap lap;
    lap.t.resize(n);
    lap.d.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        lap.t[i] = start + rate.timeAt(static_cast<SampleIdx>(i), 0);
        lap.d[i] = distOffset + dist->values[i]; // Corr Dist is cumulative over the outing
    }
    DistanceIndexKey key;
    key.corrDistVersion = 1;
    key.start = lap.t.front();
    key.end = lap.t.back();
    auto idx = DistanceIndex::fromSamples(lap.t, lap.d, key);
    REQUIRE(idx.ok());
    lap.idx = std::move(idx).value();
    lap.idx.setStretch(stretch);
    lap.lengthM = lap.d.back() - lap.d.front();
    lap.durationS = static_cast<double>(lap.t.back() - lap.t.front()) / 1e6;
    return lap;
}

// Reference: seconds since the lap start at which the lap first reaches `metres` (unstretched).
double refTimeAt(const Lap& lap, double metres) {
    const double d0 = lap.d.front();
    for (std::size_t i = 0; i < lap.d.size(); ++i) {
        if (lap.d[i] - d0 >= metres) {
            if (i == 0) {
                return 0.0;
            }
            const double a = lap.d[i - 1] - d0;
            const double b = lap.d[i] - d0;
            const double ta = static_cast<double>(lap.t[i - 1] - lap.t.front()) / 1e6;
            const double tb = static_cast<double>(lap.t[i] - lap.t.front()) / 1e6;
            return ta + (metres - a) / (b - a) * (tb - ta);
        }
    }
    return lap.durationS;
}

// Reference: metres since the lap start at `seconds` since the lap start (unstretched).
double refDistAt(const Lap& lap, double seconds) {
    const double at = static_cast<double>(lap.t.front()) + seconds * 1e6;
    for (std::size_t i = 1; i < lap.t.size(); ++i) {
        const auto ta = static_cast<double>(lap.t[i - 1]);
        const auto tb = static_cast<double>(lap.t[i]);
        if (at <= tb) {
            return lap.d[i - 1] - lap.d.front() + (at - ta) / (tb - ta) * (lap.d[i] - lap.d[i - 1]);
        }
    }
    return lap.d.back() - lap.d.front();
}

// The brute-force variance of the overlay against Main at x (offsets and stretch as VarianceTable applies
// them, 04 section 3.3): NaN outside the Main lap [0, extent) or the overlay lap [0, extent].
double reference(const Lap& main, const Lap& overlay, VarianceMode mode, double x, double mainOffset,
                 double overlayOffset) {
    const double xm = x - mainOffset;
    const double xo = x - overlayOffset;
    if (mode == VarianceMode::TimeAtDistance) {
        const double sm = main.idx.stretch();
        const double so = overlay.idx.stretch();
        if (xm < 0.0 || xm >= main.lengthM * sm || xo < 0.0 || xo > overlay.lengthM * so) {
            return kNaN;
        }
        return refTimeAt(overlay, xo / so) - refTimeAt(main, xm / sm);
    }
    if (xm < 0.0 || xm >= main.durationS || xo < 0.0 || xo > overlay.durationS) {
        return kNaN;
    }
    return refDistAt(overlay, xo) * overlay.idx.stretch() - refDistAt(main, xm) * main.idx.stretch();
}

struct Pair {
    const Lap* overlay;
    double mainOffset;
    double overlayOffset;
};

} // namespace

TEST_CASE("variance: dt(d) equals the brute-force reference within 1 ms for 3 overlay pairs",
          "[unit][AC-M3-05]") {
    // Main 50 Hz; overlay 1 faster at 50 Hz; overlay 2 at 20 Hz with a 2 s stop and a stretch; overlay 3 at
    // 100 Hz, slower, starting later in the outing, stretched the other way.
    const Lap main = makeLap(sec(100), 50, 90.0, 45.0, 15.0, 17.0);
    const Lap fast = makeLap(sec(300), 50, 88.0, 46.0, 14.0, 16.0);
    const Lap stop = makeLap(sec(500), 20, 95.0, 44.0, 16.0, 18.0, 40.0, 42.0, 1.0015);
    const Lap slow = makeLap(sec(777.777777), 100, 92.0, 44.5, 13.0, 19.0, -1.0, -1.0, 0.9985);
    const Pair pairs[] = {{&fast, 0.0, 0.0}, {&stop, 0.5, -1.25}, {&slow, -2.0, 3.75}};
    const double extent = main.lengthM * main.idx.stretch();
    VarianceRequest request;
    request.mode = VarianceMode::TimeAtDistance;
    request.x0 = -15.0; // the visible span reaches into the neighbouring laps on both sides
    request.x1 = extent + 15.0;
    request.columns = 1601;
    for (const Pair& pair : pairs) {
        request.mainOffset = pair.mainOffset;
        request.overlayOffset = pair.overlayOffset;
        auto table = VarianceTable::build(main.idx, pair.overlay->idx, request);
        REQUIRE(table.ok());
        CHECK(table->mode() == VarianceMode::TimeAtDistance);
        REQUIRE(table->size() == 1601);
        double worst = 0.0;
        std::size_t finite = 0;
        std::optional<std::pair<double, double>> range;
        for (std::size_t k = 0; k < table->size(); ++k) {
            const double x = table->xAt(k);
            CHECK(x == Catch::Approx(request.x0 +
                                     (request.x1 - request.x0) * (static_cast<double>(k) + 0.5) / 1601.0));
            const double want =
                reference(main, *pair.overlay, request.mode, x, pair.mainOffset, pair.overlayOffset);
            const double got = table->values()[k];
            INFO("column " << k << " x " << x);
            if (std::isnan(want)) {
                CHECK(std::isnan(got));
                continue;
            }
            REQUIRE(std::isfinite(got));
            worst = std::max(worst, std::abs(got - want));
            ++finite;
            range = range ? std::make_pair(std::min(range->first, got), std::max(range->second, got))
                          : std::make_pair(got, got);
        }
        INFO("worst |dt - reference| " << worst << " s over " << finite << " columns");
        CHECK(worst <= 1e-3); // AC-M3-05: +-1 ms at every column
        CHECK(finite > 1500);
        CHECK(table->valueRange() == range);
        // The single-point lookup equals a one-column table with zero offsets.
        const double d = 0.37 * extent;
        CHECK(varianceTimeAt(main.idx, pair.overlay->idx, d) ==
              Catch::Approx(reference(main, *pair.overlay, request.mode, d, 0.0, 0.0)).margin(1e-6));
    }
    // The stop costs the overlay at least 2 s by the end of the lap.
    request.mainOffset = request.overlayOffset = 0.0;
    auto late = VarianceTable::build(main.idx, stop.idx, request);
    REQUIRE(late.ok());
    CHECK(varianceTimeAt(main.idx, stop.idx, 0.99 * extent) > 2.0);
}

TEST_CASE("variance: dd(t) equals the brute-force reference in the time domain", "[unit][AC-M3-05]") {
    const Lap main = makeLap(sec(10), 50, 60.0, 50.0, 10.0, 13.0);
    const Lap other = makeLap(sec(200), 20, 62.0, 48.0, 12.0, 11.0, 20.0, 21.5, 1.002);
    VarianceRequest request;
    request.mode = VarianceMode::DistanceAtTime;
    request.x0 = -2.0;
    request.x1 = main.durationS + 2.0;
    request.columns = 900;
    request.mainOffset = 0.25;
    request.overlayOffset = -0.4;
    auto table = VarianceTable::build(main.idx, other.idx, request);
    REQUIRE(table.ok());
    std::size_t finite = 0;
    for (std::size_t k = 0; k < table->size(); ++k) {
        const double want =
            reference(main, other, request.mode, table->xAt(k), request.mainOffset, request.overlayOffset);
        const double got = table->values()[k];
        INFO("column " << k);
        if (std::isnan(want)) {
            CHECK(std::isnan(got));
        } else {
            ++finite;
            CHECK(got == Catch::Approx(want).margin(1e-4)); // metres; DistanceIndex reads whole microseconds
        }
    }
    CHECK(finite > 800); // 60 s of Main in 64 s of columns
    CHECK(varianceDistanceAt(main.idx, other.idx, 30.0) ==
          Catch::Approx(reference(main, other, request.mode, 30.0, 0.0, 0.0)).margin(1e-4));
    CHECK(std::isnan(varianceDistanceAt(main.idx, other.idx, -0.1)));
    CHECK(std::isnan(varianceDistanceAt(main.idx, other.idx, main.durationS))); // [0, extent)
}

TEST_CASE("variance: requests without distance data or columns are refused", "[unit]") {
    const Lap main = makeLap(0, 10, 20.0, 30.0, 5.0, 7.0);
    VarianceRequest request;
    request.x1 = 100.0;
    request.columns = 0;
    CHECK(VarianceTable::build(main.idx, main.idx, request).error().code == ErrorCode::Invalid);
    request.columns = 10;
    request.x1 = -1.0;
    CHECK(VarianceTable::build(main.idx, main.idx, request).error().code == ErrorCode::Invalid);
    request.x1 = 100.0;
    request.overlayOffset = kNaN;
    CHECK(VarianceTable::build(main.idx, main.idx, request).error().code == ErrorCode::Invalid);
    request.overlayOffset = 0.0;
    auto empty = VarianceTable::build(main.idx, DistanceIndex{}, request);
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().code == ErrorCode::Invalid);
    CHECK(empty.error().context.detail == "variance needs distance data");
    CHECK(VarianceTable::build(DistanceIndex{}, main.idx, request).error().context.detail ==
          "variance needs distance data");
    CHECK(std::isnan(varianceTimeAt(main.idx, DistanceIndex{}, 10.0)));

    // A lap against itself is 0 wherever it is defined; x0 == x1 samples one point.
    request.x0 = request.x1 = 50.0;
    request.columns = 3;
    auto self = VarianceTable::build(main.idx, main.idx, request);
    REQUIRE(self.ok());
    for (const double v : self->values()) {
        CHECK(v == 0.0);
    }
    CHECK(self->valueRange() == std::make_pair(0.0, 0.0));
    const VarianceTable none;
    CHECK(none.size() == 0);
    CHECK(std::isnan(none.xAt(0)));
    CHECK_FALSE(none.valueRange().has_value());
}
