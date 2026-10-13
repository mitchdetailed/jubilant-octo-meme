// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// 04 section 4.9 property tests: TraceBuilder::decimate on the chunk path (regimes A, B, C) equals a
// brute-force per-column reference bit for bit on first/last/lo/hi/flags/xFrac, for random F32, F64 and I32
// columns, uniform and timestamped, with NaN sprinkles, invalid runs, an all-invalid chunk and a partial last
// chunk, at every zoom from 0.1 to 10^6 samples per pixel and random offsets, in time and distance mode
// (stopped-car plateaus, a lap without an index). The reference folds raw samples; for regime-C columns of
// F64 and I32 it applies the pyramid's directional float rounding (04 section 4.1), which is exactly the
// enclosure the pyramid returns. Also: the summary path's bound, decimateMany equals sequential decimation,
// and cancellation.

#include "plotcore_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/plotcore/trace_builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

struct Data {
    store::Dtype dtype = store::Dtype::F32;
    std::vector<double> v;   // the exact stored value
    std::vector<bool> valid; // effective validity
    std::vector<Micros> t;   // sample times
    store::Column column;
    store::Column times; // valid() for a timestamped series
    Rate rate;
    Micros t0 = 0;

    SampleIdx size() const { return static_cast<SampleIdx>(v.size()); }
};

Data makeData(const test::Store& st, store::Dtype dtype, SampleIdx n, bool timestamped, Rate rate,
              std::mt19937_64& rng) {
    Data d;
    d.dtype = dtype;
    const auto un = static_cast<std::size_t>(n);
    d.v.resize(un);
    d.valid.assign(un, true);
    d.t.resize(un);
    std::vector<bool> flags(un, true);
    std::uniform_int_distribution<int> pct(0, 99);
    std::normal_distribution<double> normal(0.0, 100.0);
    std::uniform_int_distribution<std::int32_t> big(-(1 << 30), 1 << 30);
    for (std::size_t i = 0; i < un; ++i) {
        flags[i] = pct(rng) != 0; // 1 % sprinkles
    }
    std::uniform_int_distribution<SampleIdx> start(0, n - 1);
    std::uniform_int_distribution<SampleIdx> runLen(1, 3000);
    for (int r = 0; r < 6; ++r) { // invalid runs
        const SampleIdx s = start(rng);
        const SampleIdx e = std::min(n, s + runLen(rng));
        for (SampleIdx i = s; i < e; ++i) {
            flags[static_cast<std::size_t>(i)] = false;
        }
    }
    if (n > 3 * SampleIdx{store::kChunkSamples}) { // chunk 2 entirely invalid
        for (SampleIdx i = 2 * SampleIdx{store::kChunkSamples}; i < 3 * SampleIdx{store::kChunkSamples};
             ++i) {
            flags[static_cast<std::size_t>(i)] = false;
        }
    }
    std::vector<float> f32;
    std::vector<double> f64;
    std::vector<std::int32_t> i32;
    for (std::size_t i = 0; i < un; ++i) {
        switch (dtype) {
        case store::Dtype::F32: {
            float x = static_cast<float>(normal(rng));
            if (pct(rng) == 7) {
                x = pct(rng) < 50 ? 0.0F : -0.0F;
            }
            // Half the invalid samples are NaN with a set valid bit (the store clears it at seal).
            if (!flags[i] && pct(rng) < 50) {
                x = std::numeric_limits<float>::quiet_NaN();
                flags[i] = true;
            }
            f32.push_back(x);
            d.v[i] = static_cast<double>(x);
            d.valid[i] = flags[i] && !std::isnan(x);
            break;
        }
        case store::Dtype::F64: {
            double x = -33.4479 + normal(rng) * 1e-8; // GPS latitude scale: below float resolution
            if (!flags[i] && pct(rng) < 50) {
                x = std::numeric_limits<double>::quiet_NaN();
                flags[i] = true;
            }
            f64.push_back(x);
            d.v[i] = x;
            d.valid[i] = flags[i] && !std::isnan(x);
            break;
        }
        case store::Dtype::I32:
        case store::Dtype::I64: {
            const std::int32_t x = big(rng);
            i32.push_back(x);
            d.v[i] = static_cast<double>(x);
            d.valid[i] = flags[i];
            break;
        }
        }
    }
    switch (dtype) {
    case store::Dtype::F32:
        d.column = st.f32(f32, flags);
        break;
    case store::Dtype::F64:
        d.column = st.f64(f64, flags);
        break;
    case store::Dtype::I32:
    case store::Dtype::I64:
        d.column = st.i32(i32, flags);
        break;
    }
    if (timestamped) {
        std::uniform_int_distribution<Micros> step(1, 1999);
        Micros acc = 5000;
        std::vector<std::int64_t> ts(un);
        for (std::size_t i = 0; i < un; ++i) {
            acc += step(rng);
            d.t[i] = acc;
            ts[i] = acc;
        }
        d.times = st.i64(ts);
    } else {
        d.rate = rate;
        d.t0 = 12'345;
        for (std::size_t i = 0; i < un; ++i) {
            d.t[i] = rate.timeAt(static_cast<SampleIdx>(i), d.t0);
        }
    }
    return d;
}

