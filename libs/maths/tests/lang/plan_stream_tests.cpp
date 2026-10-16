// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlanStream (src/lang/plan_stream.hpp, D-191): the streamed evaluation gives the whole-extent values bit
// for bit. The oracle is the evaluation as 0.1 first ran it: every input gathered over the whole extent into
// an F64 buffer with a validity bitmap, every stage run once over the whole extent through Kernel::run with
// bitmaps (so each kernel takes its single-step path, and with any invalid sample in its input its generic
// per-sample code), the last stage rounded as evaluateWhole rounds it. The stream is pulled in random pieces
// (seed ESSES_TEST_SEED), so the kernels' state crosses arbitrary step boundaries and their fast loops (a
// step whose input has no invalid sample) meet the generic code on the same samples. restartAt() is checked
// against the continuous stream at chunk boundaries and arbitrary samples. Expectations come from the oracle
// and the continuous stream, never from stored values.

#include "../kernels/kernel_test_support.hpp"
#include "chunk_values.hpp"
#include "lang/ew_kernel.hpp"
#include "lang/plan_stream.hpp"

#include <esses/base/cancel.hpp>
#include <esses/model/range.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

constexpr std::size_t kSamples1k = 70'000; // 70 s at 1 kHz: two chunks, a re-prime gap, several laps

// A random walk with isolated invalid samples, short invalid runs and (when `gap`) a 1.5 s invalid run.
std::vector<double> walk(std::mt19937_64& rng, std::size_t n, double level, bool invalid, bool gap) {
    std::normal_distribution<double> step(0.0, 0.7);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<double> v(n);
    double x = level;
    for (std::size_t i = 0; i < n; ++i) {
        x += step(rng) + 0.002 * (level - x);
        v[i] = x;
        if (invalid && unit(rng) < 0.0005) {
            const std::size_t len = 1 + static_cast<std::size_t>(unit(rng) * 30.0);
            for (std::size_t k = i; k < std::min(n, i + len); ++k) {
                v[k] = kNaN;
            }
        }
    }
    if (gap) {
        for (std::size_t k = n / 2; k < std::min(n, n / 2 + 1500); ++k) {
            v[k] = kNaN;
        }
    }
    return v;
}

void addRanges(KernelFixture& f) {
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    model::Range outing;
    outing.id = model::rangeId("Outings", model::MarkerId{}, "outing");
    outing.kind = model::RangeKind::Outing;
    outing.start = 0;
    outing.end = 70'000'000;
    outing.number = 1;
    outings.ranges.push_back(outing);
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    int number = 1;
    for (Micros t = 2'000'000; t < 70'000'000; t += 15'000'000, ++number) {
        model::Range r;
        r.id = model::rangeId("Outings:Laps", model::MarkerId{}, std::to_string(number));
        r.kind = model::RangeKind::Lap;
        r.lapClass = model::LapClass::Flying;
        r.start = t;
        r.end = std::min<Micros>(t + 14'500'000, 70'000'000);
        r.number = number;
        laps.ranges.push_back(r);
    }
    f.ranges().setGroup(std::move(outings));
    f.ranges().setGroup(std::move(laps));
}

// A at 1 kHz with invalid samples and a long gap, B at 1 kHz without, C at 250 Hz with invalid samples
// (resampled where it meets A), G at 100 Hz whole numbers read Previous.
void addChannels(KernelFixture& f, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    f.add("A", walk(rng, kSamples1k, 20.0, true, true), Rate::fromHz(1000));
    f.add("B", walk(rng, kSamples1k, 5.0, false, false), Rate::fromHz(1000));
    f.add("C", walk(rng, kSamples1k / 4, 1.0, true, false), Rate::fromHz(250));
    std::vector<double> g = walk(rng, kSamples1k / 10, 3.0, true, false);
    for (double& x : g) {
        x = std::isnan(x) ? x : std::round(std::clamp(x, 0.0, 6.0));
    }
    f.add("G", g, Rate::fromHz(100), "", store::Interp::Previous);
    addRanges(f);
}

