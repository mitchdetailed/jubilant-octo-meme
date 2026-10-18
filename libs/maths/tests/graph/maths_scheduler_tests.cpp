// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsScheduler (03 section 7.4): share keys that carry the chunk (".../all", ".../c<n>"), coalescing of a
// repeated submit (one task, one more requester, the priority raised), and the refusals.

#include "../maths_test_support.hpp"

#include <esses/maths/maths_scheduler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>

using namespace esses;
using namespace esses::maths;

TEST_CASE("maths scheduler: share keys coalesce and carry the chunk", "[unit][graph][scheduler]") {
    TaskPool pool(2);
    MathsScheduler scheduler(&pool);
    CHECK(scheduler.pool() == &pool);
    auto runs = std::make_shared<std::atomic<int>>(0);
    const auto work = [runs](const CancelToken&) -> Result<ColumnHandle> {
        runs->fetch_add(1);
        ColumnHandle h;
        h.key.nameCf = "x";
        return h;
    };
    const MathTaskKey all{hash128("node"), MathTaskKey::kAll};
    Future<ColumnHandle> a;
    Future<ColumnHandle> b;
    {
        test::WorkerGate gate(pool, 2);
        a = scheduler.submit(all, "maths/src/Normal/x/v", Priority::Background, {}, work);
        b = scheduler.submit(all, "maths/src/Normal/x/v", Priority::Visible, {}, work);
        CHECK(a.sharesStateWith(b));
        const std::vector<TaskPool::QueuedTask> queued = pool.queuedTasks("maths/src/");
        REQUIRE(queued.size() == 1);
        CHECK(queued[0].key == "maths/src/Normal/x/v/all");
        CHECK(queued[0].priority == Priority::Visible); // raised by the second requester
        Future<ColumnHandle> c = scheduler.submit(MathTaskKey{hash128("node"), 3}, "maths/src/Normal/x/v",
                                                  Priority::Background, {}, work);
        CHECK_FALSE(c.sharesStateWith(a));
        CHECK(pool.queuedTasks("maths/src/Normal/x/v/c3").size() == 1);
        gate.release();
        REQUIRE(c.get());
    }
    REQUIRE(a.get());
    CHECK(b.get()->key.nameCf == "x");
    CHECK(runs->load() == 2);
}

TEST_CASE("maths scheduler: refusals resolve ready instead of blocking", "[unit][graph][scheduler]") {
    REQUIRE(TaskPool::installed() == nullptr);
    MathsScheduler none;
    Future<ColumnHandle> f =
        none.submit(MathTaskKey{}, "maths/x/all", Priority::Visible, {},
                    [](const CancelToken&) -> Result<ColumnHandle> { return ColumnHandle{}; });
    REQUIRE(f.ready());
    CHECK(f.get().error().code == ErrorCode::Invalid);
    TaskPool pool(1);
    MathsScheduler scheduler(&pool);
    CHECK(scheduler.submit(MathTaskKey{}, "maths/x", Priority::Visible, {}, {}).get().error().code ==
          ErrorCode::Invalid);
    CHECK(scheduler
              .submit(MathTaskKey{}, "", Priority::Visible, {},
                      [](const CancelToken&) -> Result<ColumnHandle> { return ColumnHandle{}; })
              .get()
              .error()
              .code == ErrorCode::Invalid);
}
