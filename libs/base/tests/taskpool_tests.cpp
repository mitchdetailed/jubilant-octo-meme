// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M0-11: 10,000 tasks across three priorities complete; Interactive tasks submitted after 1,000 Background
// tasks start before the 20th Background task; equal-key coalescing returns one shared Future; CancelToken is
// observed within one chunk. Plus the pool semantics of 01 sections 5.1 and 5.4 (replace keys, collective
// cancellation, Rule W, exceptions, type-checked keys, join). Runs under TSan in the sanitize job.
#include "test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace esses;
using esses::test::waitFor;
using namespace std::chrono_literals;

namespace {

// Tasks reach a Gate (and other test locals) by reference, so each test declares them before its TaskPool:
// the pool is destroyed first and joins its workers while they can still touch the Gate (ASan
// stack-use-after-scope on CI otherwise). open() notifies under the lock, so a waiter that returns and lets
// the Gate go out of scope never races the opener's notify.
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        std::lock_guard lock(mutex_);
        open_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Occupies every worker of `pool` until the gate opens; returns when all of them are blocked.
std::vector<Future<int>> blockWorkers(TaskPool& pool, Gate& gate, int workers) {
    std::vector<Future<int>> blockers;
    auto started = std::make_shared<std::atomic<int>>(0);
    for (int w = 0; w < workers; ++w) {
        blockers.push_back(
            pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&gate, started](CancelToken) {
                started->fetch_add(1);
                gate.wait();
                return 0;
            }));
    }
    REQUIRE(waitFor([&] { return started->load() == workers; }));
    return blockers;
}

std::size_t totalQueued(const TaskPool::Stats& s) {
    return s.queued[0] + s.queued[1] + s.queued[2];
}

} // namespace

TEST_CASE("taskpool: 10000 tasks across three priorities complete", "[unit][taskpool]") {
    constexpr int kTasks = 10'000;
    std::atomic<int> executed{0};
    std::vector<Future<int>> futures;
    futures.reserve(kTasks);
    TaskPool pool(4);
    for (int i = 0; i < kTasks; ++i) {
        const auto priority = static_cast<Priority>(i % 3);
        futures.push_back(pool.submit(priority, TaskKey{}, CancelToken{}, [i, &executed](CancelToken) {
            executed.fetch_add(1);
            return i;
        }));
    }
    int wrong = 0;
    for (int i = 0; i < kTasks; ++i) {
        const Result<int>& r = futures[static_cast<std::size_t>(i)].get();
        if (!r.ok() || r.value() != i) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
    CHECK(executed.load() == kTasks);
    CHECK(waitFor([&] { return pool.stats().running == 0; }));
    CHECK(totalQueued(pool.stats()) == 0u);
    CHECK(pool.stats().workers == 4u);
}

TEST_CASE(
    "taskpool: Interactive tasks submitted after 1000 Background tasks start before the 20th Background task",
    "[unit][taskpool]") {
    // One worker: the start number is taken inside the task body, so with several workers a worker that has
    // already claimed an Interactive task can be pre-empted before recording it while the others run hundreds
    // of trivial Background tasks (seen on a loaded CI runner: last Interactive start 506). With one worker
    // the start order is the claim order, so the check is exact. Multi-worker behaviour is covered by the
    // 10,000-task test above.
    constexpr int kWorkers = 1;
    constexpr int kBackground = 1'000;
    constexpr int kInteractive = 10;
    Gate gate;
    std::atomic<int> sequence{0};
    std::vector<int> backgroundStart(kBackground, -1);
    std::vector<int> interactiveStart(kInteractive, -1);
    std::vector<Future<int>> futures;
    TaskPool pool(kWorkers);
    // The first kWorkers Background tasks start at once and hold their workers until the gate opens, so the
    // remaining ones are still queued when the Interactive tasks arrive.
    for (int b = 0; b < kBackground; ++b) {
        futures.push_back(pool.submit(Priority::Background, TaskKey{}, CancelToken{}, [&, b](CancelToken) {
            backgroundStart[static_cast<std::size_t>(b)] = sequence.fetch_add(1);
            if (b < kWorkers) {
                gate.wait();
            }
            return b;
        }));
    }
    REQUIRE(waitFor([&] { return sequence.load() == kWorkers; }));
    for (int f = 0; f < kInteractive; ++f) {
        futures.push_back(pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&, f](CancelToken) {
            interactiveStart[static_cast<std::size_t>(f)] = sequence.fetch_add(1);
            return f;
        }));
    }
    gate.open();
    for (auto& fut : futures) {
        REQUIRE(fut.get().ok());
    }
    std::vector<int> sortedBackground = backgroundStart;
    std::sort(sortedBackground.begin(), sortedBackground.end());
    const int twentiethBackground = sortedBackground[19];
    const int lastInteractive = *std::max_element(interactiveStart.begin(), interactiveStart.end());
    INFO("last Interactive start " << lastInteractive << ", 20th Background start " << twentiethBackground);
    CHECK(lastInteractive < twentiethBackground);
    // Stronger, and exact with one worker: every Interactive task starts before any queued Background task.
    CHECK(lastInteractive < sortedBackground[kWorkers]);
}