// Every kernel class and parameter form the streamed evaluator handles, mixed.
const std::vector<std::string>& expressions() {
    static const std::vector<std::string> list = {
        "('A' + 'C') / 2",
        "choose('G' > 2, 'A', 'B' * 2)",
        "hypot('A', 'C') - 'G'",
        "'G' * 2 + 1",
        "max('A' + 'C', 1) - rand_chan()",
        "smooth('A', 5)",
        "smooth('A', 4)",
        "smooth('A', 0.25)",
        "smooth('B', 5001)",
        "smooth('C', 3)",
        "derivative('A')",
        "derivative('B')",
        "derivative('A', 11)",
        "derivative('A', 6)",
        "derivative('A', 'B')",
        "derivative('A', 'B', 21)",
        "derivative(smooth('A', 5))",
        "filter_lp('A', 10, 1)",
        "filter_lp('A', 10, 2)",
        "filter_lp('A', 5, 3)",
        "filter_lp('A', 5, -1)",
        "filter_lp('A', 5, -2)",
        "filter_hp('B', 1, 2)",
        "filter_cheby_lp('A', 20, 4, 0.5)",
        "filter_cheby_lp('A', 20, 6)",
        "filter_cheby_lp('A', 20, -8, 0.5)",
        "filter_cheby_bp('A', 5, 20, 4, 1)",
        "filter_cheby_bs('A', 5, 20, -4)",
        "integrate('A')",
        "integrate('A', 'B' > 5, range_change(\"Outings:Laps\"))",
        "running_max('A', 'G' > 2, range_change(\"Outings:Laps\"))",
        "running_min('A')",
        "stat_max('A', 'G' > 0, range_change(\"Outings:Laps\"))",
        "stat_mean('A')",
        "flip_flop('A' > 30, 'A' < 10)",
        "edge_delay('A' > 20, 0.05, rise)",
        "edge_delay('A' > 20, 0.05, fall)",
        "time_valid('A' > 20, 0.2)",
        "time_shift('A', 0.1)",
        "time_shift('A', -0.25)",
        "time_shift('A', 1000)",
        "pulse_width('A' > 20)",
        "previous_sample('A', 3)",
        "range_is(\"Outings:Laps\")",
        "range_change(\"Outings:Laps\")",
        "filter_lp(smooth('A', 9), 10, 2) + integrate('C')",
        "smooth(derivative('A', 3), 0.1) * 'B'",
        "time_shift(smooth('A', 7), -0.05) - previous_sample('A', 2)",
        "derivative(filter_cheby_lp('A', 20, 4, 0.5), 'B')",
    };
    return list;
}

struct Buffer {
    std::vector<double> values;
    std::vector<std::uint8_t> bits;

    explicit Buffer(std::uint32_t n) : values(n, 0.0), bits((std::size_t{n} + 7) / 8, 0) {}
    BlockView view() {
        return BlockView{values.data(), bits.data(), store::Dtype::F64,
                         static_cast<std::uint32_t>(values.size())};
    }
};

