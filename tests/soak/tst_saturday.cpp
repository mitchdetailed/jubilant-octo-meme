// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Saturday soak (07 section 7.1 "Soak", AC-M4-03; 08 sections 4.1 and 6.10; D-213), offscreen in the real
// shell: ten logs open (eight .ld + .ldx sessions and two CSV exports, written by the test, soak_data.hpp)
// and the day's workflow in a loop (soak_driver.hpp) for ESSES_SOAK_SECONDS (default 60; at least
// ESSES_SOAK_MIN_CYCLES cycles, default 2, so a short run still does every step once). It fails on
//   * a crash (the process dies; under ASan with the stack);
//   * a Dispatcher thread violation: every ESSES_ASSERT (the Dispatcher's GUI-thread check, the GUI thread's
//     NoBlock checks) and Qt's thread-affinity warnings abort at once (soak_guard.hpp);
//   * pending work that never drains: every step ends on the pool settled with every posted result delivered
//     and the loader idle, within ESSES_SOAK_SETTLE_SECONDS (default 900);
//   * unbounded growth, judged per cycle in the same state (soak_metrics.hpp): the heap, the resident set,
//   the
//     chunk cache, the shell's widgets and objects, the peak of queued tasks and Dispatcher closures; and, at
//     the end, a chunk cache that does not give every byte back once every log is closed and the window is
//     gone;
//   * leaks (the soak job: LeakSanitizer with lsan.supp, judged by lsan_check.py).
// The guard's own tests run first: its message rule and growth rule by hand, a recorded Dispatcher violation
// of each kind in this process, and the fatal path in a child process (ctest soak.guard; ctest
// soak.tst_saturday runs the saturday function alone, D-216 (b)). selfTestChild is that child (and the soak
// job's leak-pipeline check); it skips without ESSES_SOAK_SELFTEST. Environment: ESSES_SOAK_SECONDS,
// ESSES_SOAK_MIN_CYCLES, ESSES_SOAK_SCALE (session length, default 1), ESSES_SOAK_SETTLE_SECONDS,
// ESSES_SOAK_LOG_DIR (writes saturday-cycles.csv and saturday-summary.txt there), ESSES_SOAK_CRT_CHECK (MSVC
// debug runtime: a full heap check every n allocations, and _CrtCheckMemory after every cycle),
// ESSES_SOAK_SELFTEST (the child's case).

#include "soak_data.hpp"
#include "soak_driver.hpp"
#include "soak_guard.hpp"
#include "soak_metrics.hpp"
#include "ui_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

using namespace esses;

namespace {

double envNumber(const char* name, double fallback) {
    bool ok = false;
    const double v = qEnvironmentVariable(name).toDouble(&ok);
    return ok && std::isfinite(v) && v >= 0.0 ? v : fallback;
}

double soakSeconds() {
    return envNumber("ESSES_SOAK_SECONDS", 60.0);
}

std::unique_ptr<qt::AppRuntime> runtime(unsigned workers) {
    qt::AppRuntimeOptions o;
    o.workers = workers;
    o.markGuiThreadNoBlock = true; // as esses (01 section 5.3 rule 4)
    return std::make_unique<qt::AppRuntime>(o);
}

std::vector<double> series(const std::vector<soak::CycleSample>& samples, bool skipRestarted,
                           double (*value)(const soak::CycleSample&)) {
    std::vector<double> out;
    for (const soak::CycleSample& s : samples) {
        if (!(skipRestarted && s.restarted)) {
            out.push_back(value(s));
        }
    }
    return out;
}

} // namespace