TraceSpec specOf(const Data& d, units::Converter conv) {
    TraceSpec s;
    s.column = d.column;
    s.rate = d.rate;
    s.t0 = d.t0;
    s.timesUs = d.times;
    s.toDisplay = conv;
    return s;
}

// ---- the brute-force reference --------------------------------------------------------------------------

struct Reference {
    double origin = 0.0;
    std::vector<M4Column> cols;
};

Reference reference(const Data& d, const DomainMapper& map, XSpan visible, int w, units::Converter conv) {
    Reference ref;
    ref.cols.assign(static_cast<std::size_t>(w), M4Column{0.0F, 0.0F, 0.0F, 0.0F, M4Flag::Empty, 0.0F});
    if (map.domain() == Domain::Distance && !map.hasDistance()) {
        return ref;
    }
    std::vector<std::pair<Micros, Micros>> excludedSpans;
    if (map.domain() == Domain::Distance && map.chain()) {
        excludedSpans = map.chain()->unindexedSpans();
    }
    const auto excluded = [&](std::size_t i) {
        for (const auto& [s, e] : excludedSpans) {
            if (d.t[i] >= s && d.t[i] < e) {
                return true;
            }
        }
        return false;
    };
    std::vector<SampleIdx> bounds(static_cast<std::size_t>(w) + 1);
    Micros prev = kMicrosMin;
    for (int c = 0; c <= w; ++c) {
        const double x = visible.a + static_cast<double>(c) * visible.length() / static_cast<double>(w);
        Micros tc = map.timeAt(x);
        tc = std::max(tc, prev);
        prev = tc;
        bounds[static_cast<std::size_t>(c)] =
            static_cast<SampleIdx>(std::lower_bound(d.t.begin(), d.t.end(), tc) - d.t.begin());
    }
    bool haveOrigin = false;
    for (int c = 0; c < w; ++c) {
        const SampleIdx i0 = bounds[static_cast<std::size_t>(c)];
        const SampleIdx i1 = bounds[static_cast<std::size_t>(c) + 1];
        M4Column& col = ref.cols[static_cast<std::size_t>(c)];
        if (i1 <= i0) {
            continue;
        }
        SampleIdx included = 0;
        bool anyExcluded = false;
        for (SampleIdx i = i0; i < i1; ++i) {
            if (excluded(static_cast<std::size_t>(i))) {
                anyExcluded = true;
            } else {
                ++included;
            }
        }
        if (included == 0) {
            col.flags = M4Flag::Empty | M4Flag::HasInvalid;
            continue;
        }
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        double first = 0.0;
        double last = 0.0;
        bool any = false;
        bool hasInvalid = anyExcluded;
        bool breakBefore = false;
        bool invalidSinceLast = false;
        SampleIdx single = -1;
        for (SampleIdx i = i0; i < i1; ++i) {
            const auto u = static_cast<std::size_t>(i);
            if (excluded(u) || !d.valid[u]) {
                hasInvalid = true;
                invalidSinceLast = true;
                breakBefore = breakBefore || !any;
                if (!excluded(u)) {
                    single = i;
                }
                continue;
            }
            if (!any) {
                first = d.v[u];
                any = true;
            }
            single = i;
            last = d.v[u];
            invalidSinceLast = false;
            lo = store::foldMin(lo, d.v[u]);
            hi = store::foldMax(hi, d.v[u]);
        }
        std::uint8_t flags = hasInvalid ? M4Flag::HasInvalid : 0;
        if (included < 2) {
            if (!any) {
                col.flags = M4Flag::Empty | M4Flag::HasInvalid | M4Flag::RawRegime;
                continue;
            }
            flags |= M4Flag::RawRegime;
            // Excluded samples before / after the single included one break the line on that side.
            if (breakBefore) {
                flags |= M4Flag::BreakBefore;
            }
            if (invalidSinceLast) {
                flags |= M4Flag::BreakAfter;
            }
            const double px =
                DomainMapper::pxOf(map.xOf(d.t[static_cast<std::size_t>(single)]), visible, 0.0, w);
            const double frac = px - static_cast<double>(c);
            col.xFrac =
                !(frac > 0.0) ? 0.0F : (frac < 1.0 ? static_cast<float>(frac) : std::nextafter(1.0F, 0.0F));
        } else if (included < 32) {
            if (any && breakBefore) {
                flags |= M4Flag::BreakBefore;
            }
            if (any && invalidSinceLast) {
                flags |= M4Flag::BreakAfter;
            }
        } else if (any) {
            lo = static_cast<double>(store::roundFloatDown(lo)); // the pyramid's float entries
            hi = static_cast<double>(store::roundFloatUp(hi));
        }
        if (!any) {
            col.flags = M4Flag::Empty | M4Flag::HasInvalid;
            continue;
        }
        const double f = conv(first);
        const double l = conv(last);
        double dlo = conv(lo);
        double dhi = conv(hi);
        if (dlo > dhi) {
            std::swap(dlo, dhi);
        }
        if (!haveOrigin) {
            haveOrigin = true;
            ref.origin = f;
        }
        col.first = static_cast<float>(f - ref.origin);
        col.last = static_cast<float>(l - ref.origin);
        col.lo = static_cast<float>(dlo - ref.origin);
        col.hi = static_cast<float>(dhi - ref.origin);
        col.flags = flags;
    }
    return ref;
}