TEST_CASE("taskpool: FIFO within a band", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    std::mutex m;
    std::vector<int> order;
    std::vector<Future<int>> futures;
    for (int i = 0; i < 50; ++i) {
        futures.push_back(pool.submit(Priority::Background, TaskKey{}, CancelToken{}, [&, i](CancelToken) {
            std::lock_guard lock(m);
            order.push_back(i);
            return i;
        }));
    }
    gate.open();
    for (auto& f : futures) {
        REQUIRE(f.get().ok());
    }
    std::vector<int> expected(50);
    for (int i = 0; i < 50; ++i) {
        expected[static_cast<std::size_t>(i)] = i;
    }
    CHECK(order == expected);
}

TEST_CASE("taskpool: equal share keys coalesce into one shared Future", "[unit][taskpool]") {
    Gate gate;
    std::atomic<int> runs{0};
    const TaskKey key{"maths/src1/Normal/speed/1/all"};
    TaskPool pool(2);
    auto f1 = pool.submit(Priority::Background, key, CancelToken{}, [&](CancelToken) {
        runs.fetch_add(1);
        gate.wait();
        return 42;
    });
    auto f2 = pool.submit(Priority::Interactive, key, CancelToken{}, [&](CancelToken) {
        runs.fetch_add(1);
        return -1;
    });
    CHECK(f2.sharesStateWith(f1));
    gate.open();
    CHECK(f1.get().value() == 42);
    CHECK(f2.get().value() == 42);
    CHECK(runs.load() == 1);

    // Once the task is done and gone, the same key starts a new task.
    REQUIRE(waitFor([&] { return pool.stats().running == 0; }));
    auto f3 = pool.submit(Priority::Visible, key, CancelToken{}, [&](CancelToken) {
        runs.fetch_add(1);
        return 7;
    });
    CHECK_FALSE(f3.sharesStateWith(f1));
    CHECK(f3.get().value() == 7);
    CHECK(runs.load() == 2);
}

// A worker publishes a task's result (and runs its continuations) a moment before it drops the task's key. A
// submit in that window, after the Future is ready, starts a task of its own instead of joining the finished
// one (01 section 5.4 rule 1): the continuation below submits exactly there, deterministically. The window
// was how MathGraph's Recalculate got a previous result back without evaluating (AC-M2-04, CI on ubuntu).
TEST_CASE("taskpool: a share key submitted once the task's Future is ready starts a new task",
          "[unit][taskpool]") {
    Gate registered;
    Gate submitted;
    std::atomic<int> runs{0};
    const TaskKey key{"maths/src1/Normal/speed/2/all"};
    TaskPool pool(1);
    auto f1 = pool.submit(Priority::Visible, key, CancelToken{}, [&](CancelToken) {
        registered.wait(); // until the continuation is in place, so that it runs on this worker
        return runs.fetch_add(1) + 1;
    });
    Future<int> f2;
    bool joined = true;
    bool readyThere = false;
    f1.onReady([&](const Result<int>&) {
        // On the worker: f1 is ready, its task not yet completed (the key is still registered).
        readyThere = f1.ready();
        f2 = pool.submit(Priority::Visible, key, CancelToken{},
                         [&](CancelToken) { return runs.fetch_add(1) + 1; });
        joined = f2.sharesStateWith(f1);
        submitted.open();
    });
    registered.open();
    submitted.wait();
    CHECK(readyThere);
    CHECK_FALSE(joined);
    CHECK(f1.get().value() == 1);
    CHECK(f2.get().value() == 2); // ran again: one worker, so after f1's task completed
    CHECK(runs.load() == 2);
}

