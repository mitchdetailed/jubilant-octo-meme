// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Property tests of the kernels (03 section 14.2 items 3, 4, 5 and 10; seed ESSES_TEST_SEED): the sliding
// windows of smooth and derivative equal brute-force per-sample loops on random inputs with invalid samples
// and random window sizes (both parities, seconds, wider than the extent); window identities (smooth of a
// constant, derivative of a line, integrate of a constant); the RC low pass against the scalar recursion; the
// zero-phase filter of a reversed signal is the reversed output; Chebyshev and Butterworth low passes keep a
// constant (unit DC gain, steady-state priming) and high passes remove it; repeat evaluations are
// bit-identical; the validity flags equal the NaN positions; the Table kernel against a linear search.

#include "kernel_test_support.hpp"
#include "kernels/kernels_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

std::vector<double> randomSignal(std::mt19937_64& rng, std::size_t n, double invalidShare) {
    std::uniform_real_distribution<double> value(-50.0, 50.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<double> v(n);
    double walk = value(rng);
    for (double& x : v) {
        walk += value(rng) * 0.1;
        x = unit(rng) < invalidShare ? kNaN : walk;
    }
    return v;
}

// Brute force: the window of n points around i (odd centred, even biased earlier), truncated at the edges.
std::pair<std::size_t, std::size_t> window(std::size_t i, std::size_t n, std::size_t size) {
    const std::size_t before = n % 2 == 1 ? (n - 1) / 2 : n / 2;
    const std::size_t after = n % 2 == 1 ? (n - 1) / 2 : n / 2 - 1;
    return {i >= before ? i - before : 0, std::min(size - 1, i + after)};
}

std::vector<double> bruteSmooth(const std::vector<double>& x, std::size_t n) {
    std::vector<double> out(x.size(), kNaN);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto [a, b] = window(i, n, x.size());
        double sum = 0.0;
        std::size_t c = 0;
        for (std::size_t j = a; j <= b; ++j) {
            if (!std::isnan(x[j])) {
                sum += x[j];
                ++c;
            }
        }
        out[i] = c > 0 ? sum / static_cast<double>(c) : kNaN;
    }
    return out;
}

std::vector<double> bruteSlope(const std::vector<double>& x, std::size_t n, double hz) {
    std::vector<double> out(x.size(), kNaN);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto [a, b] = window(i, n, x.size());
        double c = 0.0;
        double mu = 0.0;
        double mv = 0.0;
        for (std::size_t j = a; j <= b; ++j) {
            if (!std::isnan(x[j])) {
                c += 1.0;
                mu += static_cast<double>(j) / hz;
                mv += x[j];
            }
        }
        if (c < 2.0) {
            continue;
        }
        mu /= c;
        mv /= c;
        double suu = 0.0;
        double suv = 0.0;
        for (std::size_t j = a; j <= b; ++j) {
            if (!std::isnan(x[j])) {
                const double du = static_cast<double>(j) / hz - mu;
                suu += du * du;
                suv += du * (x[j] - mv);
            }
        }
        out[i] = suu > 0.0 ? suv / suu : kNaN;
    }
    return out;
}

void requireClose(const std::vector<double>& got, const std::vector<double>& want, double tol,
                  const std::string& what) {
    INFO(what);
    REQUIRE(got.size() == want.size());
    std::size_t bad = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!close(got[i], want[i], tol)) {
            if (bad++ < 5) {
                UNSCOPED_INFO("sample " << i << ": got " << got[i] << " want " << want[i]);
            }
        }
    }
    CHECK(bad == 0);
}

} // namespace