bool bitEqual(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

void checkAgainstReference(const Data& d, const DomainMapper& map, XSpan visible, int w,
                           units::Converter conv, const std::string& what) {
    std::vector<M4Column> out(static_cast<std::size_t>(w));
    const Result<double> origin =
        TraceBuilder::decimate(specOf(d, conv), map, visible, w, out, CancelToken{});
    REQUIRE(origin.ok());
    const Reference ref = reference(d, map, visible, w, conv);
    CHECK(*origin == ref.origin);
    int mismatches = 0;
    for (int c = 0; c < w && mismatches < 3; ++c) {
        const M4Column& a = out[static_cast<std::size_t>(c)];
        const M4Column& b = ref.cols[static_cast<std::size_t>(c)];
        const bool same = a.flags == b.flags && bitEqual(a.first, b.first) && bitEqual(a.last, b.last) &&
                          bitEqual(a.lo, b.lo) && bitEqual(a.hi, b.hi) && bitEqual(a.xFrac, b.xFrac);
        if (!same) {
            ++mismatches;
            UNSCOPED_INFO(what << " column " << c << " flags " << int(a.flags) << "/" << int(b.flags)
                               << " first " << a.first << "/" << b.first << " last " << a.last << "/"
                               << b.last << " lo " << a.lo << "/" << b.lo << " hi " << a.hi << "/" << b.hi
                               << " xFrac " << a.xFrac << "/" << b.xFrac);
        }
    }
    CHECK(mismatches == 0);
}

constexpr double kSpps[] = {0.1,  0.25, 0.6,  1.0,   1.5,    2.0,    3.7,    17.0,
                            31.0, 32.0, 33.0, 100.0, 1000.0, 9000.0, 70000.0};

} // namespace

