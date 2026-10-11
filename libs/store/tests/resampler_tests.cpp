// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Resampler and ResampleView (02 section 3.3 rules 13-16, 02 section 8 item 5): the four policies on uniform
// and timestamped series, boundaries against a brute-force reference, valueAt/block consistency, identity
// gathers, partial last chunks and all-invalid chunks.
#include "store_test_support.hpp"

#include <esses/base/taskpool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <random>

using namespace esses;
using namespace esses::store;

namespace {

constexpr std::size_t kGiB = std::size_t{1} << 30;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr Interp kPolicies[] = {Interp::Linear, Interp::Closest, Interp::Previous, Interp::Next};

bool sameValue(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) ||
           std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Brute-force bracketing by linear scans (the reference shares none of the binary searches): the last index
// with time <= t and the first with time >= t.
std::pair<SampleIdx, SampleIdx> referenceBracket(const std::vector<std::int64_t>& times, Micros t) {
    const auto n = static_cast<SampleIdx>(times.size());
    SampleIdx prev = -1;
    SampleIdx next = n;
    for (SampleIdx i = 0; i < n; ++i) {
        const std::int64_t ti = times[static_cast<std::size_t>(i)];
        if (ti <= t) {
            prev = i;
        }
        if (ti >= t && next == n) {
            next = i;
        }
    }
    return {prev, next};
}

// Brute-force policy reference over explicit times/values (values NaN = invalid).
double referenceAt(const std::vector<std::int64_t>& times, const std::vector<double>& values,
                   std::pair<SampleIdx, SampleIdx> bracket, Micros t, Interp interp) {
    if (times.empty() || t < times.front() || t > times.back()) {
        return kNaN;
    }
    const auto [prev, next] = bracket;
    const auto at = [&](SampleIdx i) { return values[static_cast<std::size_t>(i)]; };
    const auto tm = [&](SampleIdx i) { return times[static_cast<std::size_t>(i)]; };
    switch (interp) {
    case Interp::Previous:
        return at(prev);
    case Interp::Next:
        return at(next);
    case Interp::Closest:
        if (tm(next) == t) {
            return at(next);
        }
        return (t - tm(prev)) <= (tm(next) - t) ? at(prev) : at(next);
    case Interp::Linear: {
        if (tm(prev) == t) {
            return at(prev);
        }
        const double va = at(prev);
        const double vb = at(next);
        if (std::isnan(va) || std::isnan(vb)) {
            return kNaN;
        }
        return va +
               (vb - va) * (static_cast<double>(t - tm(prev)) / static_cast<double>(tm(next) - tm(prev)));
    }
    }
    return kNaN;
}

struct UniformFixture {
    ChunkCache cache{kGiB};
    UniformSeries series;
    ChunkHandle pin;

    UniformFixture(std::vector<float> v, std::vector<bool> valid, Rate rate, Micros t0) {
        series.rate = rate;
        series.t0 = t0;
        series.values =
            Column(InMemoryProvider::fromF32(std::move(v), std::move(valid)), ColumnOptions{&cache});
        Result<ChunkHandle> h = series.values.require(0, series.values.size() - 1, Priority::Visible, {});
        REQUIRE(h.ok());
        pin = std::move(*h);
    }
};

struct TimestampedFixture {
    ChunkCache cache{kGiB};
    TimestampedSeries series;
    ChunkHandle pinTimes;
    ChunkHandle pinValues;

