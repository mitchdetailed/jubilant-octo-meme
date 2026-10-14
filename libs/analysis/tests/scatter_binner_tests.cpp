// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterBinner (04 section 10.1 items 1, 2, 5, 9): the point sets on the union grid at the highest rate with
// each channel's own Interp, yDelay and gating equal a brute-force evaluation on plain arrays; the series
// cursor behind them equals store::Resampler::valueAt for every policy; the interval average and the
// density grid equal hand-computed values.
#include "analysis_test_support.hpp"
#include "binning/series_cursor.hpp"

#include <esses/store/resampler.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <variant>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Plain { // any series as plain arrays
    std::vector<Micros> t;
    std::vector<double> v;
    std::vector<bool> ok;
};

Plain uniformPlain(Rate rate, Micros t0, std::size_t n, double (*f)(std::size_t), int invalidEvery = 0) {
    Plain p;
    for (std::size_t i = 0; i < n; ++i) {
        p.t.push_back(rate.timeAt(static_cast<SampleIdx>(i), t0));
        p.v.push_back(f(i));
        p.ok.push_back(invalidEvery == 0 || i % static_cast<std::size_t>(invalidEvery) != 3);
    }
    return p;
}

// store::Resampler rule 13 on plain arrays, by linear search.
double refAt(const Plain& p, Micros t, store::Interp interp) {
    if (p.t.empty() || t < p.t.front() || t > p.t.back()) {
        return kNaN;
    }
    std::size_t j = 0;
    while (j + 1 < p.t.size() && p.t[j + 1] <= t) {
        ++j;
    }
    const auto value = [&](std::size_t i) { return p.ok[i] ? p.v[i] : kNaN; };
    if (p.t[j] == t) {
        return value(j);
    }
    switch (interp) {
    case store::Interp::Previous:
        return value(j);
    case store::Interp::Next:
        return value(j + 1);
    case store::Interp::Closest:
        return value(t - p.t[j] <= p.t[j + 1] - t ? j : j + 1);
    case store::Interp::Linear:
        break;
    }
    if (!p.ok[j] || !p.ok[j + 1]) {
        return kNaN;
    }
    const double f = static_cast<double>(t - p.t[j]) / static_cast<double>(p.t[j + 1] - p.t[j]);
    return p.v[j] + f * (p.v[j + 1] - p.v[j]);
}

bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == Catch::Approx(b).epsilon(1e-12).margin(1e-12);
}

} // namespace