// The oracle (see the file comment), as evaluateWhole() returns the values (invalid samples NaN).
std::vector<double> oracle(Plan& plan, const EvalContext& ctx) {
    const auto count = static_cast<std::uint32_t>(ctx.resultCount);
    std::vector<Buffer> inputs;
    Micros t0 = 0;
    for (std::size_t k = 0; k < plan.inputs.size(); ++k) {
        const PlanInput& pi = plan.inputs[k];
        Result<MathInputGatherer> host = ctx.getChannel(pi.key, pi.unit, ctx.resultRate);
        REQUIRE(host);
        MathInputGatherer g(host->series(), host->rate(), host->outT0(), pi.interp, host->converter(),
                            ctx.cancel, host->priority());
        t0 = k == 0 ? host->outT0() : t0;
        Buffer& b = inputs.emplace_back(count);
        for (std::uint32_t at = 0; at < count; at += store::kChunkSamples) {
            const std::uint32_t n = std::min(store::kChunkSamples, count - at);
            REQUIRE(g.fill(ctx.resultFirst + at, n,
                           BlockView{b.values.data() + at, b.bits.data() + at / 8, store::Dtype::F64, n}));
        }
    }
    std::vector<Buffer> outputs;
    for (Stage& stage : plan.stages) {
        stage.kernel->reset(ctx);
        REQUIRE(stage.kernel->status());
        std::vector<BlockView> in;
        in.reserve(stage.inputs.size());
        for (const Stage::Ref& r : stage.inputs) {
            in.push_back(r.from == Stage::Ref::From::Input ? inputs[r.index].view()
                                                           : outputs[r.index].view());
        }
        BlockCtx b;
        b.first = ctx.resultFirst;
        b.n = count;
        b.t0 = t0;
        b.rate = ctx.resultRate;
        b.ranges = ctx.ranges;
        b.mainDist = ctx.mainDist;
        b.refDist = ctx.refDist;
        b.cancel = ctx.cancel;
        Buffer& out = outputs.emplace_back(count);
        stage.kernel->run(b, in, out.view());
        REQUIRE(stage.kernel->status());
    }
    Buffer& last = outputs.back();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (((last.bits[i / 8] >> (i % 8)) & 1u) == 0) {
            last.values[i] = kNaN;
        }
    }
    lang::finaliseResult(last.values.data(), count, plan.outDtype);
    return last.values;
}

// The stream pulled in pieces of 1 .. maxPiece samples, rounded as evaluateWhole() rounds.
std::vector<double> pulledInPieces(KernelFixture::Prepared& p, std::mt19937_64& rng, std::uint64_t maxPiece) {
    lang::PlanStream stream(p.plan, p.ctx);
    REQUIRE(stream.start());
    std::vector<double> out(static_cast<std::size_t>(stream.size()));
    std::uint64_t at = 0;
    while (at < out.size()) {
        const std::uint64_t n = std::min<std::uint64_t>(out.size() - at, 1 + rng() % maxPiece);
        REQUIRE(stream.pull(out.data() + at, n, CancelToken{}));
        at += n;
        CHECK(stream.produced() == at);
    }
    lang::finaliseResult(out.data(), out.size(), p.plan.outDtype);
    return out;
}

// Bitwise equality (every invalid sample is the same NaN after finaliseResult).
std::size_t differences(const std::vector<double>& got, const std::vector<double>& want) {
    if (got.size() != want.size()) {
        return std::max(got.size(), want.size());
    }
    std::size_t bad = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::bit_cast<std::uint64_t>(got[i]) != std::bit_cast<std::uint64_t>(want[i])) {
            if (bad++ < 3) {
                UNSCOPED_INFO("sample " << i << ": got " << got[i] << " want " << want[i]);
            }
        }
    }
    return bad;
}

} // namespace