TEST_CASE("kernel properties: smooth and derivative equal brute-force windows", "[property]") {
    std::mt19937_64 rng(testSeed());
    std::uniform_int_distribution<std::size_t> sizes(1, 6000);
    std::uniform_int_distribution<std::size_t> points(0, 70);
    for (int round = 0; round < 12; ++round) {
        const std::size_t size = round == 0 ? 9000 : sizes(rng); // one extent beyond two re-summation blocks
        const std::vector<double> x = randomSignal(rng, size, round % 3 == 0 ? 0.0 : 0.15);
        KernelFixture f;
        f.add("X", x, Rate::fromHz(50));
        const std::size_t n = round == 1 ? size + 7 : points(rng); // once wider than the extent
        const std::size_t eff = n < 2 ? 2 : n;
        const std::string what = "size " + std::to_string(size) + ", n " + std::to_string(n);
        requireClose(f.values("smooth('X', " + std::to_string(n) + ")"), bruteSmooth(x, eff), 1e-9,
                     "smooth " + what);
        requireClose(f.values("derivative('X', " + std::to_string(n) + ")"), bruteSlope(x, eff, 50.0), 1e-7,
                     "derivative " + what);
    }
    // Seconds: 0.3 s at 50 Hz is 2 * floor(7.5) + 1 = 15 points.
    const std::vector<double> x = randomSignal(rng, 500, 0.1);
    KernelFixture f;
    f.add("X", x, Rate::fromHz(50));
    requireClose(f.values("smooth('X', 0.3)"), bruteSmooth(x, 15), 1e-9, "smooth 0.3 s");
    requireClose(f.values("derivative('X', 0.3)"), bruteSlope(x, 15, 50.0), 1e-7, "derivative 0.3 s");
}

TEST_CASE("kernel properties: window and integral identities", "[property]") {
    std::mt19937_64 rng(testSeed() ^ 0x1D);
    std::uniform_real_distribution<double> coeff(-20.0, 20.0);
    for (int round = 0; round < 5; ++round) {
        const double a = coeff(rng);
        const double b = coeff(rng);
        std::vector<double> line(400);
        std::vector<double> constant(400, b);
        for (std::size_t i = 0; i < line.size(); ++i) {
            line[i] = a * static_cast<double>(i) / 100.0 + b; // a per second at 100 Hz
        }
        KernelFixture f;
        f.add("L", line, Rate::fromHz(100));
        f.add("C", constant, Rate::fromHz(100));
        for (const double v : f.values("smooth('C', 9)")) {
            CHECK(close(v, b, 1e-12));
        }
        const std::vector<double> d = f.values("derivative('L', 7)");
        for (std::size_t i = 1; i < d.size(); ++i) { // the first sample has one point only
            CHECK(close(d[i], a, 1e-7));
        }
        const std::vector<double> d2 = f.values("derivative('L', 31)");
        for (std::size_t i = 1; i < d2.size(); ++i) {
            CHECK(close(d2[i], a, 1e-7));
        }
        const std::vector<double> in = f.values("integrate('C')");
        for (std::size_t i = 0; i < in.size(); ++i) {
            CHECK(close(in[i], b * static_cast<double>(i + 1) / 100.0, 1e-9));
        }
    }
}

TEST_CASE("kernel properties: RC filters against the scalar recursion; zero phase is symmetric in time",
          "[property]") {
    std::mt19937_64 rng(testSeed() ^ 0x5C);
    const std::vector<double> x = randomSignal(rng, 3000, 0.0);
    KernelFixture f;
    f.add("X", x, Rate::fromHz(200));
    const double fc = 7.5;
    const double rc = 1.0 / (2.0 * std::numbers::pi * fc);
    const double dt = 1.0 / 200.0;
    // Forward order 1: y[i] = y[i-1] + a (x[i] - y[i-1]) from y[-1] = x[0].
    {
        const double a = dt / (rc + dt);
        std::vector<double> want(x.size());
        double y = x[0];
        for (std::size_t i = 0; i < x.size(); ++i) {
            y += a * (x[i] - y);
            want[i] = y;
        }
        requireClose(f.values("filter_lp('X', 7.5)"), want, 1e-12, "low pass recursion");
    }
    // High pass: y[i] = b (y[i-1] + x[i] - x[i-1]) from y[-1] = 0, x[-1] = x[0].
    {
        const double b = rc / (rc + dt);
        std::vector<double> want(x.size());
        double y = 0.0;
        double prev = x[0];
        for (std::size_t i = 0; i < x.size(); ++i) {
            y = b * (y + x[i] - prev);
            prev = x[i];
            want[i] = y;
        }
        requireClose(f.values("filter_hp('X', 7.5)"), want, 1e-12, "high pass recursion");
    }
    // Zero phase (03 section 14.2 item 5): between long constant ends (where both primings are exact), the
    // filter of the reversed signal is the reversed output.
    std::vector<double> padded(2000, x.front());
    padded.insert(padded.end(), x.begin(), x.end());
    padded.insert(padded.end(), 2000, x.front());
    std::vector<double> reversed(padded.rbegin(), padded.rend());
    KernelFixture g;
    g.add("P", padded, Rate::fromHz(200));
    g.add("R", reversed, Rate::fromHz(200));
    for (const std::string& fn :
         {std::string("filter_lp('%', 7.5, -2)"), std::string("filter_cheby_lp('%', 7.5, -8, 0.5)"),
          std::string("filter_cheby_bp('%', 3, 9, -4)")}) {
        std::string p = fn;
        std::string r = fn;
        p.replace(p.find('%'), 1, "P");
        r.replace(r.find('%'), 1, "R");
        const std::vector<double> fwd = g.values(p);
        std::vector<double> back = g.values(r);
        std::reverse(back.begin(), back.end());
        requireClose(fwd, back, 1e-9, fn);
    }
}