TEST_CASE("taskpool: a chunk key requested by ten components materialises once", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    std::atomic<int> runs{0};
    std::vector<Future<int>> futures;
    for (int c = 0; c < 10; ++c) {
        futures.push_back(pool.submit(Priority::Visible, TaskKey{"chunk/src1/Normal/3/17"}, CancelToken{},
                                      [&](CancelToken) {
                                          runs.fetch_add(1);
                                          return 17;
                                      }));
    }
    gate.open();
    for (auto& f : futures) {
        CHECK(f.sharesStateWith(futures.front()));
        CHECK(f.get().value() == 17);
    }
    CHECK(runs.load() == 1);
}

TEST_CASE("taskpool: coalescing and raise move a queued task to a higher band", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    std::mutex m;
    std::vector<std::string> order;
    const auto record = [&](std::string name) {
        return [&m, &order, name](CancelToken) {
            std::lock_guard lock(m);
            order.push_back(name);
            return 0;
        };
    };
    auto a = pool.submit(Priority::Background, TaskKey{"maths/a"}, CancelToken{}, record("a"));
    auto b = pool.submit(Priority::Visible, TaskKey{"maths/b"}, CancelToken{}, record("b"));
    auto c = pool.submit(Priority::Background, TaskKey{"maths/c"}, CancelToken{}, record("c"));
    pool.raise(TaskKey{"maths/a"}, Priority::Interactive); // explicit raise
    auto c2 = pool.submit(Priority::Interactive, TaskKey{"maths/c"}, CancelToken{},
                          record("x"));                   // raise by coalescing
    pool.raise(TaskKey{"maths/b"}, Priority::Background); // lowering is ignored
    CHECK(pool.stats().queued[0] == 2u);
    gate.open();
    for (auto* f : {&a, &b, &c, &c2}) {
        REQUIRE(f->get().ok());
    }
    CHECK(order == std::vector<std::string>{"a", "c", "b"});
}

TEST_CASE("taskpool: queuedTasks lists the runnable tasks of a key prefix with their bands",
          "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    const auto noop = [](CancelToken) { return 0; };
    auto a = pool.submit(Priority::Background, TaskKey{"maths/s1/a"}, CancelToken{}, noop);
    auto b = pool.submit(Priority::Visible, TaskKey{"maths/s1/b"}, CancelToken{}, noop);
    auto c = pool.submit(Priority::Background, TaskKey{"maths/s2/c"}, CancelToken{}, noop);
    auto d = pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, noop);
    pool.raise(TaskKey{"maths/s1/a"}, Priority::Interactive); // the stale Background entry is not listed

    const std::vector<TaskPool::QueuedTask> s1 = pool.queuedTasks("maths/s1/");
    REQUIRE(s1.size() == 2u);
    CHECK(s1[0].key == "maths/s1/a");
    CHECK(s1[0].priority == Priority::Interactive);
    CHECK(s1[1].key == "maths/s1/b");
    CHECK(s1[1].priority == Priority::Visible);
    CHECK(pool.queuedTasks("maths/").size() == 3u);
    CHECK(pool.queuedTasks("").size() == 4u); // "" includes the unkeyed task
    CHECK(pool.queuedTasks("trace/").empty());

    pool.cancelAll("maths/s2/");
    CHECK(pool.queuedTasks("maths/s2/").empty());
    gate.open();
    for (auto* f : {&a, &b, &d}) {
        REQUIRE(f->get().ok());
    }
    CHECK(c.get().error().code == ErrorCode::Cancelled);
    CHECK(pool.queuedTasks("").empty());
}

