// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Column: require/tryPin/prefetch semantics, coalescing, Rule W on a small pool, sequential providers,
// cancellation, failures, version bumps under concurrent readers, buildSummary (02 section 3.2 behaviours
// 6-11; 01 sections 5.1 and 5.3; 02 section 8 item 4).
#include "store_test_support.hpp"

#include <esses/base/cancel.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace esses;
using namespace esses::store;

namespace {

constexpr std::size_t kGiB = std::size_t{1} << 30;

std::shared_ptr<test::TestProvider<float>> rampProvider(SampleIdx n, bool independent = true) {
    test::ColumnData<float> d;
    d.v.resize(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < d.v.size(); ++i) {
        d.v[i] = static_cast<float>(i);
    }
    return std::make_shared<test::TestProvider<float>>(std::move(d), independent);
}

bool poolIdle(const TaskPool& pool) {
    const TaskPool::Stats s = pool.stats();
    return s.running == 0 && s.queued[0] == 0 && s.queued[1] == 0 && s.queued[2] == 0;
}

// A provider that breaks the contract: allocates the wrong sample count.
class WrongCountProvider final : public IChunkProvider {
public:
    SampleIdx size() const override { return 100; }
    Dtype dtype() const override { return Dtype::F32; }
    std::uint64_t version() const override { return 1; }
    bool chunkIndependent() const override { return true; }
    Result<void> materialise(SampleIdx, ChunkBuilder& out, CancelToken) override {
        return out.allocate(50, Dtype::F32);
    }
    std::string debugName() const override { return "wrong-count"; }
};

// version() throws once, on a chosen call: an exception inside the Building section of ensure().
class ThrowingVersionProvider final : public IChunkProvider {
public:
    explicit ThrowingVersionProvider(int throwOnCall) : throwOnCall_(throwOnCall) {}
    SampleIdx size() const override { return kChunkSamples; }
    Dtype dtype() const override { return Dtype::F32; }
    std::uint64_t version() const override {
        if (++calls_ == throwOnCall_) {
            throw std::runtime_error("injected fault in version()");
        }
        return 1;
    }
    bool chunkIndependent() const override { return true; }
    Result<void> materialise(SampleIdx, ChunkBuilder& out, CancelToken) override {
        return out.allocate(static_cast<std::uint32_t>(kChunkSamples), Dtype::F32);
    }
    std::string debugName() const override { return "throwing-version"; }

private:
    int throwOnCall_;
    mutable std::atomic<int> calls_{0};
};

} // namespace

TEST_CASE("column: an exception while a chunk is Building leaves it absent, never stuck", "[unit][column]") {
    ChunkCache cache(kGiB);
    // Call 1 is ensure()'s version read under the lock; call 2 the race check after materialise(), inside
    // the Building section.
    auto provider = std::make_shared<ThrowingVersionProvider>(2);
    Column col(provider, ColumnOptions{&cache});
    Result<ChunkHandle> first = col.require(0, 10, Priority::Visible, {});
    REQUIRE_FALSE(first.ok());
    CHECK(first.error().code == ErrorCode::Internal);
    CHECK_FALSE(col.isResident(0));
    // Without the reset the slot stayed Building with this thread as its builder: "recursive
    // materialisation" here, an endless wait on any other thread.
    Result<ChunkHandle> second = col.require(0, 10, Priority::Visible, {});
    REQUIRE(second.ok());
    CHECK(col.isResident(0));
}

TEST_CASE("column: tryPin and prefetch build a sequential provider's chunks 0..n in order",
          "[unit][column]") {
    ChunkCache cache(kGiB);
    TaskPool pool(3);
    auto provider = rampProvider(8 * kChunkSamples, /*independent=*/false);
    Column col(provider, ColumnOptions{&cache, &pool});
    CHECK_FALSE(col.tryPin(5, Priority::Visible).has_value()); // schedules chunk 5's task
    col.prefetch(4, Priority::Background);
    col.prefetch(6, Priority::Visible);
    REQUIRE(test::waitFor([&] { return col.isResident(6) && col.isResident(5) && col.isResident(4); }));
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    // Every chunk once, never one out of order (02 section 3.2 behaviour 7).
    CHECK(provider->order() == std::vector<SampleIdx>{0, 1, 2, 3, 4, 5, 6});
}

