// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// StreamPlanProvider (src/graph/stream_provider.hpp, D-191): an expression node's chunks equal, bit for bit
// (samples, validity bitmap, pyramid), the chunks PlanProvider makes from evaluateWhole()'s result, for F32,
// F64 and I32 columns with invalid and overflowing samples, for plans that restart at any chunk and plans
// that share one stream; chunks built concurrently in any order equal the chunks built in order; a failure is
// remembered and a cancellation is not; the evaluations are released after the last chunk and a later
// request starts afresh. MathGraph's columns of the same expressions equal evaluateWhole() bit for bit while
// the nodes run at once on a pool with several workers. Expectations come from evaluateWhole() and
// PlanProvider (the whole-extent path), never from stored values.

#include "../kernels/kernel_test_support.hpp"
#include "graph/stream_provider.hpp"
#include "graph_test_support.hpp"

#include <esses/base/cancel.hpp>
#include <esses/maths/plan_provider.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/column.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

constexpr std::size_t kSamples = 150'000; // 150 s at 1 kHz: three chunks

std::vector<double> signal(std::uint64_t seed, double level, bool invalid) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> step(0.0, 0.5);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<double> v(kSamples);
    double x = level;
    for (double& sample : v) {
        x += step(rng) + 0.001 * (level - x);
        sample = invalid && unit(rng) < 0.001 ? kNaN : x;
    }
    if (invalid) {
        for (std::size_t i = 70'000; i < 72'000; ++i) {
            v[i] = kNaN; // a 2 s gap across nothing in particular
        }
    }
    return v;
}

// "A" F64 with invalid samples, "B" F64 without, "F" F32 with invalid samples, all 1 kHz.
void addChannels(KernelFixture& f) {
    f.add("A", signal(testSeed(), 20.0, true), Rate::fromHz(1000));
    f.add("B", signal(testSeed() + 1, 4.0, false), Rate::fromHz(1000));
    f.addF32("F", signal(testSeed() + 2, 10.0, true), Rate::fromHz(1000));
}

// Compiles `expr` and starts a stream over the fixture's extent, as MathGraph's task does.
graph::NodeEvaluationFactory factoryOf(KernelFixture& f, const std::string& expr) {
    return [&f, expr](const CancelToken& token) -> Result<std::unique_ptr<graph::NodeEvaluation>> {
        Diagnostics diags;
        Result<KernelFixture::Prepared> p = f.prepare(expr, diags);
        if (!p) {
            return Failure{p.error()};
        }
        auto ev = std::make_unique<graph::NodeEvaluation>();
        ev->plan = std::move(p->plan);
        p->ctx.cancel = token;
        ev->stream = std::make_unique<lang::PlanStream>(ev->plan, std::move(p->ctx));
        if (Result<void> r = ev->stream->start(); !r) {
            return Failure{r.error()};
        }
        return ev;
    };
}

struct Columns {
    std::shared_ptr<graph::StreamPlanProvider> streamed;
    store::Column stream;
    store::Column whole; // PlanProvider over evaluateWhole()
    store::Dtype dtype = store::Dtype::F32;
};

// The streamed column of `expr` and the reference column of the same samples through PlanProvider.
Columns columnsOf(KernelFixture& f, const std::string& expr) {
    Diagnostics diags;
    Result<KernelFixture::Prepared> p = f.prepare(expr, diags);
    REQUIRE(p);
    const store::Dtype dtype = p->plan.outDtype;
    Result<analysis::UniformSignal> whole = evaluateWhole(p->plan, p->ctx);
    REQUIRE(whole);
    auto snapshot = std::make_shared<PlanSnapshot>();
    snapshot->version = Version{0x1234, 0x5678};
    snapshot->outDtype = dtype;
    snapshot->rate = whole->rate;
    snapshot->t0 = whole->t0;
    snapshot->size = static_cast<SampleIdx>(whole->values.size());
    snapshot->name = expr;
    store::ColumnOptions options;
    options.cache = &f.runtime().cache;
    options.pool = &f.runtime().pool;
    const graph::NodeEvaluationFactory factory = factoryOf(f, expr);
    Result<std::unique_ptr<graph::NodeEvaluation>> first = factory(CancelToken{});
    REQUIRE(first);
    graph::StreamPlanProvider::Info info;
    info.version = Version{0x1234, 0x9abc};
    info.dtype = dtype;
    info.size = snapshot->size;
    info.name = expr;
    auto streamed = std::make_shared<graph::StreamPlanProvider>(info, factory, std::move(first).value());
    Columns c{streamed, store::Column(streamed, options),
              store::Column(std::make_shared<PlanProvider>(snapshot, std::move(whole).value()), options),
              dtype};
    return c;
}