TEST_CASE("taskpool: replace keys keep one running and one pending task", "[unit][taskpool]") {
    Gate gate;
    std::atomic<int> started{0};
    const TaskKey key{"trace/ws1/comp1"};
    TaskPool pool(2);
    auto a = pool.submit(Priority::Visible, key, CancelToken{}, [&](CancelToken) {
        started.fetch_add(1);
        gate.wait();
        return 1;
    });
    REQUIRE(waitFor([&] { return started.load() == 1; }));
    auto b = pool.submit(Priority::Visible, key, CancelToken{}, [](CancelToken) { return 2; });
    CHECK(pool.stats().parked == 1u); // waits for the running build, although a worker is free
    auto c = pool.submit(Priority::Visible, key, CancelToken{}, [](CancelToken) { return 3; });
    REQUIRE(b.ready()); // replaced: latest wins on the queue
    CHECK(b.get().error().code == ErrorCode::Cancelled);
    CHECK_FALSE(a.ready()); // the running build is never replaced
    CHECK(pool.stats().parked == 1u);
    gate.open();
    CHECK(a.get().value() == 1);
    CHECK(c.get().value() == 3);
}

TEST_CASE("taskpool: a trace key submitted twice while nothing runs executes once", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    std::atomic<int> runs{0};
    auto x = pool.submit(Priority::Visible, TaskKey{"trace/ws/c"}, CancelToken{}, [&](CancelToken) {
        runs.fetch_add(1);
        return 1;
    });
    auto y = pool.submit(Priority::Visible, TaskKey{"trace/ws/c"}, CancelToken{}, [&](CancelToken) {
        runs.fetch_add(1);
        return 2;
    });
    CHECK(x.get().error().code == ErrorCode::Cancelled);
    gate.open();
    CHECK(y.get().value() == 2);
    CHECK(runs.load() == 1);
}

TEST_CASE("taskpool: CancelToken is observed within one chunk", "[unit][taskpool]") {
    // The task checks its token at every chunk boundary. "Observed within one chunk" means: once cancel() has
    // returned, at most one further chunk begins (the one whose checkpoint raced with cancel()), and the next
    // checkpoint stops the task.
    CancelSource source;
    std::atomic<int> chunksDone{0};
    std::atomic<bool> cancelReturned{false};
    std::atomic<int> chunksBegunAfterCancel{0};
    TaskPool pool(2);
    auto f = pool.submit(Priority::Visible, TaskKey{}, source.token(), [&](CancelToken token) -> Result<int> {
        for (int chunk = 0; chunk < 1'000'000; ++chunk) {
            if (token.checkpoint()) {
                return fail<int>(ErrorCode::Cancelled, "cancelled");
            }
            if (cancelReturned.load()) {
                chunksBegunAfterCancel.fetch_add(1);
            }
            std::this_thread::sleep_for(200us); // one "chunk" of work
            chunksDone.fetch_add(1);
        }
        return 0;
    });
    REQUIRE(waitFor([&] { return chunksDone.load() >= 5; }));
    source.cancel();
    cancelReturned.store(true);
    const Result<int>& r = f.get();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK(chunksBegunAfterCancel.load() <= 1);
}

TEST_CASE("taskpool: a task cancelled while queued never runs", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    CancelSource source;
    std::atomic<bool> ran{false};
    auto f = pool.submit(Priority::Visible, TaskKey{}, source.token(), [&](CancelToken) {
        ran = true;
        return 1;
    });
    source.cancel();
    gate.open();
    CHECK(f.get().error().code == ErrorCode::Cancelled);
    CHECK_FALSE(ran.load());
}

TEST_CASE("taskpool: share-key cancellation is collective", "[unit][taskpool]") {
    CancelSource s1;
    CancelSource s2;
    std::atomic<bool> started{false};
    const auto loop = [&](CancelToken token) -> Result<int> {
        started = true;
        for (int i = 0; i < 20'000; ++i) {
            if (token.checkpoint()) {
                return fail<int>(ErrorCode::Cancelled, "cancelled");
            }
            std::this_thread::sleep_for(1ms);
        }
        return 1;
    };
    TaskPool pool(2);
    auto f1 = pool.submit(Priority::Visible, TaskKey{"maths/s/Normal/x/1/all"}, s1.token(), loop);
    auto f2 = pool.submit(Priority::Visible, TaskKey{"maths/s/Normal/x/1/all"}, s2.token(), loop);
    REQUIRE(f2.sharesStateWith(f1));
    REQUIRE(waitFor([&] { return started.load(); }));
    s1.cancel(); // one requester gives up; the other still wants the result
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(f1.ready());
    s2.cancel(); // now every requester has cancelled
    CHECK(f1.get().error().code == ErrorCode::Cancelled);
}