TEST_CASE("column: require pins a clamped span including the partial last chunk", "[unit][column]") {
    ChunkCache cache(kGiB);
    const SampleIdx n = 2 * kChunkSamples + 77;
    std::vector<double> v(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = 0.5 * static_cast<double>(i);
    }
    Column col(InMemoryProvider::fromF64(v), ColumnOptions{&cache});
    CHECK(col.size() == n);
    CHECK(col.chunkCount() == 3);
    CHECK(col.dtype() == Dtype::F64);
    CHECK(col.chunkIndependent());
    Result<ChunkHandle> h = col.require(-5, n + 5, Priority::Visible, {});
    REQUIRE(h.ok());
    CHECK(h->firstChunk() == 0);
    CHECK(h->lastChunk() == 2);
    CHECK(h->endSample() == n);
    CHECK(h->chunk(2)->count() == 77u);
    CHECK(h->at(n - 1) == 0.5 * static_cast<double>(n - 1));
    CHECK(h->samples<double>(1)[3] == 0.5 * static_cast<double>(kChunkSamples + 3));
    CHECK(h->contains(n - 1));
    CHECK_FALSE(h->contains(-1));
    CHECK(col.require(n, n + 10, Priority::Visible, {}).error().code == ErrorCode::Invalid);
    CHECK(col.require(10, 5, Priority::Visible, {}).error().code == ErrorCode::Invalid);

    Column empty;
    CHECK_FALSE(empty.valid());
    CHECK(empty.size() == 0);
    CHECK(empty.chunkCount() == 0);
    CHECK(std::isnan(empty.at(0)));
    CHECK(empty.require(0, 0, Priority::Visible, {}).error().code == ErrorCode::Invalid);
    CHECK(empty.summary().complete());
    CHECK_FALSE(empty.tryPin(0, Priority::Visible).has_value());
}

TEST_CASE("column: require and buildSummary refuse to block a NoBlock thread", "[unit][column]") {
    ChunkCache cache(kGiB);
    Column col(rampProvider(100), ColumnOptions{&cache});
    test::RecordingAssertHandler asserts;
    {
        ThreadRole::Scope gui(ThreadRole::NoBlock);
        CHECK(col.require(0, 10, Priority::Interactive, {}).error().code == ErrorCode::Invalid);
        CHECK(col.buildSummary({}).error().code == ErrorCode::Invalid);
        CHECK(std::isnan(col.at(3))); // tryPin path is allowed: absent, nothing scheduled without a pool
    }
    CHECK(test::RecordingAssertHandler::failures() == (ESSES_ASSERTS_ENABLED ? 2 : 0));
    CHECK(col.require(0, 10, Priority::Interactive, {}).ok());
    CHECK(col.at(3) == 3.0);
}

TEST_CASE("column: tryPin and prefetch schedule once on the pool and pin after sealing", "[unit][column]") {
    ChunkCache cache(kGiB);
    TaskPool pool(2);
    auto provider = rampProvider(4 * kChunkSamples);
    provider->setDelayMs(30);
    std::atomic<int> sealed{0};
    ColumnOptions opt{&cache, &pool};
    opt.onChunkSealed = [&](SampleIdx) { sealed.fetch_add(1); };
    Column col(provider, opt);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(col.tryPin(1, Priority::Visible).has_value()); // coalesced: scheduled once
    }
    col.tryPin(1, Priority::Interactive); // raises the queued task
    REQUIRE(test::waitFor([&] { return col.isResident(1); }));
    std::optional<ChunkHandle> h = col.tryPin(1, Priority::Visible);
    REQUIRE(h.has_value());
    CHECK(h->at(kChunkSamples + 9) == static_cast<double>(kChunkSamples + 9));
    CHECK(provider->calls(1) == 1);

    col.prefetch(2, Priority::Background);
    col.prefetch(2, Priority::Background);
    REQUIRE(test::waitFor([&] { return col.isResident(2); }));
    CHECK(provider->calls(2) == 1);
    CHECK(cache.stats().pinnedChunks == 1u); // prefetch does not pin
    REQUIRE(test::waitFor([&] { return sealed.load() == 2; }));
    CHECK(col.at(2 * kChunkSamples + 1) == static_cast<double>(2 * kChunkSamples + 1));
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    pool.join();
}