// Samples, validity and pyramid of two sealed chunks.
void requireSameChunk(const store::Chunk& got, const store::Chunk& want) {
    REQUIRE(got.count() == want.count());
    REQUIRE(got.dtype() == want.dtype());
    REQUIRE(got.validity() == want.validity());
    const std::span<const std::uint8_t> gb = got.validBits();
    const std::span<const std::uint8_t> wb = want.validBits();
    REQUIRE(gb.size() == wb.size());
    CHECK(std::equal(gb.begin(), gb.end(), wb.begin()));
    const std::size_t bytes = std::size_t{got.count()} * store::dtypeSize(got.dtype());
    CHECK(std::memcmp(got.data(), want.data(), bytes) == 0);
    for (unsigned k = 0; k < store::kPyramidLevels; ++k) {
        const std::span<const store::MinMax> gl = got.level(k);
        const std::span<const store::MinMax> wl = want.level(k);
        REQUIRE(gl.size() == wl.size());
        CHECK(std::memcmp(gl.data(), wl.data(), gl.size() * sizeof(store::MinMax)) == 0);
    }
}

void requireSameColumns(Columns& c) {
    const SampleIdx size = c.stream.size();
    REQUIRE(size == c.whole.size());
    Result<store::ChunkHandle> got = c.stream.require(0, size - 1, Priority::Visible, {});
    Result<store::ChunkHandle> want = c.whole.require(0, size - 1, Priority::Visible, {});
    REQUIRE(got);
    REQUIRE(want);
    for (SampleIdx n = 0; n < c.stream.chunkCount(); ++n) {
        INFO("chunk " << n);
        REQUIRE(got->chunk(n) != nullptr);
        REQUIRE(want->chunk(n) != nullptr);
        requireSameChunk(*got->chunk(n), *want->chunk(n));
    }
}

const std::vector<std::pair<std::string, bool>>& cases() { // expression, restarting
    static const std::vector<std::pair<std::string, bool>> list = {
        {"smooth('A', 5) + 'B'", true},               // F64
        {"'F' * 2 - 1", true},                        // F32
        {"'F' * 1e38", true},                         // F32, overflows to invalid at the cast
        {"'A' > 20", true},                           // I32
        {"exp('A' / 2)", true},                       // F64, overflows to invalid
        {"derivative(smooth('F', 9))", true},         // F32
        {"filter_lp('A', 10, 2)", false},             // F64, one stream
        {"filter_cheby_lp('F', 20, -4, 0.5)", false}, // F32, whole extent
        {"flip_flop('A' > 25, 'A' < 15)", false},     // I32
        {"integrate('F', 'B' > 4)", false},           // F32
    };
    return list;
}

} // namespace

TEST_CASE("stream provider: a node's chunks equal PlanProvider's chunks of evaluateWhole bit for bit",
          "[unit][graph]") {
    KernelFixture f(4);
    addChannels(f);
    for (const auto& [expr, restarting] : cases()) {
        INFO(expr);
        Columns c = columnsOf(f, expr);
        CHECK(c.streamed->restarting() == restarting);
        CHECK(c.streamed->chunkIndependent());
        requireSameColumns(c);
        CHECK_FALSE(c.streamed->holdsEvaluation()); // every chunk produced: the evaluations are gone
        CHECK(c.stream.summary().complete());
    }
}