TEST_CASE("scatter: the series cursor equals Resampler::valueAt for every policy", "[unit]") {
    TestCache tc;
    const Plain u = uniformPlain(
        Rate{50, 3}, 12'345, 400, [](std::size_t i) { return std::sin(0.1 * static_cast<double>(i)); }, 7);
    std::vector<bool> valid(u.ok.begin(), u.ok.end());
    const store::Series uniform = tc.uniform(Rate{50, 3}, u.v, valid, 12'345);
    Plain s;
    std::mt19937_64 rng(testSeed());
    Micros at = 5'000;
    for (int i = 0; i < 300; ++i) {
        at += 1 + static_cast<Micros>(rng() % 90'000);
        s.t.push_back(at);
        s.v.push_back(static_cast<double>(i % 17) - 4.0);
        s.ok.push_back(true);
    }
    const store::Series stamped = tc.timestamped(s.t, s.v);
    for (const store::Series* series : {&uniform, &stamped}) {
        const Plain& plain = series == &uniform ? u : s;
        // Pin everything so that valueAt (which never blocks) sees resident chunks.
        auto pinned =
            store::seriesValues(*series).require(0, store::seriesSize(*series) - 1, Priority::Visible, {});
        REQUIRE(pinned.ok());
        std::optional<store::ChunkHandle> pinnedTimes;
        if (const auto* ts = std::get_if<store::TimestampedSeries>(series)) {
            auto times = ts->timesUs.require(0, ts->size() - 1, Priority::Visible, {});
            REQUIRE(times.ok());
            pinnedTimes = std::move(times).value();
        }
        for (const store::Interp interp :
             {store::Interp::Linear, store::Interp::Closest, store::Interp::Previous, store::Interp::Next}) {
            analysis::detail::SeriesCursor cursor(*series, interp, Priority::Visible, {});
            std::vector<Micros> queries;
            queries.reserve(2002);
            for (int k = 0; k < 2000; ++k) {
                queries.push_back(
                    static_cast<Micros>(rng() % static_cast<std::uint64_t>(plain.t.back() + 20'000)));
            }
            std::sort(queries.begin(), queries.end());
            queries.insert(queries.begin() + 1000,
                           plain.t[10]); // a step back (a fresh search) and exact hits
            queries.push_back(plain.t.back());
            for (const Micros q : queries) {
                auto got = cursor.at(q);
                REQUIRE(got.ok());
                const double viaResampler = store::Resampler::valueAt(*series, q, interp);
                INFO("t " << q << " interp " << static_cast<int>(interp));
                CHECK(same(*got, viaResampler));
                CHECK(same(*got, refAt(plain, q, interp)));
            }
        }
    }
}

TEST_CASE("scatter: gather equals brute force on the union grid with delays and gating", "[unit]") {
    TestCache tc;
    // X 10 Hz (some invalid), Y1 20 Hz, Y2 50 Hz from 7 ms (the grid), Z timestamped, gating 5 Hz.
    const Plain x = uniformPlain(
        Rate::fromHz(10), 0, 101, [](std::size_t i) { return 10.0 * std::sin(0.2 * static_cast<double>(i)); },
        9);
    const Plain y1 =
        uniformPlain(Rate::fromHz(20), 0, 201, [](std::size_t i) { return 0.05 * static_cast<double>(i); });
    const Plain y2 = uniformPlain(
        Rate::fromHz(50), 7'000, 499, [](std::size_t i) { return std::cos(0.03 * static_cast<double>(i)); },
        11);
    Plain z;
    for (int i = 0; i < 40; ++i) {
        z.t.push_back(static_cast<Micros>(i) * 250'000 + static_cast<Micros>(i % 3) * 17'000);
        z.v.push_back(static_cast<double>(i));
        z.ok.push_back(true);
    }
    const Plain g = uniformPlain(Rate::fromHz(5), 0, 50,
                                 [](std::size_t i) { return i % 4 == 1 ? 0.0 : 1.0; }); // 0..9.8 s
    const auto valid = [](const Plain& p) { return std::vector<bool>(p.ok.begin(), p.ok.end()); };

    ScatterInput input;
    input.x = ScatterChannel{tc.uniform(Rate::fromHz(10), x.v, valid(x)), units::Converter{2.0, 0.0},
                             store::Interp::Linear};
    input.y.push_back(ScatterChannel{tc.uniform(Rate::fromHz(20), y1.v, valid(y1)),
                                     units::Converter{1.0, 1.0}, store::Interp::Closest});
    input.y.push_back(ScatterChannel{tc.uniform(Rate::fromHz(50), y2.v, valid(y2), 7'000), units::Converter{},
                                     store::Interp::Linear});
    input.yDelayUs = {30'000}; // Y2 has none (0)
    input.colour =
        ScatterChannel{tc.timestamped(z.t, z.v), units::Converter{0.5, 0.0}, store::Interp::Previous};
    input.gating = store::Series{tc.uniform(Rate::fromHz(5), g.v, valid(g))};
    input.spans = {SampleSpan{sec(0.5), sec(4.0), true}, SampleSpan{sec(6.0), sec(11.0), false}};

    auto got = ScatterBinner::gather(input, Priority::Visible, {});
    REQUIRE(got.ok());
    REQUIRE(got->size() == 2);
    const Rate grid = Rate::fromHz(50);
    for (std::size_t k = 0; k < 2; ++k) {
        INFO("trace " << k);
        const Plain& y = k == 0 ? y1 : y2;
        const store::Interp yInterp = k == 0 ? store::Interp::Closest : store::Interp::Linear;
        const Micros delay = k == 0 ? 30'000 : 0;
        ScatterSeries want;
        for (const SampleSpan& span : input.spans) {
            for (SampleIdx i = 0; i < 2000; ++i) {
                const Micros t = grid.timeAt(i, 7'000);
                if (t < span.start || t >= span.end) {
                    continue;
                }
                if (!(refAt(g, t, store::Interp::Previous) > 0.0)) {
                    continue;
                }
                const double xv = refAt(x, t, store::Interp::Linear);
                const double yv = refAt(y, t + delay, yInterp);
                if (std::isnan(xv) || std::isnan(yv)) {
                    continue;
                }
                want.x.push_back(2.0 * xv);
                want.y.push_back(k == 0 ? yv + 1.0 : yv);
                const double zv = refAt(z, t, store::Interp::Previous);
                want.z.push_back(std::isnan(zv) ? kNaN : 0.5 * zv);
                want.t.push_back(t);
            }
        }
        const ScatterSeries& s = (*got)[k];
        REQUIRE(want.t.size() > 100);
        REQUIRE(s.t == want.t);
        REQUIRE(s.x.size() == want.x.size());
        REQUIRE(s.y.size() == want.y.size());
        REQUIRE(s.z.size() == want.z.size());
        for (std::size_t p = 0; p < want.t.size(); ++p) {
            INFO("point " << p);
            CHECK(same(s.x[p], want.x[p]));
            CHECK(same(s.y[p], want.y[p]));
            CHECK(same(s.z[p], want.z[p]));
        }
    }

    // Without a colour channel z stays empty; without y there is nothing; a timestamped x is refused.
    ScatterInput plain = input;
    plain.colour.reset();
    auto noZ = ScatterBinner::gather(plain, Priority::Visible, {});
    REQUIRE(noZ.ok());
    CHECK((*noZ)[0].z.empty());
    CHECK((*noZ)[0].t == (*got)[0].t);
    plain.y.clear();
    CHECK(ScatterBinner::gather(plain, Priority::Visible, {})->empty());
    plain.x.series = tc.timestamped({0, 1000}, {1, 2});
    CHECK(ScatterBinner::gather(plain, Priority::Visible, {}).error().code == ErrorCode::Invalid);
    CancelSource cancel;
    cancel.cancel();
    CHECK(ScatterBinner::gather(input, Priority::Visible, cancel.token()).error().code ==
          ErrorCode::Cancelled);
}

TEST_CASE("scatter: interval average and density grid", "[unit]") {
    const std::vector<double> x{0.5, 1.2, 1.9, -0.3, 2.0, 7.9, kNaN, 3.0};
    const std::vector<double> y{1, 2, 3, 4, 5, 6, 7, std::numeric_limits<double>::infinity()};
    auto avg = ScatterBinner::intervalAverage(x, y, 1.0);
    REQUIRE(avg.ok());
    CHECK(avg->binLo == std::vector<double>{-1, 0, 1, 2, 7});
    CHECK(avg->mean == std::vector<double>{4, 1, 2.5, 5, 6});
    auto half = ScatterBinner::intervalAverage(x, y, 0.5);
    REQUIRE(half.ok());
    CHECK(half->binLo == std::vector<double>{-0.5, 0.5, 1.0, 1.5, 2.0, 7.5});
    // Far-apart bins take the sorting path and give the same kind of answer.
    auto sparse = ScatterBinner::intervalAverage(std::vector<double>{1e9 + 0.5, 0.25, 1e9 + 0.75, 0.5},
                                                 std::vector<double>{1, 2, 3, 6}, 1.0);
    REQUIRE(sparse.ok());
    CHECK(sparse->binLo == std::vector<double>{0, 1e9});
    CHECK(sparse->mean == std::vector<double>{4, 2});
    CHECK(ScatterBinner::intervalAverage(x, y, 0.0).error().code == ErrorCode::Invalid);
    CHECK(ScatterBinner::intervalAverage(x, y, kNaN).error().code == ErrorCode::Invalid);
    CHECK(ScatterBinner::intervalAverage(x, std::vector<double>{1}, 1.0).error().code == ErrorCode::Invalid);
    CHECK(ScatterBinner::intervalAverage({}, {}, 1.0)->binLo.empty());

    ScatterSeries s;
    s.x = {0.0, 0.5, 1.0, 0.25, 2.0, 0.99, kNaN};
    s.y = {0.0, 0.5, 1.0, 0.75, 0.0, 0.01, 0.5};
    const DensityGrid grid = ScatterBinner::density(s, 2, 2, 0.0, 1.0, 0.0, 1.0);
    CHECK(grid.nx == 2);
    CHECK(grid.ny == 2);
    CHECK(grid.counts == std::vector<std::uint32_t>{1, 1, 1, 2}); // row 0 at y0
    CHECK(ScatterBinner::density(s, 0, 2, 0.0, 1.0, 0.0, 1.0).counts.empty());
    CHECK(ScatterBinner::density(s, 2, 2, 1.0, 1.0, 0.0, 1.0).nx == 0);
    CHECK(ScatterBinner::density(s, 2, 2, 0.0, 1.0, 0.0, kNaN).ny == 0);
    const DensityGrid fine = ScatterBinner::density(s, 256, 256, -1.0, 3.0, -1.0, 2.0);
    std::uint64_t total = 0;
    for (const std::uint32_t c : fine.counts) {
        total += c;
    }
    CHECK(total == 6);
}