TEST_CASE("M4: decimate equals the brute-force reference in time mode", "[property]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed());
    const SampleIdx n = 3 * SampleIdx{store::kChunkSamples} + 4321; // partial last chunk
    const units::Converter convs[] = {{1.0, 0.0}, {3.6, 0.0}, {-1.5, 10.0}};
    for (const store::Dtype dtype : {store::Dtype::F32, store::Dtype::F64, store::Dtype::I32}) {
        for (const bool timestamped : {false, true}) {
            const Rate rate = dtype == store::Dtype::F64 ? Rate{512, 25} : Rate{1000, 1};
            const Data d = makeData(st, dtype, n, timestamped, rate, rng);
            const Micros span0 = d.t.front();
            const Micros spanN = d.t.back();
            const double meanPeriod = static_cast<double>(spanN - span0) / static_cast<double>(n - 1);
            const RangeGeom range{span0 + 1'000'000, span0 + 61'000'000, 777, 0.0, 1.0};
            const DomainMapper map(Domain::Time, range, OutingGeom{span0, spanN + 1}, nullptr, d.rate, d.t0);
            std::uniform_int_distribution<int> widths(1, 257);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            std::uniform_real_distribution<double> jitter(-0.15, 0.15);
            int convIndex = 0;
            for (const double spp : kSpps) {
                for (int rep = 0; rep < 3; ++rep) {
                    const int w = widths(rng);
                    const double len = spp * w * meanPeriod;
                    // Mostly over the data, sometimes hanging over either end.
                    const double room = std::max(0.0, static_cast<double>(spanN - span0) - len);
                    const double a = static_cast<double>(span0 - range.start + range.offsetUs) +
                                     unit(rng) * room + jitter(rng) * len;
                    const units::Converter conv = convs[convIndex++ % 3];
                    checkAgainstReference(d, map, XSpan{a, a + len}, w, conv,
                                          std::string(store::dtypeName(dtype)) +
                                              (timestamped ? " ts" : " uniform") + " spp " +
                                              std::to_string(spp));
                }
            }
        }
    }
}

TEST_CASE("M4: decimate equals the reference at 10^6 samples per pixel", "[property]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed() + 3);
    const SampleIdx n = 2'100'000;
    const Data d = makeData(st, store::Dtype::F32, n, false, Rate{1000, 1}, rng);
    const DomainMapper map(Domain::Time, RangeGeom{d.t0, d.t0 + 1000}, OutingGeom{d.t0, d.t.back() + 1},
                           nullptr, d.rate, d.t0);
    const double period = 1000.0;
    checkAgainstReference(d, map, XSpan{0.0, 2.0 * 1e6 * period}, 2, units::Converter{}, "1e6 spp");
    checkAgainstReference(d, map, XSpan{-12345.0, 1e6 * period}, 1, units::Converter{}, "1e6 spp offset");
    checkAgainstReference(d, map, XSpan{777.0, 777.0 + 3.0 * 7e5 * period}, 3, units::Converter{3.6, 0.0},
                          "7e5 spp");
}

