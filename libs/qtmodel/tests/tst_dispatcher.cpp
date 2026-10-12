// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dispatcher and AppRuntime (01 section 5.3 rules 3, 4 and 11; D-031, D-123; AC-M1-12): FIFO delivery on the
// GUI thread, closures of destroyed contexts skipped, posts from many workers, close() discarding pending
// work, the debug assertion behind "the GUI thread never blocks on a Future", and a shutdown with builds in
// flight.

#include <esses/base/assert.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>

#include <QCoreApplication>
#include <QTest>
#include <QThread>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace esses;

namespace {

std::atomic<int> gAsserts{0};
std::atomic<bool> gNoBlockAssert{false};

void recordingHandler(const char* expression, const char*, int, const char* message) {
    gAsserts.fetch_add(1);
    const std::string text = std::string(expression) + (message != nullptr ? message : "");
    if (text.find("NoBlock") != std::string::npos) {
        gNoBlockAssert.store(true);
    }
}

struct AssertRecorder {
    AssertHandler previous;
    AssertRecorder() : previous(setAssertHandler(&recordingHandler)) {
        gAsserts.store(0);
        gNoBlockAssert.store(false);
    }
    ~AssertRecorder() { setAssertHandler(previous); }
    AssertRecorder(const AssertRecorder&) = delete;
    AssertRecorder& operator=(const AssertRecorder&) = delete;
};

} // namespace

class TstDispatcher : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void runsOnTheGuiThreadAndSkipsDeadContexts() {
        qt::Dispatcher dispatcher;
        QVERIFY(dispatcher.isGuiThread());
        int ran = 0;
        QObject alive;
        auto* doomed = new QObject;
        dispatcher.post(&alive, [&ran] { ++ran; });
        dispatcher.post(doomed, [&ran] { ran += 100; });
        delete doomed;
        QCOMPARE(dispatcher.pending(), std::size_t{2});
        QTRY_COMPARE(ran, 1);
        QCOMPARE(dispatcher.pending(), std::size_t{0});
        dispatcher.post(nullptr, [&ran] { ++ran; }); // no context: refused
        dispatcher.post(&alive, {});                 // no closure: refused
        QCOMPARE(dispatcher.pending(), std::size_t{0});
        dispatcher.close();
        QVERIFY(dispatcher.closed());
        dispatcher.post(&alive, [&ran] { ++ran; });
        QCOMPARE(dispatcher.pending(), std::size_t{0});
        QCoreApplication::processEvents();
        QCOMPARE(ran, 1);
    }

    void deliversInFifoOrderAcrossDrains() {
        qt::Dispatcher dispatcher;
        QObject context;
        std::vector<int> order;
        for (int i = 0; i < 5; ++i) {
            dispatcher.post(&context, [&order, &dispatcher, &context, i] {
                order.push_back(i);
                if (i == 2) { // a closure posting more work: it runs after everything queued before it
                    dispatcher.post(&context, [&order] { order.push_back(99); });
                }
            });
        }
        QTRY_COMPARE(order.size(), std::size_t{6});
        QCOMPARE(order, (std::vector<int>{0, 1, 2, 3, 4, 99}));
    }

    void postsFromWorkersRunOnTheGuiThread() {
        qt::Dispatcher dispatcher;
        QObject context;
        constexpr int kThreads = 8;
        constexpr int kPosts = 250;
        std::atomic<int> offGui{0};
        int ran = 0;
        std::vector<int> lastSeen(kThreads, -1);
        bool ordered = true;
        QThread* gui = QThread::currentThread();
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    for (int i = 0; i < kPosts; ++i) {
                        dispatcher.post(&context, [&, t, i] {
                            if (QThread::currentThread() != gui) {
                                offGui.fetch_add(1);
                            }
                            ordered = ordered && lastSeen[static_cast<std::size_t>(t)] == i - 1;
                            lastSeen[static_cast<std::size_t>(t)] = i;
                            ++ran;
                        });
                    }
                });
            }
            for (std::thread& th : threads) {
                th.join();
            }
        }
        QTRY_COMPARE(ran, kThreads * kPosts);
        QCOMPARE(offGui.load(), 0);
        QVERIFY(ordered); // per producer, FIFO
    }

    void drainRunsQueuedClosuresNow() {
        qt::Dispatcher dispatcher;
        QObject context;
        int ran = 0;
        dispatcher.post(&context, [&ran] { ++ran; });
        dispatcher.drain();
        QCOMPARE(ran, 1);
        QCoreApplication::processEvents(); // the queued wake-up finds nothing left
        QCOMPARE(ran, 1);
    }

    // AC-M1-12: a NoBlock (GUI) thread that waits on a Future trips the debug assertion; the tryGet path does
    // not. The recording handler returns, so the wait then completes when the worker publishes.
    void guiThreadNeverBlocksOnAFuture() {
#if ESSES_ASSERTS_ENABLED
        const AssertRecorder recorder;
        TaskPool pool(2);
        const ThreadRole::Scope role(ThreadRole::NoBlock);
        Future<int> f = pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, [](const CancelToken&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return 7;
        });
        static_cast<void>(f.tryGet()); // the non-blocking accessor is allowed
        static_cast<void>(f.ready());
        QCOMPARE(gAsserts.load(), 0);
        const int v = *f.get(); // blocking accessor on the NoBlock thread
        QCOMPARE(v, 7);
        QVERIFY(gNoBlockAssert.load());