TEST_CASE("kernel properties: low passes keep a constant, high passes remove it", "[property]") {
    std::vector<double> c(500, 3.25);
    KernelFixture f;
    f.add("C", c, Rate::fromHz(100));
    for (const char* expr : {"filter_lp('C', 2, 3)", "filter_lp('C', 2, -2)", "filter_cheby_lp('C', 5, 6, 1)",
                             "filter_cheby_lp('C', 5, -12)", "filter_cheby_bs('C', 5, 9, 4, 0.5)"}) {
        INFO(expr);
        for (const double v : f.values(expr)) {
            CHECK(close(v, 3.25, 1e-10));
        }
    }
    for (const char* expr :
         {"filter_hp('C', 2)", "filter_cheby_hp('C', 5, 4)", "filter_cheby_bp('C', 3, 9)"}) {
        INFO(expr);
        for (const double v : f.values(expr)) {
            CHECK(std::abs(v) < 1e-9);
        }
    }
}

TEST_CASE("kernel properties: repeat evaluations are bit-identical and validity equals NaN", "[property]") {
    std::mt19937_64 rng(testSeed() ^ 0xB17);
    const std::vector<double> x = randomSignal(rng, 7000, 0.2);
    KernelFixture f;
    f.add("X", x, Rate::fromHz(100));
    for (const char* expr : {"smooth('X', 11) + derivative('X', 2.5)", "filter_cheby_lp('X', 3, -8, 0.5)",
                             "stat_std_dev('X') + running_max('X', 'X' > 0)", "time_shift('X', 0.07)",
                             "integrate('X', 'X' > 0)"}) {
        INFO(expr);
        KernelFixture::Outcome a = f.eval(expr);
        KernelFixture::Outcome b = f.eval(expr);
        REQUIRE(a.signal.ok());
        REQUIRE(b.signal.ok());
        REQUIRE(a.signal->values.size() == b.signal->values.size());
        CHECK(std::memcmp(a.signal->values.data(), b.signal->values.data(),
                          a.signal->values.size() * sizeof(double)) == 0);
        CHECK(a.signal->valid == b.signal->valid);
        for (std::size_t i = 0; i < a.signal->values.size(); ++i) {
            if (std::isnan(a.signal->values[i]) == a.signal->validAt(i)) {
                FAIL("validity differs from the NaN mark at sample " << i);
            }
        }
    }
}