class TstSaturday : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        uitest::prepareFontEnvironment();
        // Qt Test's watchdog (QTEST_FUNCTION_TIMEOUT, 5 minutes by default) must outlast the soak function.
        const double needMs =
            (soakSeconds() + envNumber("ESSES_SOAK_SETTLE_SECONDS", 900.0) * 4.0 + 3600.0) * 1000.0;
        const double have = qEnvironmentVariableIntValue("QTEST_FUNCTION_TIMEOUT");
        if (have < needMs) {
            const double capped = std::min(needMs, 2.0e9);
            qputenv("QTEST_FUNCTION_TIMEOUT", QByteArray::number(static_cast<qint64>(capped)));
        }
    }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        widgets::registerBuiltinComponents();
        soak::Guard::install(soak::Guard::Mode::Fatal);
        const int crt = qEnvironmentVariableIntValue("ESSES_SOAK_CRT_CHECK");
        if (crt > 0) {
            qInfo("debug CRT heap checks every %d allocations: %s", crt,
                  soak::Guard::enableCrtHeapChecks(crt) ? "on" : "not available in this build");
        }
    }

    // The growth rule (soak_metrics.hpp) on series worked by hand.
    void growthRule() {
        using soak::judgeGrowth;
        // Noise: the late minimum (99) stays under the early maximum (104) plus 10.
        const soak::GrowthVerdict noise =
            judgeGrowth({100, 103, 99, 101, 104, 100, 102, 98, 101, 103, 99, 100}, 2, 10.0, 0.0);
        QVERIFY(noise.judged);
        QCOMPARE(noise.early, 104.0);
        QCOMPARE(noise.late, 99.0);
        QVERIFY(!noise.grew);
        // A steady leak of 10 per cycle: early max(140, 150, 160)... first third after the warm-up of 2 is
        // samples 2-4 (120, 130, 140), last third 9-11 (190, 200, 210): 190 > 140 + 20.
        std::vector<double> leak;
        for (int i = 0; i < 12; ++i) {
            leak.push_back(100.0 + 10.0 * i);
        }
        const soak::GrowthVerdict steady = judgeGrowth(leak, 2, 20.0, 0.0);
        QVERIFY(steady.judged);
        QCOMPARE(steady.early, 140.0);
        QCOMPARE(steady.late, 190.0);
        QVERIFY(steady.grew);
        // A cache that fills in the warm-up and stays: no growth.
        QVERIFY(!judgeGrowth({10, 60, 100, 101, 100, 102, 101, 100, 101, 102}, 2, 1.0, 0.0).grew);
        // One late spike: the late minimum ignores it.
        QVERIFY(!judgeGrowth({100, 100, 100, 100, 100, 100, 100, 100, 500}, 2, 1.0, 0.0).grew);
        // Too few samples: not judged.
        QVERIFY(!judgeGrowth({1, 2, 3, 4}, 2, 0.0, 0.0).judged);
        // The relative slack: 1090 is within 1000 + 10 % but not within 1000 + 5 %.
        const std::vector<double> rel{1000, 1000, 1000, 1000, 1000, 1080, 1090, 1095};
        QVERIFY(!judgeGrowth(rel, 0, 10.0, 0.10).grew);
        QVERIFY(judgeGrowth(rel, 0, 10.0, 0.05).grew);
    }

    // The Qt messages the guard treats as thread violations: Qt's thread-affinity warnings, and nothing else.
    void threadViolationMessages() {
        using soak::Guard;
        const QStringList violations{
            QStringLiteral(
                "QObject: Cannot create children for a parent that is in a different thread.\n(Parent "
                "is QWidget(0x1), parent's thread is QThread(0x2), current thread is QThread(0x3)"),
            QStringLiteral("QObject::setParent: Cannot set parent, new parent is in a different thread"),
            QStringLiteral("QObject::startTimer: Timers cannot be started from another thread"),
            QStringLiteral("QObject::killTimer: Timers cannot be stopped from another thread"),
            QStringLiteral(
                "QBasicTimer::start: QBasicTimer can only be used with threads started with QThread"),
            QStringLiteral("QPixmap: It is not safe to use pixmaps outside the GUI thread"),
            QStringLiteral(
                "QObject::moveToThread: Current thread (0x1) is not the object's thread (0x2).\nCannot "
                "move to target thread (0x3)"),
            QStringLiteral(
                "QSocketNotifier: Socket notifiers cannot be enabled or disabled from another thread"),
            QStringLiteral(
                "QObject::connect: Cannot queue arguments of type 'Foo'\n(Make sure 'Foo' is registered "
                "using qRegisterMetaType().)"),
        };
        for (const QString& text : violations) {
            QVERIFY2(Guard::isThreadViolationMessage(QtWarningMsg, text), qPrintable(text));
        }
        QVERIFY(Guard::isThreadViolationMessage(
            QtFatalMsg,
            QStringLiteral("ASSERT failure in QCoreApplication::sendEvent: \"Cannot send events to "
                           "objects owned by a different thread.\"")));
        QVERIFY(!Guard::isThreadViolationMessage(QtWarningMsg,
                                                 QStringLiteral("QFont::fromString: Invalid description")));
        QVERIFY(!Guard::isThreadViolationMessage(
            QtWarningMsg,
            QStringLiteral("QWidget::setMinimumSize: (/QWidget) Negative sizes (-1,-1) are not possible")));
        QVERIFY(!Guard::isThreadViolationMessage(QtDebugMsg, QStringLiteral("work in a different thread")));
        QCOMPARE(QString::fromLatin1(Guard::assertClass("C:/x/libs/qtmodel/src/dispatcher.cpp", nullptr)),
                 QStringLiteral("dispatcher"));
        QCOMPARE(QString::fromLatin1(Guard::assertClass("/x/libs/base/src/taskpool.cpp",
                                                        "blocking Future wait on a NoBlock (GUI) thread")),
                 QStringLiteral("dispatcher"));
        QCOMPARE(
            QString::fromLatin1(Guard::assertClass("/x/libs/store/src/column.cpp", "an index out of range")),
            QStringLiteral("assert"));
    }

    // Each kind of violation reaches the guard (Record mode, in this process): Dispatcher::drain on a pool
    // worker, a blocking Future wait on the NoBlock GUI thread, a QObject parented across threads.
    void guardRecordsDispatcherViolations() {
        using soak::Guard;
        Guard::install(Guard::Mode::Record);
        Guard::reset();
        const auto back = qScopeGuard([] {
            Guard::setRecordHook({});
            Guard::install(Guard::Mode::Fatal);
            Guard::reset();
        });
        auto rt = runtime(2);
        QTRY_VERIFY(uitest::runtimeQuiet(*rt));
        int expected = 0;
#if ESSES_ASSERTS_ENABLED
        // 1. The Dispatcher drained off the GUI thread (its queue is empty: nothing runs there).
        std::atomic<bool> drained{false};
        rt->pool().run(Priority::Interactive, TaskKey{}, CancelToken{}, [&rt, &drained](CancelToken) {
            rt->dispatcher().drain();
            drained.store(true);
        });
        QTRY_VERIFY(drained.load());
        ++expected;
        QCOMPARE(Guard::violations(), expected);
        // 2. A blocking wait on the GUI thread: the task waits until the assertion's hook lets it finish, so
        // the Future is not ready when get() starts, whatever the scheduling.
        std::atomic<bool> release{false};
        Guard::setRecordHook([&release] { release.store(true); });
        Future<int> f =
            rt->pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&release](CancelToken) {
                while (!release.load()) {
                    std::this_thread::yield();
                }
                return 7;
            });
        const Result<int>& r = f.get(); // the NoBlock assertion fires here, then the wait completes
        QVERIFY(r.ok() && r.value() == 7);
        Guard::setRecordHook({});
        ++expected;
        QCOMPARE(Guard::violations(), expected);