TEST_CASE("stream provider: chunks built concurrently in any order equal the chunks built in order",
          "[property][graph]") {
    KernelFixture f(4);
    addChannels(f);
    std::mt19937_64 rng(testSeed());
    for (const auto& [expr, restarting] : cases()) {
        INFO(expr);
        Columns ref = columnsOf(f, expr);
        Result<store::ChunkHandle> want = ref.whole.require(0, ref.whole.size() - 1, Priority::Visible, {});
        REQUIRE(want);
        for (int round = 0; round < 3; ++round) {
            Columns c = columnsOf(f, expr);
            const SampleIdx chunks = c.stream.chunkCount();
            // Every chunk twice (a request after the release starts afresh), shuffled, over four workers.
            std::vector<SampleIdx> order;
            order.reserve(static_cast<std::size_t>(chunks) * 2);
            for (int twice = 0; twice < 2; ++twice) {
                for (SampleIdx n = 0; n < chunks; ++n) {
                    order.push_back(n);
                }
            }
            std::shuffle(order.begin(), order.end(), rng);
            std::mutex mutex;
            std::vector<std::pair<SampleIdx, store::OwnedChunk>> built;
            std::atomic<int> failures{0};
            std::vector<Future<void>> done;
            done.reserve(order.size());
            for (const SampleIdx n : order) {
                done.push_back(f.runtime().pool.submit(
                    Priority::Visible, TaskKey{}, CancelToken{}, [&, n](const CancelToken&) -> Result<void> {
                        store::ChunkBuilder builder;
                        if (Result<void> r = c.streamed->materialise(n, builder, CancelToken{}); !r) {
                            failures.fetch_add(1);
                            return r;
                        }
                        Result<store::OwnedChunk> sealed = builder.seal(1);
                        if (!sealed) {
                            failures.fetch_add(1);
                            return Failure{sealed.error()};
                        }
                        std::lock_guard lock(mutex);
                        built.emplace_back(n, std::move(sealed).value());
                        return ok();
                    }));
            }
            for (Future<void>& d : done) {
                static_cast<void>(d.get());
            }
            REQUIRE(failures.load() == 0);
            REQUIRE(built.size() == order.size());
            for (const auto& [n, chunk] : built) {
                INFO("chunk " << n);
                requireSameChunk(*chunk, *want->chunk(n));
            }
        }
    }
}

TEST_CASE("stream provider: a failure is remembered, a cancellation is not, and evaluations are released",
          "[unit][graph]") {
    KernelFixture f(2);
    addChannels(f);
    for (const bool restarting : {true, false}) {
        const std::string expr = restarting ? "'A' + 'B'" : "integrate('A')";
        INFO(expr);
        Columns ref = columnsOf(f, expr);
        Result<store::ChunkHandle> want = ref.whole.require(0, ref.whole.size() - 1, Priority::Visible, {});
        REQUIRE(want);

        // A cancelled request fails with Cancelled and is not remembered.
        Columns c = columnsOf(f, expr);
        const SampleIdx last = c.stream.chunkCount() - 1;
        CancelSource cancel;
        cancel.cancel();
        store::ChunkBuilder b0;
        CHECK(c.streamed->materialise(last, b0, cancel.token()).error().code == ErrorCode::Cancelled);
        for (SampleIdx n = 0; n <= last; ++n) {
            store::ChunkBuilder b;
            REQUIRE(c.streamed->materialise(n, b, CancelToken{}));
            Result<store::OwnedChunk> sealed = b.seal(1);
            REQUIRE(sealed);
            requireSameChunk(**sealed, *want->chunk(n));
        }
        CHECK_FALSE(c.streamed->holdsEvaluation());
        const std::uint32_t started = c.streamed->evaluationsStarted();
        store::ChunkBuilder again;
        REQUIRE(c.streamed->materialise(1, again, CancelToken{})); // after the release: a fresh evaluation
        CHECK(c.streamed->evaluationsStarted() == started + 1);
        Result<store::OwnedChunk> sealedAgain = again.seal(1);
        REQUIRE(sealedAgain);
        requireSameChunk(**sealedAgain, *want->chunk(1));

        // A failing evaluation is remembered: the factory is not asked again.
        std::atomic<int> calls{0};
        graph::StreamPlanProvider::Info info;
        info.dtype = ref.dtype;
        info.size = ref.whole.size();
        info.name = "failing";
        const graph::NodeEvaluationFactory good = factoryOf(f, expr);
        Result<std::unique_ptr<graph::NodeEvaluation>> first = good(CancelToken{});
        REQUIRE(first);
        graph::StreamPlanProvider failing(
            info,
            [&calls](const CancelToken&) -> Result<std::unique_ptr<graph::NodeEvaluation>> {
                calls.fetch_add(1);
                ErrorContext ctx;
                ctx.detail = "E-ARG-VALUE";
                return Failure{Error{ErrorCode::Invalid, "E-ARG-VALUE: failing on purpose", ctx}};
            },
            std::move(first).value());
        // The handed-in evaluation serves the chunks in order; the next request needs the factory.
        for (SampleIdx n = 0; n <= last; ++n) {
            store::ChunkBuilder b;
            REQUIRE(failing.materialise(n, b, CancelToken{}));
        }
        for (int k = 0; k < 3; ++k) {
            store::ChunkBuilder b;
            const Result<void> r = failing.materialise(0, b, CancelToken{});
            REQUIRE_FALSE(r);
            CHECK(r.error().context.detail == "E-ARG-VALUE");
        }
        CHECK(calls.load() == 1);
    }
}