#else
        QSKIP("assertions are compiled out in this configuration");
#endif
    }

    void runtimeInstallsAndShutsDown() {
        qt::AppRuntimeOptions options;
        options.workers = 2;
        options.cacheBudget = std::size_t{64} << 20;
        qt::AppRuntime rt(options);
        QCOMPARE(TaskPool::installed(), &rt.pool());
        QCOMPARE(store::ChunkCache::installed(), &rt.cache());
        QVERIFY(!rt.isShutDown());
        rt.shutdown();
        QVERIFY(rt.isShutDown());
        QVERIFY(TaskPool::installed() == nullptr);
        QVERIFY(store::ChunkCache::installed() == nullptr);
        QVERIFY(rt.dispatcher().closed());
        rt.shutdown(); // idempotent
    }

    void runtimeMarksTheGuiThreadNoBlockOnRequest() {
        const ThreadRole::Scope restore(ThreadRole::Normal);
        {
            qt::AppRuntimeOptions options;
            options.workers = 2;
            options.cacheBudget = std::size_t{16} << 20;
            options.markGuiThreadNoBlock = true;
            qt::AppRuntime rt(options);
            QVERIFY(ThreadRole::isNoBlock());
        }
        // The runtime gives the thread its previous role back (shutdown, also from the destructor), so a
        // later runtime or test on the same thread does not inherit the mark.
        QVERIFY(!ThreadRole::isNoBlock());
        {
            qt::AppRuntimeOptions options;
            options.workers = 2;
            options.cacheBudget = std::size_t{16} << 20;
            options.markGuiThreadNoBlock = true;
            qt::AppRuntime rt(options);
            QVERIFY(ThreadRole::isNoBlock());
            rt.shutdown();
            QVERIFY(!ThreadRole::isNoBlock());
        }
        QVERIFY(!ThreadRole::isNoBlock());
    }

    // A sink removed before shutdown no longer receives records; shutdown removes the rest.
    void runtimeRemovesASinkOnRequest() {
        struct Counting final : ILogSink {
            std::atomic<int> records{0};
            void log(const LogRecord&) override { records.fetch_add(1); }
        };
        qt::AppRuntimeOptions options;
        options.workers = 2;
        options.cacheBudget = std::size_t{16} << 20;
        qt::AppRuntime rt(options);
        auto kept = std::make_shared<Counting>();
        auto removed = std::make_shared<Counting>();
        rt.addSink(kept);
        rt.addSink(removed);
        rt.removeSink(removed);
        rt.removeSink(removed); // unknown now: no-op
        ESSES_LOG(Warning, "test.sink", "one record");
        QCOMPARE(kept->records.load(), 1);
        QCOMPARE(removed->records.load(), 0);
        rt.shutdown();
        ESSES_LOG(Warning, "test.sink", "after shutdown");
        QCOMPARE(kept->records.load(), 1);
    }

    // 01 section 5.3 rule 11: closing with 20 builds and a chain of dependent tasks in flight neither crashes
    // nor runs a closure after shutdown.
    void shutdownWithBuildsInFlight() {
        std::atomic<int> started{0};
        std::atomic<int> delivered{0};
        {
            qt::AppRuntimeOptions options;
            options.workers = 2;
            options.cacheBudget = std::size_t{64} << 20;
            qt::AppRuntime rt(options);
            QObject context;
            qt::Dispatcher* dispatcher = &rt.dispatcher();
            for (int i = 0; i < 20; ++i) {
                Future<int> f =
                    rt.pool().submit(Priority::Visible, TaskKey{"trace/ws/" + std::to_string(i)},
                                     CancelToken{}, [&started](const CancelToken& cancel) -> Result<int> {
                                         started.fetch_add(1);
                                         for (int k = 0; k < 50 && !cancel.checkpoint(); ++k) {
                                             std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                         }
                                         return 1;
                                     });
                f.onReady([dispatcher, &context, &delivered](const Result<int>&) {
                    dispatcher->post(&context, [&delivered] { delivered.fetch_add(1); });
                });
            }
            // A three-level chain: each task waits on the previous one (Rule W runs a queued one inline).
            Future<int> a = rt.pool().submit(Priority::Background, TaskKey{"maths/chain/a"}, CancelToken{},
                                             [](const CancelToken&) { return 1; });
            Future<int> b = rt.pool().submit(Priority::Background, TaskKey{"maths/chain/b"}, CancelToken{},
                                             [a](const CancelToken&) -> Result<int> { return *a.get() + 1; });
            Future<int> c = rt.pool().submit(Priority::Background, TaskKey{"maths/chain/c"}, CancelToken{},
                                             [b](const CancelToken&) -> Result<int> { return *b.get() + 1; });
            QTRY_VERIFY(started.load() > 0);
            rt.shutdown();
            const int afterShutdown = delivered.load();
            QCoreApplication::processEvents();
            QCOMPARE(delivered.load(), afterShutdown); // the closed dispatcher runs nothing
            QVERIFY(c.ready());
        }
        QVERIFY(started.load() <= 20);
    }
};

QTEST_GUILESS_MAIN(TstDispatcher)
#include "tst_dispatcher.moc"
