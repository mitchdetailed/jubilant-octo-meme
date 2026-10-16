// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Teardown order of a MathGraph's runtime (01 section 5.3 rule 11): the ChunkCache must outlive the TaskPool
// that evaluates over it. A pool worker publishes a node's result, wakes the requester, then runs the
// Future's continuations and completes the task; until then it holds the result's shared state, and through
// it the result's Column (and the source Columns its provider reads). A requester that lets go of the graph,
// the Future and the source as soon as the Future is ready can therefore reach ~ChunkCache while a Column
// still uses the cache, which ESSES_ASSERT reports (bench.maths_bench.smoke aborted this way on Linux CI).
// The test holds the worker inside that window with a continuation, so the window is always open here, and
// checks that the order of test::Runtime (cache first, pool second: the pool joins before the cache goes)
// tears down cleanly under the same assertion.

#include "graph_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

using namespace esses;
using namespace esses::maths;

namespace {

class Latch {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Opens the latches when the test leaves, so that a failed REQUIRE never leaves the worker waiting while the
// pool joins.
struct OpenOnExit {
    Latch& first;
    Latch& second;
    ~OpenOnExit() {
        first.open();
        second.open();
    }
};

} // namespace

TEST_CASE("math graph teardown: a worker holds the result after its Future is ready, so the cache must "
          "outlive the pool",
          "[unit][graph]") {
    test::Runtime rt(1); // one worker, held by the gate until the continuation is registered
    Latch gate;
    Latch inContinuation;
    Latch release;
    const OpenOnExit openOnExit{gate, release};
    bool ranOnWorker = false;
    const std::thread::id requester = std::this_thread::get_id();
    Future<int> blocker =
        rt.pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&gate](CancelToken) {
            gate.wait();
            return 0;
        });
    {
        MathGraph g = test::makeGraph(rt, test::graphSource(rt.cache, "teardown-source"));
        const MathModule m = test::module("user", {test::expr("Doubled", "'rpm' * 2")});
        g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
        Future<ColumnHandle> f =
            g.require(test::keyOf(g, "Doubled"), SampleRange::all(), Priority::Visible, CancelToken{});
        REQUIRE(!f.ready());
        f.onReady([&](const Result<ColumnHandle>&) {
            ranOnWorker = std::this_thread::get_id() != requester;
            inContinuation.open();
            if (ranOnWorker) {
                release.wait(); // the worker stays between publishing the result and completing the task
            }
        });
        gate.open();
        REQUIRE(f.get().ok());
        inContinuation.wait();
    } // the requester lets go of the graph, its Future and the source
    REQUIRE(blocker.get().ok());
    CHECK(ranOnWorker);
    // The worker still holds the result: its Column, and the source Columns its provider reads, keep chunks
    // in the cache. Destroying the cache now (a cache declared after the pool) would trip ESSES_ASSERT.
    CHECK(rt.cache.stats().chunks > 0);
    release.open();
    rt.pool.join(); // ~TaskPool does this first; the cache, declared before the pool, is destroyed after it
    CHECK(rt.cache.stats().chunks == 0);
    CHECK(rt.cache.stats().pinnedChunks == 0);
}