TEST_CASE("kernel properties: table lookups against a linear search", "[property]") {
    std::mt19937_64 rng(testSeed() ^ 0x7AB);
    std::uniform_real_distribution<double> step(0.1, 5.0);
    std::uniform_real_distribution<double> val(-100.0, 100.0);
    for (int round = 0; round < 20; ++round) {
        const bool uniform = round % 2 == 0;
        std::vector<double> bp{val(rng)};
        const double fixed = step(rng);
        for (int k = 1; k < 2 + round; ++k) {
            bp.push_back(bp.back() + (uniform ? fixed : step(rng)));
        }
        std::vector<double> values(bp.size());
        for (double& v : values) {
            v = val(rng);
        }
        std::uniform_real_distribution<double> xs(bp.front() - 3.0, bp.back() + 3.0);
        std::vector<double> x(300);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] = i % 17 == 0 ? bp[i % bp.size()] : xs(rng); // exact breakpoints too
        }
        for (const TableSpec::Interp interp :
             {TableSpec::Interp::Linear, TableSpec::Interp::Next, TableSpec::Interp::Previous}) {
            for (const TableSpec::OutOfRange oor :
                 {TableSpec::OutOfRange::Clamp, TableSpec::OutOfRange::Invalid}) {
                TableSpec t;
                t.x.breakpoints = bp;
                t.values = values;
                t.interpolation = interp;
                t.outOfRange = oor;
                Result<std::unique_ptr<Kernel>> k = kern::makeTableKernel(t, *shippedUnits());
                REQUIRE(k.ok());
                std::vector<double> in = x;
                std::vector<double> out(x.size());
                BlockView inView{in.data(), nullptr, store::Dtype::F64,
                                 static_cast<std::uint32_t>(in.size())};
                BlockView outView{out.data(), nullptr, store::Dtype::F64,
                                  static_cast<std::uint32_t>(out.size())};
                BlockCtx ctx;
                ctx.n = outView.n;
                (*k)->run(ctx, std::span<const BlockView>(&inView, 1), outView);
                for (std::size_t i = 0; i < x.size(); ++i) {
                    double want = kNaN;
                    const double v = x[i];
                    if (v < bp.front() || v > bp.back()) {
                        want = oor == TableSpec::OutOfRange::Invalid
                                   ? kNaN
                                   : (v < bp.front() ? values.front() : values.back());
                    } else if (interp == TableSpec::Interp::Next) {
                        std::size_t k2 = 0;
                        while (bp[k2] < v) {
                            ++k2;
                        }
                        want = values[k2];
                    } else if (interp == TableSpec::Interp::Previous) {
                        std::size_t k2 = bp.size() - 1;
                        while (bp[k2] > v) {
                            --k2;
                        }
                        want = values[k2];
                    } else {
                        std::size_t k2 = 0;
                        while (k2 + 2 < bp.size() && bp[k2 + 1] <= v) {
                            ++k2;
                        }
                        const double w = (v - bp[k2]) / (bp[k2 + 1] - bp[k2]);
                        want = values[k2] + w * (values[k2 + 1] - values[k2]);
                    }
                    INFO("round " << round << " sample " << i << " x " << v);
                    CHECK(close(out[i], want, 1e-9));
                }
            }
        }
    }
}

TEST_CASE("kernel properties: range functions equal a per-sample search at rates with uneven periods",
          "[property]") {
    std::mt19937_64 rng(testSeed() ^ 0x4A9);
    for (const Rate rate : {Rate{3, 1}, Rate{1000, 3}, Rate{512, 25}, Rate::fromHz(10)}) {
        std::uniform_int_distribution<Micros> gap(0, 900'000);
        std::uniform_int_distribution<Micros> length(1, 700'000);
        std::vector<std::pair<Micros, Micros>> ranges;
        Micros t = gap(rng) / 3;
        for (int k = 0; k < 12; ++k) {
            const Micros a = t;
            const Micros b = a + length(rng);
            ranges.emplace_back(a, b);
            t = b + (k % 3 == 0 ? 0 : gap(rng)); // some contiguous, some apart
        }
        KernelFixture f;
        const auto count = static_cast<std::size_t>(rate.indexAt(t + 500'000, 0) + 1);
        f.add("X", std::vector<double>(count, 1.0), rate);
        model::RangeGroup root = model::RangeGroup::make("Outings");
        f.ranges().setGroup(root);
        model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
        for (std::size_t k = 0; k < ranges.size(); ++k) {
            model::Range r;
            r.id = model::rangeId("Outings:Laps", model::MarkerId{}, std::to_string(k));
            r.kind = model::RangeKind::Lap;
            r.start = ranges[k].first;
            r.end = ranges[k].second;
            laps.ranges.push_back(r);
        }
        f.ranges().setGroup(laps);
        const std::vector<double> is = f.values("range_is(\"Outings:Laps\") + 'X' * 0");
        const std::vector<double> change = f.values("range_change(\"Outings:Laps\") + 'X' * 0");
        REQUIRE(is.size() == count);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Micros ti = rate.timeAt(static_cast<SampleIdx>(i), 0);
            double inside = 0.0;
            double starts = 0.0;
            for (const auto& [a, b] : ranges) {
                inside = (ti >= a && ti < b) ? 1.0 : inside;
                starts += a <= ti ? 1.0 : 0.0;
            }
            bad += (is[i] != inside || change[i] != starts) ? 1 : 0;
        }
        INFO("rate " << rate.num << "/" << rate.den);
        CHECK(bad == 0);
    }
}