TEST_CASE("taskpool: a new requester never joins a share task whose cancellation already fired",
          "[unit][taskpool]") {
    CancelSource s1;
    Gate leave;
    std::atomic<bool> started{false};
    std::atomic<bool> observed{false};
    std::atomic<int> runs{0};
    const auto task = [&](CancelToken token) -> Result<int> {
        if (++runs == 1) {
            started = true;
            while (!token.checkpoint()) {
                std::this_thread::sleep_for(1ms);
            }
            observed = true;
            leave.wait(); // still Running, with its token fired and latched
            return fail<int>(ErrorCode::Cancelled, "cancelled");
        }
        return 7;
    };
    TaskPool pool(2);
    auto f1 = pool.submit(Priority::Visible, TaskKey{"maths/s/Normal/y/1/all"}, s1.token(), task);
    REQUIRE(waitFor([&] { return started.load(); }));
    s1.cancel(); // the only requester gives up; the task observes it and is about to resolve Cancelled
    REQUIRE(waitFor([&] { return observed.load(); }));
    auto f2 = pool.submit(Priority::Visible, TaskKey{"maths/s/Normal/y/1/all"}, CancelToken{}, task);
    CHECK_FALSE(f2.sharesStateWith(f1));
    CHECK(f2.get().value() == 7); // a task of its own, not the cancelled one
    leave.open();
    CHECK(f1.get().error().code == ErrorCode::Cancelled);
    CHECK(runs.load() == 2);
}

TEST_CASE("taskpool: chunk keys ignore requester cancellation and stop through cancelAll",
          "[unit][taskpool]") {
    CancelSource requester;
    std::atomic<bool> started{false};
    TaskPool pool(2);
    auto f = pool.submit(Priority::Visible, TaskKey{"chunk/src1/Normal/3/0"}, requester.token(),
                         [&](CancelToken token) -> Result<int> {
                             started = true;
                             for (int i = 0; i < 20'000; ++i) {
                                 if (token.checkpoint()) {
                                     return fail<int>(ErrorCode::Cancelled, "cancelled");
                                 }
                                 std::this_thread::sleep_for(1ms);
                             }
                             return 1;
                         });
    REQUIRE(waitFor([&] { return started.load(); }));
    requester.cancel();
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(f.ready());
    pool.cancelAll("chunk/src2/"); // another source: no effect
    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(f.ready());
    pool.cancelAll("chunk/src1/");
    CHECK(f.get().error().code == ErrorCode::Cancelled);
}

TEST_CASE("taskpool: cancelAll with a prefix cancels queued tasks and leaves the rest", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    auto a = pool.submit(Priority::Visible, TaskKey{"distidx/a/Normal/1"}, CancelToken{},
                         [](CancelToken) { return 1; });
    auto b = pool.submit(Priority::Visible, TaskKey{"distidx/b/Normal/1"}, CancelToken{},
                         [](CancelToken) { return 2; });
    auto u = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [](CancelToken) { return 3; });
    pool.cancelAll("distidx/a/");
    REQUIRE(a.ready());
    CHECK(a.get().error().code == ErrorCode::Cancelled);
    gate.open();
    CHECK(b.get().value() == 2);
    CHECK(u.get().value() == 3);
}

TEST_CASE("taskpool: exceptions never escape a worker", "[unit][taskpool]") {
    TaskPool pool(2);
    auto oom = pool.submit(Priority::Visible, TaskKey{}, CancelToken{},
                           [](CancelToken) -> int { throw std::bad_alloc(); });
    auto boom = pool.submit(Priority::Visible, TaskKey{}, CancelToken{},
                            [](CancelToken) -> int { throw std::runtime_error("boom"); });
    auto odd = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [](CancelToken) -> int { throw 42; });
    CHECK(oom.get().error().code == ErrorCode::OutOfMemory);
    CHECK(boom.get().error().code == ErrorCode::Internal);
    CHECK(boom.get().error().context.detail == std::optional<std::string>("boom"));
    CHECK(odd.get().error().code == ErrorCode::Internal);
    auto still = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [](CancelToken) { return 5; });
    CHECK(still.get().value() == 5);
}