TEST_CASE("M4: decimate equals the reference in distance mode with plateaus and a lap without index",
          "[property]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed() + 4);
    const SampleIdx n = 2 * SampleIdx{store::kChunkSamples} + 999;
    for (const bool timestamped : {false, true}) {
        const Data d = makeData(st, store::Dtype::F32, n, timestamped, Rate{1000, 1}, rng);
        // Speed on a coarse grid: 20 m/s with stops (0 m/s) of ~5 s, so plateaus collapse many samples.
        std::vector<Micros> grid;
        std::vector<double> speed;
        for (std::size_t i = 0; i < d.t.size(); i += 97) {
            grid.push_back(d.t[i]);
            const Micros rel = d.t[i] - d.t.front();
            speed.push_back((rel / 1'000'000) % 23 < 5 ? 0.0
                                                       : 20.0 + static_cast<double>((rel / 1'000'000) % 7));
        }
        grid.push_back(d.t.back());
        speed.push_back(20.0);
        const Micros t0 = d.t.front();
        const Micros tn = d.t.back() + 1;
        const Micros third = (tn - t0) / 3;
        const std::vector<Micros> bounds{t0, t0 + third, t0 + 2 * third, tn};
        for (const bool partial : {false, true}) {
            const std::vector<bool> indexed =
                partial ? std::vector<bool>{true, false, true} : std::vector<bool>{};
            const auto chain = std::make_shared<const DistanceChain>(
                test::lapsFromSpeed(grid, speed, bounds, indexed, {1.0, 1.03, 0.97}), bounds[1]);
            const RangeGeom range{bounds[1], bounds[2], 0, partial ? 0.0 : 3.5, 1.0};
            const DomainMapper map(Domain::Distance, range, OutingGeom{t0, tn}, chain, d.rate, d.t0);
            const XSpan e = chain->extent();
            std::uniform_int_distribution<int> widths(1, 200);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            for (const double frac : {0.0005, 0.003, 0.02, 0.1, 0.5, 1.0}) {
                const int w = widths(rng);
                const double len = frac * e.length();
                const double a = e.a + unit(rng) * (e.length() - len) - 2.0;
                checkAgainstReference(d, map, XSpan{a, a + len}, w, units::Converter{},
                                      std::string("distance ") + (timestamped ? "ts" : "uniform") +
                                          (partial ? " partial" : "") + " frac " + std::to_string(frac));
            }
        }
    }
}

TEST_CASE("M4: a distance mapper without distance gives empty columns", "[unit]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed() + 5);
    const Data d = makeData(st, store::Dtype::F32, 1000, false, Rate{100, 1}, rng);
    const DomainMapper map(Domain::Distance, RangeGeom{0, 10'000'000}, OutingGeom{0, 10'000'000}, nullptr,
                           d.rate, d.t0);
    std::vector<M4Column> out(10);
    const Result<double> origin = TraceBuilder::decimate(specOf(d, {}), map, XSpan{0.0, 100.0}, 10, out, {});
    REQUIRE(origin.ok());
    for (const M4Column& c : out) {
        CHECK(c.flags == M4Flag::Empty);
    }
}