#endif
        // 3. Qt's thread-affinity warning: a QObject made on a pool worker with a GUI-thread parent.
        QObject guiObject;
        std::atomic<bool> made{false};
        rt->pool().run(Priority::Interactive, TaskKey{}, CancelToken{}, [&guiObject, &made](CancelToken) {
            const QObject orphan(&guiObject); // Qt refuses the parent and warns
            made.store(true);
        });
        QTRY_VERIFY(made.load());
        ++expected;
        QCOMPARE(Guard::violations(), expected);
        const std::vector<std::string> seen = Guard::recorded();
        QCOMPARE(static_cast<int>(seen.size()), expected);
#if ESSES_ASSERTS_ENABLED
        QVERIFY2(seen[0].rfind("dispatcher: ", 0) == 0 && seen[0].find("dispatcher.cpp") != std::string::npos,
                 seen[0].c_str());
        QVERIFY2(seen[1].rfind("dispatcher: ", 0) == 0 && seen[1].find("NoBlock") != std::string::npos,
                 seen[1].c_str());
#endif
        QVERIFY2(seen.back().rfind("qt-thread: ", 0) == 0, seen.back().c_str());
        rt->shutdown();
    }

    // Fatal mode stops the process at once with the violation's text, in a child process of this executable.
    void violationsAreFatal() {
        QStringList kinds{QStringLiteral("qt-thread")};
#if ESSES_ASSERTS_ENABLED
        kinds.prepend(QStringLiteral("dispatcher"));
#endif
        for (const QString& kind : kinds) {
            QProcess child;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("ESSES_SOAK_SELFTEST"), kind);
            // Qt Test's crash handler would attach a debugger for a backtrace (and the system a core dump):
            // the abort is expected, its text is the evidence.
            env.insert(QStringLiteral("QTEST_DISABLE_STACK_DUMP"), QStringLiteral("1"));
            env.insert(QStringLiteral("QTEST_DISABLE_CORE_DUMP"), QStringLiteral("1"));
            // A sanitizer build: the child's abort is expected; keep its report out of the soak's log files.
            for (const char* var : {"ASAN_OPTIONS", "UBSAN_OPTIONS"}) {
                const QString name = QString::fromLatin1(var);
                QString value = env.value(name);
                value += value.isEmpty() ? QString() : QStringLiteral(":");
                value += QStringLiteral("log_path=stderr");
                if (name == QLatin1String("ASAN_OPTIONS")) {
                    value += QStringLiteral(":handle_abort=0:detect_leaks=0");
                }
                env.insert(name, value);
            }
            child.setProcessEnvironment(env);
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("selfTestChild")});
            QVERIFY(child.waitForStarted(60'000));
            QVERIFY2(child.waitForFinished(600'000), "the child did not stop");
            const QByteArray err = child.readAllStandardError();
            const QByteArray out = child.readAllStandardOutput();
            QVERIFY2(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0, out.constData());
            const QByteArray want = "soak: fatal " + kind.toLatin1() + ": ";
            QVERIFY2(err.contains(want), (err + out).constData());
        }
    }

    void saturday() {
        const double seconds = soakSeconds();
        const int minCycles = static_cast<int>(envNumber("ESSES_SOAK_MIN_CYCLES", 2.0));
        const double scale = std::clamp(envNumber("ESSES_SOAK_SCALE", 1.0), 0.05, 20.0);
        const auto settleMs =
            static_cast<std::size_t>(envNumber("ESSES_SOAK_SETTLE_SECONDS", 900.0) * 1000.0);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const std::filesystem::path base(root.path().toStdWString());

        // The day's logs, before the runtime exists (the GUI thread may block here): written once per build
        // tree
        // (<build>/tests/soak/saturday-logs-v<n>-x<scale>) and copied for this run, which replaces files in
        // them.
        std::vector<soak::LogFile> logs;
        {
            QElapsedTimer clock;
            clock.start();
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            Result<std::vector<soak::LogFile>> written =
                soak::saturdayLogs(pathFromUtf8(ESSES_TEST_BINARY_DIR), base / "logs", scale);
            QVERIFY2(written.ok(), written.ok() ? "" : written.error().toString().c_str());
            logs = std::move(written).value();
            std::uintmax_t bytes = 0;
            for (const soak::LogFile& log : logs) {
                bytes += log.bytes;
            }
            qInfo("soak: %d logs, %.1f MB on disk, ready in %.1f s", static_cast<int>(logs.size()),
                  static_cast<double>(bytes) / 1e6, static_cast<double>(clock.elapsed()) / 1000.0);
        }
        QCOMPARE(logs.size(), std::size_t{10});

        auto rt = runtime(0); // as esses: hardware threads - 1, the default cache budget
        soak::DriverOptions options;
        options.configDir = base / "config";
        options.projectDir = base / "project";
        options.settleTimeoutMs = settleMs;
        std::filesystem::create_directories(options.configDir);
        std::vector<soak::CycleSample> samples;
        std::string coverage;
        int cycles = 0;
        {
            soak::SaturdayDriver day(*rt, logs, options);
            day.start();
            QVERIFY(!QTest::currentTestFailed());
            QElapsedTimer clock;
            clock.start();
            while (cycles < minCycles || static_cast<double>(clock.elapsed()) < seconds * 1000.0) {
                day.cycle(cycles);
                if (QTest::currentTestFailed()) {
                    return;
                }
                QVERIFY2(soak::Guard::crtHeapConsistent(), "the debug CRT found the heap corrupted");
                ++cycles;
            }
            day.finish();
            if (QTest::currentTestFailed()) {
                return;
            }
            samples = day.samples();
            coverage = day.coverage();
        }
        rt->shutdown();
        rt.reset();

        // The growth rules over the per-cycle samples (soak_metrics.hpp); the warm-up is a fifth of the run,
        // at least two cycles (the caches of the first opens, the dialogs made on first use).
        const auto judge = [](const std::vector<double>& s, double absSlack, double relSlack) {
            return soak::judgeGrowth(s, std::max<std::size_t>(2, s.size() / 5), absSlack, relSlack);
        };
        struct Check {
            const char* name;
            soak::GrowthVerdict verdict;
        };
        constexpr double kMiB = 1048576.0;
        const std::vector<Check> checks{
            {"heap bytes",
             judge(series(samples, true,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.heapBytes); }),
                   48.0 * kMiB, 0.10)},
            {"resident bytes",
             judge(series(samples, true,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.residentBytes); }),
                   192.0 * kMiB, 0.25)},
            {"chunk cache bytes", judge(series(samples, false,
                                               [](const soak::CycleSample& s) {
                                                   return static_cast<double>(s.cacheResidentBytes);
                                               }),
                                        64.0 * kMiB, 0.10)},
            {"widgets",
             judge(series(samples, true,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.widgets); }),
                   40.0, 0.05)},
            {"window objects",
             judge(series(samples, true,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.windowObjects); }),
                   80.0, 0.05)},
            {"peak queued tasks",
             judge(series(samples, false,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.poolPeak); }),
                   2000.0, 2.0)},
            {"peak Dispatcher closures",
             judge(series(samples, false,
                          [](const soak::CycleSample& s) { return static_cast<double>(s.dispatcherPeak); }),
                   2000.0, 2.0)},
        };
        QString summary = QStringLiteral("soak: %1 cycles in %2 s; steps: %3\n")
                              .arg(cycles)
                              .arg(samples.empty() ? 0.0 : samples.back().elapsedS, 0, 'f', 0)
                              .arg(QString::fromStdString(coverage));
        for (const Check& c : checks) {
            summary += QString::fromStdString(soak::describe(c.name, c.verdict)) + QLatin1Char('\n');
        }
        qInfo("%s", qPrintable(summary));
        if (const QString dir = qEnvironmentVariable("ESSES_SOAK_LOG_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            QFile csv(dir + QStringLiteral("/saturday-cycles.csv"));
            if (csv.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                csv.write(
                    "cycle,elapsed_s,cycle_s,restarted,resident_bytes,heap_bytes,cache_bytes,cache_chunks,"
                    "peak_tasks,peak_dispatcher,widgets,window_objects,sources,undo_steps\n");
                for (const soak::CycleSample& s : samples) {
                    csv.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14\n")
                                  .arg(s.cycle)
                                  .arg(s.elapsedS, 0, 'f', 1)
                                  .arg(s.cycleS, 0, 'f', 1)
                                  .arg(s.restarted ? 1 : 0)
                                  .arg(s.residentBytes)
                                  .arg(s.heapBytes)
                                  .arg(s.cacheResidentBytes)
                                  .arg(s.cacheChunks)
                                  .arg(s.poolPeak)
                                  .arg(s.dispatcherPeak)
                                  .arg(s.widgets)
                                  .arg(s.windowObjects)
                                  .arg(s.sources)
                                  .arg(s.undoSteps)
                                  .toUtf8());
                }
            }
            QFile text(dir + QStringLiteral("/saturday-summary.txt"));
            if (text.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                text.write(summary.toUtf8());
            }
        }
        for (const Check& c : checks) {
            QVERIFY2(!c.verdict.grew, soak::describe(c.name, c.verdict).c_str());
        }
    }

    // The child of violationsAreFatal (ESSES_SOAK_SELFTEST=dispatcher or qt-thread) and the soak job's check
    // of its leak pipeline (ESSES_SOAK_SELFTEST=leak); skipped otherwise.
    void selfTestChild() {
        const QByteArray what = qgetenv("ESSES_SOAK_SELFTEST");
        if (what.isEmpty()) {
            QSKIP("only as a child of violationsAreFatal or in the soak job (ESSES_SOAK_SELFTEST)");
        }
        soak::Guard::install(soak::Guard::Mode::Fatal);
        auto rt = runtime(2);
        QTRY_VERIFY(uitest::runtimeQuiet(*rt));
        if (what == "leak") {
            QObject context;
            bool ran = false;
            QTimer::singleShot(0, &context, [&ran] {
                soak::leakOnPurpose();
                ran = true;
            });
            QTRY_VERIFY(ran);
            rt->shutdown();
            return; // LeakSanitizer reports it at exit
        }
        QObject guiObject;
        if (what == "dispatcher") {
            rt->pool().run(Priority::Interactive, TaskKey{}, CancelToken{},
                           [&rt](CancelToken) { rt->dispatcher().drain(); });
        } else if (what == "qt-thread") {
            rt->pool().run(Priority::Interactive, TaskKey{}, CancelToken{},
                           [&guiObject](CancelToken) { const QObject orphan(&guiObject); });
        } else {
            QFAIL(
                qPrintable(QStringLiteral("unknown ESSES_SOAK_SELFTEST %1").arg(QString::fromLatin1(what))));
        }
        // The violation ends the process; the end of this bound means it did not.
        static_cast<void>(QTest::qWaitFor([] { return false; }, 30'000));
        QFAIL("the violation did not stop the process");
    }
};

QTEST_MAIN(TstSaturday)
#include "tst_saturday.moc"