TEST_CASE("taskpool: a key reused with a different result type resolves to Internal", "[unit][taskpool]") {
    test::RecordingAssertHandler recorder;
    Gate gate;
    TaskPool pool(1);
    auto a = pool.submit(Priority::Visible, TaskKey{"maths/k"}, CancelToken{}, [&](CancelToken) {
        gate.wait();
        return 1;
    });
    auto b = pool.submit(Priority::Visible, TaskKey{"maths/k"}, CancelToken{},
                         [](CancelToken) { return std::string("not an int"); });
    REQUIRE(b.ready());
    CHECK(b.get().error().code == ErrorCode::Internal);
    CHECK(b.get().error().message == "task key type mismatch");
#if ESSES_ASSERTS_ENABLED
    CHECK(recorder.failures() == 1);
#endif
    gate.open();
    CHECK(a.get().value() == 1);
}

TEST_CASE("taskpool: Rule W - a worker waiting on a queued task runs it inline", "[unit][taskpool]") {
    TaskPool pool(1); // a single worker: only inline execution can make progress
    auto outer =
        pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&pool](CancelToken) -> Result<int> {
            auto middle = pool.submit(
                Priority::Background, TaskKey{"maths/m"}, CancelToken{}, [&pool](CancelToken) -> Result<int> {
                    auto inner = pool.submit(Priority::Background, TaskKey{"maths/i"}, CancelToken{},
                                             [](CancelToken) { return 20; });
                    const Result<int>& r = inner.get();
                    if (!r.ok()) {
                        return Failure{r.error()};
                    }
                    return r.value() + 1;
                });
            const Result<int>& r = middle.get();
            if (!r.ok()) {
                return Failure{r.error()};
            }
            return r.value() * 2;
        });
    CHECK(outer.get().value() == 42);
}

TEST_CASE("taskpool: tryRunInline runs a queued task on the caller", "[unit][taskpool]") {
    Gate gate;
    TaskPool pool(1);
    auto blockers = blockWorkers(pool, gate, 1);
    std::thread::id ranOn;
    auto f = pool.submit(Priority::Background, TaskKey{"summary/src1"}, CancelToken{}, [&](CancelToken) {
        ranOn = std::this_thread::get_id();
        return 5;
    });
    CHECK(pool.tryRunInline(TaskKey{"summary/src1"}));
    REQUIRE(f.ready());
    CHECK(f.get().value() == 5);
    CHECK(ranOn == std::this_thread::get_id());
    CHECK_FALSE(pool.tryRunInline(TaskKey{"summary/src1"}));
    CHECK_FALSE(pool.tryRunInline(TaskKey{"unknown/key"}));
    gate.open();
}

TEST_CASE("taskpool: join cancels queued work and refuses new work", "[unit][taskpool]") {
    auto pool = std::make_unique<TaskPool>(1);
    std::atomic<bool> started{false};
    auto running = pool->submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](CancelToken token) {
        started = true;
        while (!token.cancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        return 1;
    });
    REQUIRE(waitFor([&] { return started.load(); }));
    auto queued = pool->submit(Priority::Background, TaskKey{}, CancelToken{}, [](CancelToken) { return 2; });
    pool->join();
    CHECK(queued.get().error().code == ErrorCode::Cancelled);
    CHECK(running.get().value() == 1); // the running task observed its token and finished
    auto late = pool->submit(Priority::Visible, TaskKey{}, CancelToken{}, [](CancelToken) { return 3; });
    REQUIRE(late.ready());
    CHECK(late.get().error().code == ErrorCode::Cancelled);
    pool.reset();
}

TEST_CASE("taskpool: install and instance", "[unit][taskpool]") {
    CHECK(TaskPool::installed() == nullptr);
    {
        TaskPool pool(2);
        TaskPool::install(&pool);
        CHECK(&TaskPool::instance() == &pool);
        CHECK(TaskPool::currentWorkerPool() == nullptr);
        auto f = pool.submit(Priority::Visible, TaskKey{}, CancelToken{},
                             [](CancelToken) { return TaskPool::currentWorkerPool() != nullptr; });
        CHECK(f.get().value());
    }
    CHECK(TaskPool::installed() == nullptr); // a destroyed pool uninstalls itself
    CHECK(TaskPool::defaultWorkerCount() >= 2u);
}