TEST_CASE("column: chunk tasks use the column's key prefix and cancelAll stops them", "[unit][column]") {
    ChunkCache cache(kGiB);
    TaskPool pool(1);
    ColumnOptions opt{&cache, &pool};
    opt.taskKeyPrefix = "chunk/src42/layer0/7";
    auto provider = rampProvider(2 * kChunkSamples);
    Column col(provider, opt);
    CHECK(col.chunkTaskKey(3).text == "chunk/src42/layer0/7/3");
    CHECK(Column(rampProvider(10), ColumnOptions{&cache}).chunkTaskKey(0).text.starts_with("chunk/column-"));

    // Occupy the only worker so the chunk task stays queued.
    CancelSource release;
    std::atomic<bool> blocking{false};
    Future<void> blocker = pool.submit(Priority::Interactive, TaskKey{}, {}, [&](CancelToken) {
        blocking.store(true);
        while (!release.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    REQUIRE(test::waitFor([&] { return blocking.load(); }));
    CHECK_FALSE(col.tryPin(0, Priority::Visible).has_value());
    CHECK(pool.stats().queued[1] == 1u);
    pool.cancelAll("chunk/src42"); // the source closed
    CHECK(pool.stats().queued[1] == 0u);
    release.cancel();
    REQUIRE(test::waitFor([&] { return blocker.ready(); }));
    CHECK(provider->calls(0) == 0);
    // The cancelled task cleared its scheduled mark, so the chunk can be scheduled again.
    CHECK_FALSE(col.tryPin(0, Priority::Visible).has_value());
    REQUIRE(test::waitFor([&] { return col.isResident(0); }));
    CHECK(provider->calls(0) == 1);
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    pool.join();
}

TEST_CASE("column: concurrent require, tryPin, prefetch and at materialise every chunk once",
          "[unit][column]") {
    ChunkCache cache(kGiB);
    TaskPool pool(4);
    const SampleIdx chunks = 24;
    auto provider = rampProvider(chunks * kChunkSamples);
    provider->setDelayMs(2);
    Column col(provider, ColumnOptions{&cache, &pool});
    std::atomic<int> badReads{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(test::testSeed() + static_cast<std::uint64_t>(t));
            for (int i = 0; i < 60; ++i) {
                const auto c = static_cast<SampleIdx>(rng() % chunks);
                const SampleIdx probe = chunkFirstSample(c) + static_cast<SampleIdx>(rng() % kChunkSamples);
                switch (rng() % 4) {
                case 0: {
                    const SampleIdx last = std::min(chunks - 1, c + static_cast<SampleIdx>(rng() % 3));
                    Result<ChunkHandle> h =
                        col.require(chunkFirstSample(c), chunkFirstSample(last), Priority::Visible, {});
                    if (!h || h->at(probe) != static_cast<double>(probe)) {
                        badReads.fetch_add(1);
                    }
                    break;
                }
                case 1:
                    if (std::optional<ChunkHandle> h = col.tryPin(c, Priority::Visible)) {
                        if (h->at(probe) != static_cast<double>(probe)) {
                            badReads.fetch_add(1);
                        }
                    }
                    break;
                case 2:
                    col.prefetch(c, Priority::Background);
                    break;
                default: {
                    const double v = col.at(probe);
                    if (!std::isnan(v) && v != static_cast<double>(probe)) {
                        badReads.fetch_add(1);
                    }
                    break;
                }
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    CHECK(badReads.load() == 0);
    for (SampleIdx c = 0; c < chunks; ++c) {
        INFO("chunk " << c);
        CHECK(provider->calls(c) <= 1);
        CHECK(col.isResident(c) == (provider->calls(c) == 1));
    }
    CHECK(cache.stats().pinnedChunks == 0u);
    pool.join();
}

TEST_CASE("column: Rule W - 2 workers complete 20 span builds over cold chunks with a 3-level nested chain",
          "[unit][column]") {
    // 01 section 5.1 acceptance: a TaskPool with 2 workers completes 20 concurrent builds over cold chunks
    // plus a nested chain; workers only wait for chunks another running thread is building.
    ChunkCache cache(kGiB);
    TaskPool pool(2);
    const SampleIdx chunks = 16;
    auto pa = rampProvider(chunks * kChunkSamples);
    auto pb = rampProvider(chunks * kChunkSamples);
    auto pc = rampProvider(chunks * kChunkSamples);
    pa->setDelayMs(1);
    Column a(pa, ColumnOptions{&cache, &pool});
    Column b(pb, ColumnOptions{&cache, &pool});
    Column c(pc, ColumnOptions{&cache, &pool});
    // b's chunk n reads a's chunk n; c's chunk n reads b's chunk n (inline recursion, Rule W (c)).
    pb->setHook([a](SampleIdx n, CancelToken cancel) -> Result<void> {
        Result<ChunkHandle> h =
            a.require(chunkFirstSample(n), chunkFirstSample(n + 1) - 1, Priority::Visible, cancel);
        return h ? ok() : Result<void>(Failure{h.error()});
    });
    pc->setHook([b](SampleIdx n, CancelToken cancel) -> Result<void> {
        Result<ChunkHandle> h =
            b.require(chunkFirstSample(n), chunkFirstSample(n + 1) - 1, Priority::Visible, cancel);
        return h ? ok() : Result<void>(Failure{h.error()});
    });
    std::vector<Future<void>> builds;
    std::mt19937_64 rng(test::testSeed());
    for (int i = 0; i < 20; ++i) {
        const auto first = static_cast<SampleIdx>(rng() % (chunks - 4));
        const SampleIdx last = first + 1 + static_cast<SampleIdx>(rng() % 3);
        const Column& target = i % 3 == 0 ? a : (i % 3 == 1 ? b : c);
        builds.push_back(pool.submit(Priority::Visible, TaskKey{"trace/ws/" + std::to_string(i)}, {},
                                     [target, first, last](CancelToken token) -> Result<void> {
                                         Result<ChunkHandle> h = target.require(
                                             chunkFirstSample(first), chunkFirstSample(last + 1) - 1,
                                             Priority::Visible, token);
                                         return h ? ok() : Result<void>(Failure{h.error()});
                                     }));
    }
    const bool done = test::waitFor(
        [&] {
            return std::all_of(builds.begin(), builds.end(), [](const Future<void>& f) { return f.ready(); });
        },
        std::chrono::seconds(60));
    REQUIRE(done);
    for (const Future<void>& f : builds) {
        CHECK(f.tryGet()->ok());
    }
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    for (SampleIdx n = 0; n < chunks; ++n) {
        CHECK(pa->calls(n) <= 1);
        CHECK(pb->calls(n) <= 1);
        CHECK(pc->calls(n) <= 1);
    }
    pool.join();
}

TEST_CASE("column: a sequential provider is materialised 0..last in order", "[unit][column]") {
    ChunkCache cache(kGiB);
    auto provider = rampProvider(6 * kChunkSamples, /*independent=*/false);
    Column col(provider, ColumnOptions{&cache});
    CHECK_FALSE(col.chunkIndependent());
    Result<ChunkHandle> h = col.require(chunkFirstSample(3), chunkFirstSample(3) + 5, Priority::Visible, {});
    REQUIRE(h.ok());
    CHECK(h->firstChunk() == 3);
    CHECK(provider->order() == std::vector<SampleIdx>{0, 1, 2, 3});
    CHECK(col.isResident(0)); // materialised (resident) but not pinned
    CHECK(cache.stats().pinnedChunks == 1u);
    REQUIRE(col.require(chunkFirstSample(5), chunkFirstSample(5), Priority::Visible, {}).ok());
    CHECK(provider->order() == std::vector<SampleIdx>{0, 1, 2, 3, 4, 5});
    // The summary pass streams a sequential provider through every chunk in order.
    Column fresh(provider, ColumnOptions{&cache});
    REQUIRE(fresh.buildSummary({}).ok());
    const std::vector<SampleIdx> order = provider->order();
    CHECK(std::vector<SampleIdx>(order.end() - 6, order.end()) == std::vector<SampleIdx>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("column: cancellation before and during materialisation", "[unit][column]") {
    ChunkCache cache(kGiB);
    auto provider = rampProvider(2 * kChunkSamples);
    Column col(provider, ColumnOptions{&cache});
    CancelSource early;
    early.cancel();
    CHECK(col.require(0, 0, Priority::Visible, early.token()).error().code == ErrorCode::Cancelled);
    CHECK(provider->totalCalls() == 0);

    std::atomic<bool> entered{false};
    provider->setHook([&](SampleIdx, CancelToken token) -> Result<void> {
        entered.store(true);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return fail(ErrorCode::Cancelled, "cancelled in decode");
    });
    CancelSource during;
    std::thread canceller([&] {
        test::waitFor([&] { return entered.load(); });
        during.cancel();
    });
    CHECK(col.require(0, kChunkSamples, Priority::Visible, during.token()).error().code ==
          ErrorCode::Cancelled);
    canceller.join();
    CHECK_FALSE(col.isResident(0)); // nothing half-published
    CHECK(cache.stats().chunks == 0u);
    provider->setHook({});
    CHECK(col.require(0, kChunkSamples, Priority::Visible, {}).ok());
}

TEST_CASE("column: a failed materialisation propagates and leaves the chunk absent", "[unit][column]") {
    ChunkCache cache(kGiB);
    auto provider = rampProvider(kChunkSamples);
    std::atomic<int> attempts{0};
    provider->setHook([&](SampleIdx, CancelToken) -> Result<void> {
        if (attempts.fetch_add(1) == 0) {
            return fail(ErrorCode::Io, "injected read error");
        }
        return ok();
    });
    Column col(provider, ColumnOptions{&cache});
    Result<ChunkHandle> first = col.require(0, 10, Priority::Visible, {});
    REQUIRE_FALSE(first.ok());
    CHECK(first.error().code == ErrorCode::Io);
    CHECK_FALSE(col.isResident(0));
    CHECK(col.require(0, 10, Priority::Visible, {}).ok());

    Column bad(std::make_shared<WrongCountProvider>(), ColumnOptions{&cache});
    Result<ChunkHandle> r = bad.require(0, 10, Priority::Visible, {});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Internal);
    CHECK(r.error().context.channel == std::optional<std::string>("wrong-count"));
}

TEST_CASE("column: at() reads resident chunks only and reports invalid samples as NaN", "[unit][column]") {
    ChunkCache cache(kGiB);
    std::vector<float> v(1'000, 1.5f);
    std::vector<bool> valid(1'000, true);
    valid[7] = false;
    v[8] = std::numeric_limits<float>::quiet_NaN();
    Column col(InMemoryProvider::fromF32(v, valid), ColumnOptions{&cache});
    CHECK(std::isnan(col.at(5))); // absent, and no pool to schedule it
    REQUIRE(col.require(0, 999, Priority::Visible, {}).ok());
    CHECK(col.at(5) == 1.5);
    CHECK(std::isnan(col.at(7)));
    CHECK(std::isnan(col.at(8)));
    CHECK(std::isnan(col.at(-1)));
    CHECK(std::isnan(col.at(1'000)));
}

TEST_CASE("column: version bumps under concurrent readers never expose freed or mixed chunks",
          "[unit][column]") {
    ChunkCache cache(kGiB);
    TaskPool pool(3);
    const SampleIdx chunks = 6;
    test::ColumnData<float> d;
    d.v.resize(static_cast<std::size_t>(chunks * kChunkSamples));
    auto provider = std::make_shared<test::TestProvider<float>>(std::move(d));
    provider->setValueFn([](std::uint64_t version, SampleIdx) { return static_cast<float>(version); });
    Column col(provider, ColumnOptions{&cache, &pool});
    std::atomic<bool> stop{false};
    std::atomic<int> mixed{0};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937_64 rng(test::testSeed() + 100u + static_cast<std::uint64_t>(t));
            while (!stop.load()) {
                const auto c = static_cast<SampleIdx>(rng() % chunks);
                // A pinned chunk is never torn or freed under the reader, and its samples belong to the
                // version it is stamped with (a bump during a decode makes the Column discard and rebuild the
                // chunk).
                auto check = [&](const ChunkHandle& h) {
                    const Chunk* k = h.chunk(c);
                    const std::span<const float> s = k->samples<float>();
                    const auto want = static_cast<float>(k->version());
                    if (s.front() != want || s.back() != want || s[s.size() / 2] != want) {
                        mixed.fetch_add(1);
                    }
                    reads.fetch_add(1);
                };
                if (t % 2 == 0) {
                    if (std::optional<ChunkHandle> h = col.tryPin(c, Priority::Visible)) {
                        check(*h);
                    }
                } else if (Result<ChunkHandle> h =
                               col.require(chunkFirstSample(c), chunkFirstSample(c), Priority::Visible, {})) {
                    check(*h);
                }
            }
        });
    }
    // Progress is synchronised, not timed: the bumps start once a reader has read, and the readers stop only
    // after a read that followed the last bump (under a loaded parallel ctest the readers may otherwise not
    // be scheduled before the bump loop ends).
    REQUIRE(test::waitFor([&] { return reads.load() > 0; }));
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        provider->bumpVersion();
    }
    const int readsAtLastBump = reads.load();
    REQUIRE(test::waitFor([&] { return reads.load() > readsAtLastBump; }));
    stop.store(true);
    for (std::thread& th : readers) {
        th.join();
    }
    REQUIRE(test::waitFor([&] { return poolIdle(pool); }));
    CHECK(mixed.load() == 0);
    CHECK(reads.load() > 0);
    const ChunkCache::Stats s = cache.stats();
    CHECK(s.pinnedChunks == 0u);
    CHECK(s.chunks <= static_cast<std::size_t>(chunks)); // every stale chunk was freed at pin count zero
    cache.clear();
    CHECK(cache.stats().chunks == 0u);
    CHECK(cache.stats().residentBytes == 0u);
    pool.join();
}

TEST_CASE("column: buildSummary reads resident chunks pinned, decodes the rest, and honours cancel",
          "[unit][column]") {
    ChunkCache cache(kGiB);
    auto provider = rampProvider(5 * kChunkSamples + 3);
    Column col(provider, ColumnOptions{&cache});
    REQUIRE(col.require(0, 2 * kChunkSamples - 1, Priority::Visible, {}).ok()); // chunks 0, 1 resident
    CHECK(col.summary().filledChunks() == 2);
    CHECK_FALSE(col.summary().complete());
    const ChunkCache::Stats before = cache.stats();
    CancelSource cancelled;
    cancelled.cancel();
    CHECK(col.buildSummary(cancelled.token()).error().code == ErrorCode::Cancelled);
    REQUIRE(col.buildSummary({}).ok());
    CHECK(col.summary().complete());
    CHECK(cache.stats().residentBytes == before.residentBytes);
    for (SampleIdx n = 0; n < col.chunkCount(); ++n) {
        CHECK(provider->calls(n) == 1); // resident chunks were not decoded again
    }
    const SummaryPyramid& s = col.summary();
    CHECK(s.minmax(0, col.size() - 1) == (MinMax{0.0f, static_cast<float>(col.size() - 1)}));
    CHECK(s.version() == col.version());
    CHECK(s.bytes() >= static_cast<std::size_t>(col.chunkCount()) * kSummaryEntriesPerChunk * sizeof(MinMax));
    // A version bump gives the column a fresh, empty summary; the old snapshot stays readable.
    std::shared_ptr<const SummaryPyramid> old = col.summarySnapshot();
    provider->bumpVersion();
    CHECK_FALSE(col.summary().complete());
    CHECK(old->complete());
    CHECK(col.summary().version() == 2u);
}