    TimestampedFixture(std::vector<std::int64_t> times, std::vector<double> values) {
        Column t(InMemoryProvider::fromI64(std::move(times)), ColumnOptions{&cache});
        Column v(InMemoryProvider::fromF64(std::move(values)), ColumnOptions{&cache});
        Result<TimestampedSeries> s = makeTimestampedSeries(t, v);
        REQUIRE(s.ok());
        series = std::move(*s);
        Result<ChunkHandle> ht = series.timesUs.require(0, series.size() - 1, Priority::Visible, {});
        Result<ChunkHandle> hv = series.values.require(0, series.size() - 1, Priority::Visible, {});
        REQUIRE(ht.ok());
        REQUIRE(hv.ok());
        pinTimes = std::move(*ht);
        pinValues = std::move(*hv);
    }
};

// valueAt at every output time equals block.f64 bit for bit (rule 16 "monotone-consistent"). Returns the
// number of mismatches (a large number when pinning or the block fails); no assertion inside, so callers may
// wrap it in CHECK.
int compareBlockWithValueAt(const Series& series, Rate out, Micros t0, SampleIdx first, SampleIdx n,
                            Interp interp, CancelToken cancel = {}) {
    constexpr int kFailed = 1'000'000;
    Result<Resampler::PinnedInput> pins =
        Resampler::pinFor(series, out, t0, first, n, Priority::Visible, cancel);
    if (!pins.ok()) {
        return kFailed;
    }
    const ChunkHandle* times = std::holds_alternative<TimestampedSeries>(series) ? &pins->times : nullptr;
    Result<ResampleView> view = Resampler::block(series, out, t0, first, n, interp, pins->values, times);
    if (!view.ok()) {
        return kFailed;
    }
    int mismatches = 0;
    for (SampleIdx k = 0; k < n; ++k) {
        const Micros t = out.timeAt(first + k, t0);
        const double a = Resampler::valueAt(series, t, interp);
        const double b = view->f64(k);
        if (!sameValue(a, b) || view->valid(k) == std::isnan(b) || view->timeAt(k) != t) {
            ++mismatches;
        }
    }
    return mismatches;
}

} // namespace