TEST_CASE("taskpool: task keys classify by prefix", "[unit][taskpool]") {
    CHECK(TaskKey{}.kind() == TaskKey::Kind::None);
    CHECK(TaskKey{"trace/w/c"}.kind() == TaskKey::Kind::Replace);
    CHECK(TaskKey{"geom/w/c"}.kind() == TaskKey::Kind::Replace);
    CHECK(TaskKey{"report/w/c"}.kind() == TaskKey::Kind::Replace);
    CHECK(TaskKey{"chunk/s/l/1/2"}.kind() == TaskKey::Kind::ShareOwnedBySource);
    CHECK(TaskKey{"summary/s"}.kind() == TaskKey::Kind::ShareOwnedBySource);
    CHECK(TaskKey{"maths/s/l/k/1/all"}.kind() == TaskKey::Kind::Share);
    CHECK(TaskKey{"open/c:/x.ld/10/20/a"}.kind() == TaskKey::Kind::Share);
    CHECK(TaskKey{"export/1234"}.kind() == TaskKey::Kind::Share);
    CHECK(priorityName(Priority::Visible) == "Visible");
}

TEST_CASE("future: ready futures, tryGet, onReady, take and void results", "[unit][future]") {
    auto ready = makeReadyFuture(Result<int>(5));
    CHECK(ready.ready());
    REQUIRE(ready.tryGet().has_value());
    CHECK(ready.tryGet()->value() == 5);
    int seen = 0;
    ready.onReady([&](const Result<int>& r) { seen = r.value(); }); // already ready: runs inline
    CHECK(seen == 5);

    auto unique = makeReadyFuture(Result<std::unique_ptr<int>>(std::make_unique<int>(9)));
    Result<std::unique_ptr<int>> taken = std::move(unique).take();
    REQUIRE(taken.ok());
    CHECK(*taken.value() == 9);

    Gate gate;
    TaskPool pool(2);
    auto pending = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](CancelToken) {
        gate.wait();
        return 3;
    });
    std::atomic<int> continuation{0};
    pending.onReady([&](const Result<int>& r) { continuation = r.value(); });
    CHECK_FALSE(pending.tryGet().has_value());
    gate.open();
    CHECK(pending.get().value() == 3);
    CHECK(waitFor([&] { return continuation.load() == 3; }));

    std::atomic<bool> sideEffect{false};
    Future<void> v =
        pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](CancelToken) { sideEffect = true; });
    CHECK(v.get().ok());
    CHECK(sideEffect.load());

    Future<int> invalid;
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(invalid.ready());
}

TEST_CASE("taskpool: blocking on a NoBlock thread asserts in debug builds", "[unit][taskpool]") {
    test::RecordingAssertHandler recorder;
    Gate gate;
    TaskPool pool(1);
    auto f = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](CancelToken) {
        gate.wait();
        return 1;
    });
    std::thread opener([&] {
        std::this_thread::sleep_for(50ms);
        gate.open();
    });
    {
        ThreadRole::Scope role(ThreadRole::NoBlock);
        CHECK(ThreadRole::isNoBlock());
        CHECK(f.get().value() == 1);
    }
    opener.join();
    CHECK_FALSE(ThreadRole::isNoBlock());
#if ESSES_ASSERTS_ENABLED
    CHECK(recorder.failures() == 1);
#else
    CHECK(recorder.failures() == 0);
#endif
}

TEST_CASE("executor: InlineExecutor runs on the caller and TaskPool on a worker", "[unit][taskpool]") {
    InlineExecutor inlineExec;
    std::thread::id id;
    inlineExec.run(Priority::Visible, TaskKey{}, CancelToken{},
                   [&](CancelToken) { id = std::this_thread::get_id(); });
    CHECK(id == std::this_thread::get_id());
    CancelSource cancelled;
    cancelled.cancel();
    bool ran = false;
    inlineExec.run(Priority::Visible, TaskKey{}, cancelled.token(), [&](CancelToken) { ran = true; });
    CHECK_FALSE(ran);

    TaskPool pool(2);
    Executor& exec = pool;
    std::atomic<bool> done{false};
    std::thread::id workerId;
    exec.run(Priority::Background, TaskKey{}, CancelToken{}, [&](CancelToken) {
        workerId = std::this_thread::get_id();
        done = true;
    });
    REQUIRE(waitFor([&] { return done.load(); }));
    CHECK(workerId != std::this_thread::get_id());
}