TEST_CASE("M4: the summary path encloses the exact columns within one 256-sample bucket", "[property]") {
    std::mt19937_64 rng(test::testSeed() + 6);
    const SampleIdx n = 3 * SampleIdx{store::kChunkSamples} + 1234;
    for (const bool resident : {false, true}) {
        const test::Store st;
        const Data d = makeData(st, store::Dtype::F32, n, false, Rate{1000, 1}, rng);
        REQUIRE(d.column.buildSummary(CancelToken{}).ok());
        if (resident) {
            auto pinned = d.column.require(0, n - 1, Priority::Visible, CancelToken{});
            REQUIRE(pinned.ok());
        }
        const DomainMapper map(Domain::Time, RangeGeom{d.t0, d.t0 + 1}, OutingGeom{d.t0, d.t.back() + 1},
                               nullptr, d.rate, d.t0);
        for (const int w : {1, 7, 40}) {
            const XSpan visible{-500.0, static_cast<double>(n) * 1000.0 + 500.0};
            std::vector<M4Column> out(static_cast<std::size_t>(w));
            const Result<double> origin =
                TraceBuilder::decimateSummary(specOf(d, {}), map, visible, w, out, CancelToken{});
            REQUIRE(origin.ok());
            // Exact per-column bounds and the bucket-widened bounds.
            for (int c = 0; c < w; ++c) {
                const double x0 = visible.a + static_cast<double>(c) * visible.length() / w;
                const double x1 = visible.a + static_cast<double>(c + 1) * visible.length() / w;
                const auto lb = [&](double x) {
                    return static_cast<SampleIdx>(std::lower_bound(d.t.begin(), d.t.end(), map.timeAt(x)) -
                                                  d.t.begin());
                };
                const SampleIdx i0 = lb(x0);
                const SampleIdx i1 = lb(x1);
                double lo = std::numeric_limits<double>::infinity();
                double hi = -lo;
                double wlo = lo;
                double whi = -lo;
                for (SampleIdx i = std::max<SampleIdx>(0, i0 / 256 * 256);
                     i < std::min(n, (i1 + 255) / 256 * 256); ++i) {
                    const auto u = static_cast<std::size_t>(i);
                    if (!d.valid[u]) {
                        continue;
                    }
                    wlo = std::min(wlo, d.v[u]);
                    whi = std::max(whi, d.v[u]);
                    if (i >= i0 && i < i1) {
                        lo = std::min(lo, d.v[u]);
                        hi = std::max(hi, d.v[u]);
                    }
                }
                const M4Column& col = out[static_cast<std::size_t>(c)];
                if (std::isinf(lo)) {
                    continue; // no valid sample: Empty or an approximation from the straddling buckets
                }
                REQUIRE((col.flags & M4Flag::Empty) == 0);
                const double clo = *origin + static_cast<double>(col.lo);
                const double chi = *origin + static_cast<double>(col.hi);
                const double tol = 1e-3; // float offsets from the origin
                CHECK(clo <= lo + tol);
                CHECK(chi >= hi - tol);
                CHECK(clo >= wlo - tol);
                CHECK(chi <= whi + tol);
                if (resident) {
                    CHECK((col.flags & M4Flag::Approx) == 0);
                }
            }
        }
    }
}

TEST_CASE("M4: decimateMany on a TaskPool equals sequential decimation", "[unit]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed() + 7);
    std::vector<Data> data;
    std::vector<TraceJob> jobs;
    for (int k = 0; k < 6; ++k) {
        data.push_back(makeData(st, k % 2 == 0 ? store::Dtype::F32 : store::Dtype::I32, 50'000 + 1000 * k,
                                k == 3, Rate{1000, 1}, rng));
    }
    for (const Data& d : data) {
        jobs.push_back(
            TraceJob{specOf(d, {}), DomainMapper(Domain::Time, RangeGeom{d.t.front(), d.t.front() + 1},
                                                 OutingGeom{}, nullptr, d.rate, d.t0)});
    }
    const XSpan visible{0.0, 40.0 * 1e6};
    const auto sequential = TraceBuilder::decimateMany(jobs, visible, 333, CancelToken{}, nullptr);
    TaskPool pool(4);
    const auto parallel = TraceBuilder::decimateMany(jobs, visible, 333, CancelToken{}, &pool);
    REQUIRE(sequential.size() == jobs.size());
    REQUIRE(parallel.size() == jobs.size());
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        REQUIRE(sequential[i].ok());
        REQUIRE(parallel[i].ok());
        CHECK(sequential[i]->origin == parallel[i]->origin);
        CHECK(sequential[i]->columns == parallel[i]->columns);
    }
}

TEST_CASE("M4: a cancelled token stops the decimation", "[unit]") {
    const test::Store st;
    std::mt19937_64 rng(test::testSeed() + 8);
    const Data d = makeData(st, store::Dtype::F32, 10'000, false, Rate{1000, 1}, rng);
    CancelSource source;
    source.cancel();
    const DomainMapper map(Domain::Time, RangeGeom{d.t0, d.t0 + 1}, OutingGeom{}, nullptr, d.rate, d.t0);
    std::vector<M4Column> out(100);
    const Result<double> r =
        TraceBuilder::decimate(specOf(d, {}), map, XSpan{0.0, 1e7}, 100, out, source.token());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
    std::vector<M4Column> small(5);
    CHECK_FALSE(TraceBuilder::decimate(specOf(d, {}), map, XSpan{0.0, 1e7}, 10, small, {}).ok());
}