TEST_CASE("math graph: expression columns equal evaluateWhole bit for bit while the nodes run at once",
          "[unit][graph]") {
    KernelFixture f(6);
    addChannels(f);
    const std::vector<std::string> exprs = {
        "smooth('A', 5) + 'B'",
        "derivative(smooth('F', 9))",
        "filter_lp('A', 10, 2)",
        "filter_cheby_lp('F', 20, -4, 0.5)",
        "'A' > 20",
        "integrate('F', 'B' > 4)",
        "flip_flop('A' > 25, 'A' < 15)",
        "time_shift('A', -0.05) * 'F'",
        "'F' * 1e38",
        "exp('A' / 2)",
        "stat_max('A', 'B' > 4)",
        "derivative('A', 'B', 21)",
    };
    std::vector<MathItem> items;
    items.reserve(exprs.size());
    for (std::size_t k = 0; k < exprs.size(); ++k) {
        items.push_back(expr("Out " + std::to_string(k), exprs[k]));
    }
    const MathModule m = module("stream", items);
    for (int round = 0; round < 3; ++round) {
        MathGraph g(f.source(), "Normal", shippedUnits(), {}, &f.runtime().pool, &f.runtime().cache);
        static_cast<void>(g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr));
        std::vector<Future<ColumnHandle>> futures;
        futures.reserve(exprs.size());
        for (std::size_t k = 0; k < exprs.size(); ++k) {
            futures.push_back(
                g.require(ChannelKey::make(f.source()->id, "Normal", "Out " + std::to_string(k)),
                          SampleRange::all(), Priority::Visible, {}));
        }
        for (std::size_t k = 0; k < exprs.size(); ++k) {
            INFO(exprs[k]);
            const Result<ColumnHandle>& r = futures[k].get();
            REQUIRE(r);
            Result<analysis::UniformSignal> got = readWholeSignal(r->series, {});
            REQUIRE(got);
            KernelFixture::Outcome want = f.eval(exprs[k]);
            REQUIRE(want.signal);
            REQUIRE(got->values.size() == want.signal->values.size());
            CHECK(got->rate == want.signal->rate);
            std::size_t bad = 0;
            for (std::size_t i = 0; i < got->values.size(); ++i) {
                const bool gv = got->validAt(i);
                const bool wv = want.signal->validAt(i);
                if (gv != wv || (gv && std::bit_cast<std::uint64_t>(got->values[i]) !=
                                           std::bit_cast<std::uint64_t>(want.signal->values[i]))) {
                    ++bad;
                }
            }
            CHECK(bad == 0);
        }
        static_cast<void>(g.applyReadyCompletions());
    }
}