TEST_CASE("resampler: policies on a uniform series", "[unit][resampler]") {
    // 10 Hz from t0 = 1 ms: samples at 1'000, 101'000, ..., 501'000 us; sample 3 is invalid.
    UniformFixture fx({0.0f, 10.0f, 20.0f, std::numeric_limits<float>::quiet_NaN(), 40.0f, 50.0f}, {},
                      Rate{10, 1}, 1'000);
    const Series s = fx.series;
    auto at = [&](Micros t, Interp i) { return Resampler::valueAt(s, t, i); };
    CHECK(at(51'000, Interp::Linear) == 5.0);
    CHECK(at(1'000, Interp::Linear) == 0.0);
    CHECK(at(101'000, Interp::Linear) == 10.0);
    CHECK(std::isnan(at(251'000, Interp::Linear))); // next sample invalid
    CHECK(std::isnan(at(351'000, Interp::Linear))); // previous sample invalid
    CHECK(at(451'000, Interp::Linear) == 45.0);
    CHECK(at(51'000, Interp::Closest) == 0.0); // a tie picks the earlier sample
    CHECK(at(51'001, Interp::Closest) == 10.0);
    CHECK(std::isnan(at(260'000, Interp::Closest))); // the nearer sample is invalid: no fallback
    CHECK(at(100'999, Interp::Previous) == 0.0);
    CHECK(at(101'000, Interp::Previous) == 10.0);
    CHECK(std::isnan(at(350'000, Interp::Previous)));
    CHECK(at(501'000, Interp::Previous) == 50.0);
    CHECK(at(100'999, Interp::Next) == 10.0);
    CHECK(at(101'000, Interp::Next) == 10.0);
    CHECK(std::isnan(at(250'000, Interp::Next)));
    CHECK(at(1'000, Interp::Next) == 0.0);
    for (const Interp i : kPolicies) {
        CHECK(std::isnan(at(999, i)));     // before the first sample
        CHECK(std::isnan(at(501'001, i))); // after the last
    }
    CHECK(Resampler::sampleAt(s, 51'000, Interp::Linear).state == SampleState::Valid);
    CHECK(Resampler::sampleAt(s, 251'000, Interp::Linear).state == SampleState::Invalid);
    CHECK(Resampler::sampleAt(s, 0, Interp::Linear).state == SampleState::Invalid);
    for (const Interp i : kPolicies) {
        CHECK(compareBlockWithValueAt(s, Rate{1'000, 1}, 0, -10, 600, i) == 0);
    }
    CHECK(fx.series.duration() == 600'000);
    CHECK(seriesSize(s) == 6);
    CHECK(seriesValues(s) == fx.series.values);
}

TEST_CASE("resampler: policies on a timestamped series", "[unit][resampler]") {
    TimestampedFixture fx({0, 10, 25, 26, 100}, {1.0, 2.0, 3.0, kNaN, 5.0});
    const Series s = fx.series;
    CHECK(fx.series.tMin == 0);
    CHECK(fx.series.tMax == 100);
    auto at = [&](Micros t, Interp i) { return Resampler::valueAt(s, t, i); };
    CHECK(at(5, Interp::Linear) == 1.5);
    CHECK(at(15, Interp::Linear) == 2.0 + 1.0 * (5.0 / 15.0));
    CHECK(at(25, Interp::Linear) == 3.0);
    CHECK(std::isnan(at(50, Interp::Linear)));
    CHECK(at(5, Interp::Closest) == 1.0);
    CHECK(at(6, Interp::Closest) == 2.0);
    CHECK(at(18, Interp::Closest) == 3.0);
    CHECK(std::isnan(at(60, Interp::Closest)));
    CHECK(std::isnan(at(99, Interp::Previous)));
    CHECK(at(100, Interp::Previous) == 5.0);
    CHECK(at(24, Interp::Previous) == 2.0);
    CHECK(std::isnan(at(26, Interp::Next)));
    CHECK(at(27, Interp::Next) == 5.0);
    CHECK(at(11, Interp::Next) == 3.0);
    for (const Interp i : kPolicies) {
        CHECK(std::isnan(at(-1, i)));
        CHECK(std::isnan(at(101, i)));
        CHECK(compareBlockWithValueAt(s, Rate{1'000'000, 1}, -3, 0, 110, i) == 0); // every microsecond
        CHECK(compareBlockWithValueAt(s, Rate{100'000, 3}, 7, -2, 12, i) == 0);
    }
    CHECK(seriesSize(s) == 5);
}

TEST_CASE("resampler: a uniform series resampled to itself is bit-identical", "[unit][resampler]") {
    std::mt19937_64 rng(test::testSeed());
    std::normal_distribution<float> nd(0.0f, 1e3f);
    const SampleIdx n = 2 * kChunkSamples + 4'321;
    std::vector<float> v(static_cast<std::size_t>(n));
    for (float& x : v) {
        x = nd(rng);
    }
    v[1'000] = std::numeric_limits<float>::quiet_NaN();
    v[70'000] = -0.0f;
    for (const Rate rate : {Rate{1'000, 1}, Rate{30, 1}, Rate{512, 25}}) {
        UniformFixture fx(v, {}, rate, 12'345);
        const Series s = fx.series;
        Result<ResampleView> view = Resampler::block(s, rate, 12'345, 0, n, Interp::Linear, fx.pin);
        REQUIRE(view.ok());
        CHECK(view->identity());
        CHECK(view->identityOffset() == 0);
        int diffs = 0;
        for (SampleIdx k = 0; k < n; ++k) {
            const float x = v[static_cast<std::size_t>(k)];
            if (std::isnan(x)) {
                diffs += view->valid(k) ? 1 : 0;
            } else if (std::bit_cast<std::uint32_t>(view->f32(k)) != std::bit_cast<std::uint32_t>(x)) {
                ++diffs;
            }
        }
        CHECK(diffs == 0);
        CHECK(std::isnan(view->f64(n))); // outside the block
    }
    // An integer period with t0 shifted by whole periods is still an identity gather, with an offset.
    UniformFixture fx(v, {}, Rate{1'000, 1}, 0);
    Result<ResampleView> shifted =
        Resampler::block(fx.series, Rate{2'000, 2}, 5'000, 0, 100, Interp::Previous, fx.pin);
    REQUIRE(shifted.ok());
    CHECK(shifted->identity());
    CHECK(shifted->identityOffset() == 5);
    CHECK(shifted->f32(0) == v[5]);
    // Half a period off is not an identity: Previous then picks the earlier sample.
    Result<ResampleView> half =
        Resampler::block(fx.series, Rate{1'000, 1}, 500, 0, 100, Interp::Previous, fx.pin);
    REQUIRE(half.ok());
    CHECK_FALSE(half->identity());
    CHECK(half->f32(3) == v[3]);
    CHECK(compareBlockWithValueAt(fx.series, Rate{1'000, 1}, 500, 0, 2'000, Interp::Linear) == 0);
}

TEST_CASE("resampler: valueAt and block agree at random rates and phases", "[property][resampler]") {
    std::mt19937_64 rng(test::testSeed() + 77);
    // Uniform input at 20.48 Hz with NaN and invalid runs over three chunks (the last partial).
    test::ColumnData<float> d =
        test::randomColumn<float>(rng, 2 * kChunkSamples + 999, 0.01, 0.0002, false, false);
    std::vector<bool> valid(d.valid.begin(), d.valid.end());
    UniformFixture uni(d.v, valid, Rate{512, 25}, -7'777);
    const Micros uniEnd = uni.series.timeAt(uni.series.size() - 1);
    // Timestamped input with random gaps over three chunks.
    std::vector<std::int64_t> times(2 * kChunkSamples + 17);
    std::vector<double> values(times.size());
    std::int64_t t = -50'000;
    for (std::size_t i = 0; i < times.size(); ++i) {
        t += 1 + static_cast<std::int64_t>(rng() % 3'000);
        times[i] = t;
        values[i] = (rng() % 50 == 0) ? kNaN : std::sin(static_cast<double>(i) * 0.01) * 100.0;
    }
    TimestampedFixture ts(times, values);
    const Micros tsEnd = times.back();
    int mismatches = 0;
    for (int trial = 0; trial < 12; ++trial) {
        const Rate rates[] = {Rate{100, 3}, Rate{1'000, 1}, Rate{15, 2}, Rate{512, 25},
                              Rate{1'000'000, 33'333}};
        const Rate out = rates[rng() % 5];
        const auto t0 = static_cast<Micros>(rng() % 1'000'000) - 500'000;
        for (const Interp i : kPolicies) {
            // Uniform: a block somewhere in the series, sometimes overlapping either end.
            const Micros startU =
                static_cast<Micros>(rng() % static_cast<std::uint64_t>(uniEnd + 100'000)) - 50'000;
            const SampleIdx firstU = out.indexAt(startU, t0);
            mismatches += compareBlockWithValueAt(uni.series, out, t0, firstU, 400, i);
            const Micros startT =
                static_cast<Micros>(rng() % static_cast<std::uint64_t>(tsEnd + 200'000)) - 150'000;
            const SampleIdx firstT = out.indexAt(startT, t0);
            mismatches += compareBlockWithValueAt(ts.series, out, t0, firstT, 400, i);
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("resampler: policy boundaries match a brute-force reference on random timestamped data",
          "[property][resampler]") {
    std::mt19937_64 rng(test::testSeed() + 4242);
    for (int round = 0; round < 3; ++round) {
        const std::size_t n = round == 0 ? 50 : (round == 1 ? 2'000 : kChunkSamples + 3'000);
        std::vector<std::int64_t> times(n);
        std::vector<double> values(n);
        std::int64_t t = static_cast<std::int64_t>(rng() % 1'000) - 500;
        for (std::size_t i = 0; i < n; ++i) {
            t += 1 + static_cast<std::int64_t>(rng() % 40); // dense: many exact hits and ties
            times[i] = t;
            values[i] = (rng() % 7 == 0)
                            ? kNaN
                            : static_cast<double>(static_cast<std::int64_t>(rng() % 2'001) - 1'000);
        }
        TimestampedFixture fx(times, values);
        const Series s = fx.series;
        int mismatches = 0;
        const int queries = round == 2 ? 300 : 2'000;
        for (int q = 0; q < queries; ++q) {
            Micros at = 0;
            switch (rng() % 4) {
            case 0:
                at = times[rng() % n]; // exact hit
                break;
            case 1:
                at = times[rng() % n] + ((rng() % 2) == 0 ? 1 : -1);
                break;
            case 2:
                at = times.front() - 1 - static_cast<Micros>(rng() % 10); // before / after the series
                if (rng() % 2 == 0) {
                    at = times.back() + 1 + static_cast<Micros>(rng() % 10);
                }
                break;
            default:
                at =
                    times.front() +
                    static_cast<Micros>(rng() % static_cast<std::uint64_t>(times.back() - times.front() + 1));
                break;
            }
            const std::pair<SampleIdx, SampleIdx> bracket = referenceBracket(times, at);
            for (const Interp i : kPolicies) {
                if (!sameValue(Resampler::valueAt(s, at, i), referenceAt(times, values, bracket, at, i))) {
                    ++mismatches;
                }
            }
        }
        INFO("round " << round);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("resampler: partial last chunk and all-invalid chunks", "[unit][resampler]") {
    ChunkCache cache(kGiB);
    test::ColumnData<float> d;
    const SampleIdx n = 2 * kChunkSamples + 500;
    d.v.resize(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < d.v.size(); ++i) {
        d.v[i] = static_cast<float>(i);
    }
    d.allInvalid.insert(1); // the middle chunk could not be decoded
    UniformSeries u;
    u.rate = Rate{1'000, 1};
    u.t0 = 0;
    u.values = Column(std::make_shared<test::TestProvider<float>>(d), ColumnOptions{&cache});
    Result<ChunkHandle> pin = u.values.require(0, n - 1, Priority::Visible, {});
    REQUIRE(pin.ok());
    const Series s = u;
    const Micros last0 = u.timeAt(kChunkSamples - 1);
    const Micros first1 = u.timeAt(kChunkSamples);
    CHECK(Resampler::valueAt(s, last0, Interp::Previous) == static_cast<double>(kChunkSamples - 1));
    CHECK(std::isnan(Resampler::valueAt(s, first1, Interp::Previous)));
    CHECK(std::isnan(Resampler::valueAt(s, last0 + 500, Interp::Linear))); // bracketing sample invalid
    CHECK(Resampler::valueAt(s, last0 + 400, Interp::Closest) == static_cast<double>(kChunkSamples - 1));
    CHECK(std::isnan(Resampler::valueAt(s, last0 + 600, Interp::Closest)));
    CHECK(Resampler::valueAt(s, u.timeAt(2 * kChunkSamples), Interp::Next) ==
          static_cast<double>(2 * kChunkSamples));
    CHECK(Resampler::valueAt(s, u.timeAt(n - 1), Interp::Previous) == static_cast<double>(n - 1));
    CHECK(std::isnan(Resampler::valueAt(s, u.timeAt(n - 1) + 1, Interp::Previous)));
    for (const Interp i : kPolicies) {
        CHECK(compareBlockWithValueAt(s, Rate{300, 1}, 0, 0, 700, i) == 0);
        CHECK(compareBlockWithValueAt(s, Rate{1'000, 1}, 0, kChunkSamples - 50, 200, i) == 0);
        CHECK(compareBlockWithValueAt(s, Rate{1'000, 1}, 0, n - 100, 200, i) == 0);
    }
}

TEST_CASE("resampler: absent chunks read as Absent and are scheduled", "[unit][resampler]") {
    ChunkCache cache(kGiB);
    TaskPool pool(2);
    std::vector<float> v(1'000, 2.0f);
    UniformSeries u{Rate{100, 1}, 0, Column(InMemoryProvider::fromF32(v), ColumnOptions{&cache, &pool})};
    const Series s = u;
    const ValueSample first = Resampler::sampleAt(s, 15'000, Interp::Linear);
    CHECK(first.state == SampleState::Absent);
    CHECK(std::isnan(first.value));
    REQUIRE(test::waitFor([&] { return u.values.isResident(0); }));
    const ValueSample later = Resampler::sampleAt(s, 15'000, Interp::Linear);
    CHECK(later.state == SampleState::Valid);
    CHECK(later.value == 2.0);
    REQUIRE(test::waitFor([&] { return pool.stats().running == 0; }));
    pool.join();
}

TEST_CASE("resampler: block validates its inputs", "[unit][resampler]") {
    std::vector<float> v(3 * kChunkSamples, 1.0f);
    UniformFixture fx(v, {}, Rate{1'000, 1}, 0);
    const Series s = fx.series;
    Result<ChunkHandle> only0 = fx.series.values.require(0, 0, Priority::Visible, {});
    REQUIRE(only0.ok());
    CHECK(
        Resampler::block(s, Rate{1'000, 1}, 0, kChunkSamples - 10, 20, Interp::Linear, *only0).error().code ==
        ErrorCode::Invalid);
    CHECK(Resampler::block(s, Rate{1'000, 1}, 0, 0, 20, Interp::Linear, *only0).ok());
    CHECK(Resampler::block(s, Rate{0, 1}, 0, 0, 20, Interp::Linear, *only0).error().code ==
          ErrorCode::Invalid);
    CHECK(Resampler::block(s, Rate{1'000, 1}, 0, 0, -1, Interp::Linear, *only0).error().code ==
          ErrorCode::Invalid);
    CHECK(Resampler::block(s, Rate{1'000, 1}, 0, 0, 0, Interp::Linear, *only0).ok());
    // Entirely outside the series: no input is needed, every output is invalid.
    ChunkHandle none;
    Result<ResampleView> outside = Resampler::block(s, Rate{10, 1}, -10'000'000, 0, 5, Interp::Next, none);
    REQUIRE(outside.ok());
    CHECK_FALSE(outside->valid(0));
    Result<Resampler::PinnedInput> nothing =
        Resampler::pinFor(s, Rate{10, 1}, -10'000'000, 0, 5, Priority::Visible, {});
    REQUIRE(nothing.ok());
    CHECK(nothing->values.empty());

    TimestampedFixture ts({0, 5, 9}, {1.0, 2.0, 3.0});
    CHECK(Resampler::block(ts.series, Rate{1'000'000, 1}, 0, 0, 10, Interp::Linear, ts.pinValues)
              .error()
              .code == ErrorCode::Invalid); // no time pins
    ChunkCache cache(kGiB);
    CHECK(makeTimestampedSeries(Column(InMemoryProvider::fromF32({1.0f}), ColumnOptions{&cache}),
                                Column(InMemoryProvider::fromF32({1.0f}), ColumnOptions{&cache}))
              .error()
              .code == ErrorCode::Invalid);
    CHECK(makeTimestampedSeries(Column(InMemoryProvider::fromI64({1, 2}), ColumnOptions{&cache}),
                                Column(InMemoryProvider::fromF32({1.0f}), ColumnOptions{&cache}))
              .error()
              .code == ErrorCode::Invalid);
}

TEST_CASE("resampler: default policy per sample kind", "[unit][resampler]") {
    CHECK(Resampler::defaultFor(SampleKind::Real) == Interp::Linear);
    for (const SampleKind k :
         {SampleKind::Integer, SampleKind::Enum, SampleKind::Bool, SampleKind::Bitfield, SampleKind::Text}) {
        CHECK(Resampler::defaultFor(k) == Interp::Previous);
    }
    CHECK(interpName(Interp::Closest) == "Closest");
    CHECK(sampleKindName(SampleKind::Bitfield) == "Bitfield");
    CHECK(dtypeName(Dtype::I64) == "I64");
    CHECK(validityName(Validity::Bitmap) == "Bitmap");
}