TEST_CASE("plan stream: pulled in any pieces it gives the whole-extent values bit for bit", "[property]") {
    KernelFixture f;
    addChannels(f, testSeed());
    std::mt19937_64 rng(testSeed() ^ 0x5157EA11ull);
    for (const std::string& expr : expressions()) {
        INFO(expr);
        Diagnostics diags;
        Result<KernelFixture::Prepared> a = f.prepare(expr, diags);
        REQUIRE(a);
        const std::vector<double> want = oracle(a->plan, a->ctx);
        // evaluateWhole (the CLI's evaluation) and pulls of random sizes, small and large.
        Result<KernelFixture::Prepared> b = f.prepare(expr, diags);
        REQUIRE(b);
        Result<analysis::UniformSignal> whole = evaluateWhole(b->plan, b->ctx);
        REQUIRE(whole);
        std::vector<double> wholeValues = whole->values;
        for (std::size_t i = 0; i < wholeValues.size(); ++i) {
            wholeValues[i] = whole->validAt(i) ? wholeValues[i] : kNaN;
        }
        CHECK(differences(wholeValues, want) == 0);
        for (const std::uint64_t maxPiece : {std::uint64_t{13}, std::uint64_t{4'500}}) {
            Result<KernelFixture::Prepared> c = f.prepare(expr, diags);
            REQUIRE(c);
            CHECK(differences(pulledInPieces(*c, rng, maxPiece), want) == 0);
        }
    }
}

TEST_CASE("plan stream: restartAt anywhere continues with the values of the continuous stream",
          "[property]") {
    KernelFixture f;
    addChannels(f, testSeed() + 1);
    std::mt19937_64 rng(testSeed() ^ 0x8E57A87ull);
    // Stages that keep no state along the extent (or start afresh at their re-summation points) restart.
    const std::vector<std::pair<std::string, bool>> cases = {
        {"('A' + 'C') / 2", true},
        {"smooth('A', 5)", true},
        {"smooth('B', 5001)", true},
        {"derivative('A')", true},
        {"derivative('A', 'B', 21)", true},
        {"derivative(smooth('A', 5))", true},
        {"smooth(derivative('A', 3), 0.1) * 'B'", true},
        {"time_shift(smooth('A', 7), -0.05) - previous_sample('A', 2)", true},
        {R"(range_change("Outings:Laps") + range_is("Outings:Laps"))", true},
        {"max('A' + 'C', 1) - rand_chan()", true},
        {"filter_lp('A', 10, 2)", false},
        {"filter_lp('A', 5, -2)", false},
        {"integrate('A')", false},
        {"running_max('A', 'G' > 2, range_change(\"Outings:Laps\"))", false},
        {"stat_mean('A')", false},
        {"flip_flop('A' > 30, 'A' < 10)", false},
        {"pulse_width('A' > 20)", false},
        {"smooth('A', 5) + integrate('B')", false},
    };
    for (const auto& [expr, restarts] : cases) {
        INFO(expr);
        Diagnostics diags;
        Result<KernelFixture::Prepared> a = f.prepare(expr, diags);
        REQUIRE(a);
        lang::PlanStream continuous(a->plan, a->ctx);
        REQUIRE(continuous.start());
        CHECK(continuous.restartable() == restarts);
        const auto count = static_cast<std::size_t>(continuous.size());
        std::vector<double> want(count);
        REQUIRE(continuous.pull(want.data(), count, CancelToken{}));
        if (!restarts) {
            CHECK(continuous.restartAt(0, CancelToken{}).error().code == ErrorCode::Unsupported);
            continue;
        }
        Result<KernelFixture::Prepared> b = f.prepare(expr, diags);
        REQUIRE(b);
        lang::PlanStream stream(b->plan, b->ctx);
        REQUIRE(stream.start());
        std::vector<std::uint64_t> firsts = {0,     1,     4095,  4096,  4097,  65535, 65536,
                                             65537, 69999, 70000, 12288, 50000, 3,     8192};
        for (int k = 0; k < 12; ++k) {
            firsts.push_back(rng() % (count + 1));
        }
        for (const std::uint64_t first : firsts) {
            INFO("restart at " << first);
            REQUIRE(stream.restartAt(first, CancelToken{}));
            CHECK(stream.produced() == first);
            const std::uint64_t n = std::min<std::uint64_t>(count - first, 1 + rng() % 9000);
            std::vector<double> got(static_cast<std::size_t>(n));
            REQUIRE(stream.pull(got.data(), n, CancelToken{}));
            CHECK(differences(
                      got, std::vector<double>(want.begin() + static_cast<std::ptrdiff_t>(first),
                                               want.begin() + static_cast<std::ptrdiff_t>(first + n))) == 0);
        }
    }
}

TEST_CASE("plan stream: a cancelled pull fails with Cancelled and a pull past the end is refused", "[unit]") {
    KernelFixture f;
    addChannels(f, testSeed() + 2);
    Diagnostics diags;
    Result<KernelFixture::Prepared> p = f.prepare("smooth('A', 5) * 2", diags);
    REQUIRE(p);
    lang::PlanStream stream(p->plan, p->ctx);
    std::vector<double> out(100);
    CHECK(stream.pull(out.data(), 1, CancelToken{}).error().code == ErrorCode::Invalid); // before start()
    REQUIRE(stream.start());
    CancelSource cancel;
    cancel.cancel();
    CHECK(stream.pull(out.data(), 100, cancel.token()).error().code == ErrorCode::Cancelled);
    CHECK(stream.pull(out.data(), stream.size() + 1, CancelToken{}).error().code == ErrorCode::Invalid);
}

TEST_CASE(
    "plan stream: the elementwise kernel reads F64 inputs without a bitmap in place, with the same values",
    "[unit]") {
    KernelFixture f;
    addChannels(f, testSeed() + 3);
    Diagnostics diags;
    Result<KernelFixture::Prepared> p = f.prepare("('A' + 'B') * 2 - 'B' / 3", diags);
    REQUIRE(p);
    REQUIRE(p->plan.stages.size() == 1);
    auto* ew = dynamic_cast<lang::EwKernel*>(p->plan.stages[0].kernel.get());
    REQUIRE(ew != nullptr);
    constexpr std::uint32_t kN = 10'000;
    std::mt19937_64 rng(testSeed());
    std::vector<double> a = walk(rng, kN, 3.0, true, false);
    std::vector<double> b = walk(rng, kN, 9.0, false, false);
    std::vector<std::uint8_t> allValid((kN + 7) / 8, 0xFF);
    const auto runWith = [&](std::uint8_t* bitsA, std::uint8_t* bitsB) {
        std::vector<double> out(kN);
        std::vector<std::uint8_t> outBits((kN + 7) / 8);
        const BlockView in[] = {BlockView{a.data(), bitsA, store::Dtype::F64, kN},
                                BlockView{b.data(), bitsB, store::Dtype::F64, kN}};
        BlockCtx ctx;
        ctx.n = kN;
        ew->reset(p->ctx);
        ew->run(ctx, in, BlockView{out.data(), outBits.data(), store::Dtype::F64, kN});
        return out;
    };
    std::vector<std::uint8_t> bitsA = allValid;
    std::vector<std::uint8_t> bitsB = allValid;
    const std::vector<double> copied = runWith(bitsA.data(), bitsB.data()); // loads copy and apply the bitmap
    const std::vector<double> inPlace = runWith(nullptr, nullptr);          // loads read the inputs in place
    CHECK(differences(inPlace, copied) == 0);
    // A bitmap still marks a sample invalid whatever its value.
    bitsB[5] = 0xFE; // sample 40
    const std::vector<double> masked = runWith(bitsA.data(), bitsB.data());
    CHECK(std::isnan(masked[40]));
    CHECK(differences(std::vector<double>(masked.begin(), masked.begin() + 40),
                      std::vector<double>(inPlace.begin(), inPlace.begin() + 40)) == 0);
}

TEST_CASE("plan stream: chunk values truncate and clamp to I32 as the rounding and the conversion do",
          "[unit]") {
    const double values[] = {0.0,
                             -0.0,
                             0.5,
                             -0.5,
                             1.999,
                             -1.999,
                             2147483646.7,
                             2147483647.0,
                             2147483647.5,
                             2147483648.0,
                             -2147483647.5,
                             -2147483648.0,
                             -2147483648.9,
                             -2147483649.0,
                             1e300,
                             -1e300,
                             std::numeric_limits<double>::denorm_min(),
                             12345.678};
    for (const double v : values) {
        INFO(v);
        CHECK(maths::detail::truncToI32(v) == maths::detail::toI32(std::trunc(v)));
    }
    const std::vector<double> inRange = {0.0, -3.5, 2147483646.0, -2147483646.0};
    CHECK(maths::detail::finiteBelow(inRange.data(), inRange.size(), 2147483647.0));
    const std::vector<double> big = {0.0, 1.0, -2147483647.0};
    CHECK_FALSE(maths::detail::finiteBelow(big.data(), big.size(), 2147483647.0));
    const std::vector<double> notFinite = {1.0, 2.0, std::numeric_limits<double>::infinity()};
    CHECK_FALSE(maths::detail::finiteBelow(notFinite.data(), notFinite.size(), 2147483647.0));
    const std::vector<double> nan = {1.0, kNaN, 2.0};
    CHECK_FALSE(maths::detail::finiteBelow(nan.data(), nan.size(), 2147483647.0));
}
