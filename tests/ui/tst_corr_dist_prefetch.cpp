// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-10 (07 section 4.2, 03 sections 6.0 and 9.1; D-016), offscreen, in the real shell:
//   * Corr Speed / Corr Dist are the outputs of the built-in module esses.corrected_speed_distance,
//   registered
//     for every published source; their columns are served by the maths PlanProvider (chunk keys
//     chunk/<src>/<layer>/maths/<name>), on the public ACC stand-in (Corr Speed from SPEED).
//   * They are prefetched in the Background band after open: a second log is opened while the first one is
//     Main, with every worker of the pool parked at the moment of its publication, so its module tasks are
//     seen waiting in the Background queue (TaskPool::queuedTasks: its maths/<src>/ tasks are queued at
//     Background and no task of the second source is queued at Interactive or Visible; a visible Main asks
//     for its own source at Visible, which raises those tasks, as designed). The first log's graph and the
//     Dispatcher are quiet before the second open, so no build of the first log interleaves.
//   * Distance mode is populated without user action: with no request from the test, the second log's
//     outputs evaluate and every lap index of its outing is built; made Main, F9 converts through its chain
//     at once and the graph draws distance traces without the banner.
//   * The module's settings (the project's, the defaults and the shipped example) validate against
//     schemas/builtin-esses.corrected_speed_distance-1.schema.json and an invalid object is refused.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/model/ids.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr unsigned kWorkers = 2;

model::OpaqueJson readJson(const std::filesystem::path& p) {
    QFile f(QString::fromStdWString(p.wstring()));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    auto doc = model::OpaqueJson::parse(f.readAll().toStdString());
    return doc ? std::move(doc).value() : model::OpaqueJson{};
}

// Blocks the calling (GUI) thread without processing events: a posted publication stays queued.
bool spinUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool poolIdle(const TaskPool& pool) {
    const TaskPool::Stats s = pool.stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.running == 0 && s.parked == 0;
}

maths::ChannelKey keyOf(const model::DataSource& src, const char* name) {
    return maths::ChannelKey::make(src.id, model::kNormalLayer, name);
}

bool evaluated(qt::MathsService& m, const model::DataSource& src) {
    for (const char* name : {"Corr Speed", "Corr Dist"}) {
        const maths::MathState s = m.status(keyOf(src, name)).state;
        if (s != maths::MathState::Ok && s != maths::MathState::Warning) {
            return false;
        }
    }
    return true;
}

// Every lap of the source's outing has its index; asked at Background, so nothing is raised.
bool allLapsIndexed(MainWindow& w, const model::DataSource& src) {
    const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src.id);
    if (laps.empty()) {
        return false;
    }
    const auto chain = w.distance().chain(laps.front(), Priority::Background);
    return chain != nullptr && chain->complete();
}

} // namespace

class TstCorrDistPrefetch : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    // AC-M1-12: the GUI thread never blocked on a Future (no assertion recorded, any thread).
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() {
        registerBuiltinComponents();
        if (uitest::accFixtureInput().empty()) {
            QSKIP("synthetic-acc-dialect/input.ld is not generated (ctest fixtures.generate_inputs or "
                  "esses-gen-fixtures --inputs-only)");
        }
    }

    void prefetchedAtBackgroundAndPopulatesDistanceMode() {
        QTemporaryDir config;
        QTemporaryDir logs;
        auto rt = uitest::makeRuntime(kWorkers);
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.resize(1600, 1000);
            QVERIFY(uitest::showAndActivate(w));
            qt::MathsService& m = w.maths();

            // 1. The ACC stand-in (Main): the built-in module's outputs, served by the PlanProvider.
            w.openLog(uitest::accFixtureInput());
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kTimeoutMs);
            const auto acc = w.selection().sources().front();
            QCOMPARE(m.correctedSpeedModule().id(), std::string(maths::CorrectedSpeedDistanceModule::kId));
            QVERIFY(m.isMathsChannel(acc->id, model::kNormalLayer, "Corr Speed"));
            QVERIFY(m.isMathsChannel(acc->id, model::kNormalLayer, "Corr Dist"));
            QCOMPARE(m.outputs(acc->id, model::kNormalLayer).size(), std::size_t{2});
            QTRY_VERIFY_WITH_TIMEOUT(evaluated(m, *acc), kTimeoutMs);
            for (const char* name : {"Corr Speed", "Corr Dist"}) {
                const auto handle = m.require(keyOf(*acc, name), Priority::Visible);
                QVERIFY(handle.has_value());
                QCOMPARE(handle->descriptor.source, model::ChannelSource::Maths);
                const store::Column& col = store::seriesValues(handle->series);
                QVERIFY(col.size() > 0);
                const std::string chunkKey = col.chunkTaskKey(0).text;
                QVERIFY2(chunkKey.rfind("chunk/", 0) == 0 && chunkKey.find("/maths/") != std::string::npos,
                         chunkKey.c_str());
            }
            QTRY_VERIFY_WITH_TIMEOUT(allLapsIndexed(w, *acc), kTimeoutMs);
            // Quiet before the second open: the first log's graph has delivered its last build, nothing waits
            // in the Dispatcher (a distanceReady's queued static-build request included) and the pool is
            // idle.
            TimeDistanceGraph* firstGraph = uitest::graphOf(w);
            QVERIFY(firstGraph != nullptr);
            QVERIFY(uitest::waitQuiet(*firstGraph));
            QTRY_COMPARE_WITH_TIMEOUT(rt->dispatcher().pending(), std::size_t{0}, kTimeoutMs);
            QCoreApplication::processEvents();
            QTRY_VERIFY_WITH_TIMEOUT(poolIdle(rt->pool()), kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*firstGraph));

            // 2. A second log, not shown: its module tasks wait in the Background band at publication.
            qt::test::SyntheticOptions o;
            o.path = "second.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            std::filesystem::path second;
            {
                const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
                const auto mem = qt::test::makeSource(o, rt->cache());
                second = qt::test::writeLd(*mem, std::filesystem::path(logs.path().toStdWString()), "second");
            }
            TaskPool::Stats atPublish;
            std::vector<TaskPool::QueuedTask> queuedAtPublish;
            std::shared_ptr<const model::DataSource> src;
            maths::MathState speedAtPublish = maths::MathState::Ok;
            QObject::connect(&w.loader(), &qt::SourceLoader::sourcePublished,
                             [&](std::shared_ptr<const model::DataSource> s) {
                                 atPublish = rt->pool().stats();
                                 queuedAtPublish = rt->pool().queuedTasks("");
                                 src = std::move(s);
                                 speedAtPublish = m.status(keyOf(*src, "Corr Speed")).state;
                             });
            // Open, wait (without processing events) until the open task has finished and posted its result,
            // then park every worker so the publication's tasks stay in the band they were queued in.
            w.openLog(second);
            QVERIFY(spinUntil([&] { return poolIdle(rt->pool()) && rt->dispatcher().pending() >= 1; },
                              kTimeoutMs));
            std::atomic<bool> release{false};
            std::atomic<int> parked{0};
            struct Release { // frees the workers on every exit path, before the runtime joins them
                std::atomic<bool>& flag;
                ~Release() { flag.store(true); }
            } releaseOnExit{release};
            for (unsigned i = 0; i < kWorkers; ++i) {
                rt->pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                    parked.fetch_add(1);
                    while (!release.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    return 0;
                });
            }
            QVERIFY(spinUntil([&] { return parked.load() == static_cast<int>(kWorkers); }, kTimeoutMs));
            QTRY_VERIFY_WITH_TIMEOUT(src != nullptr, kTimeoutMs);
            QVERIFY(w.selection().snapshot().main.src == acc->id); // the new log is not shown
            const auto band = [&](Priority p) { return atPublish.queued[static_cast<std::size_t>(p)]; };
            qInfo("queued at the second publication: Interactive %zu, Visible %zu, Background %zu (workers "
                  "parked: %zu running)",
                  band(Priority::Interactive), band(Priority::Visible), band(Priority::Background),
                  atPublish.running);
            // The second source's own tasks, by key: its module evaluations wait at Background, and nothing
            // of it is queued above Background (the first log's builds, if any, are not counted).
            const std::string hex = src->id.hex();
            std::size_t moduleAtBackground = 0;
            for (const TaskPool::QueuedTask& t : queuedAtPublish) {
                if (t.key.find(hex) == std::string::npos) {
                    continue;
                }
                QVERIFY2(t.priority == Priority::Background, t.key.c_str());
                moduleAtBackground += t.key.rfind("maths/" + hex + "/", 0) == 0 ? 1 : 0;
            }
            QVERIFY2(moduleAtBackground >= 2, "Corr Speed and Corr Dist queued in the Background band");
            QVERIFY2(band(Priority::Background) >= 2, "the Background band holds the module tasks");
            QCOMPARE(band(Priority::Interactive), std::size_t{0});
            QVERIFY(speedAtPublish == maths::MathState::Pending ||
                    speedAtPublish == maths::MathState::Evaluating);
            QVERIFY(m.isMathsChannel(src->id, model::kNormalLayer, "Corr Dist"));
            release.store(true);

            // 3. No request: both outputs evaluate and every lap index of the outing is built.
            QTRY_VERIFY_WITH_TIMEOUT(evaluated(m, *src), kTimeoutMs);
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src->id), qt::DistanceState::Ready, kTimeoutMs);
            QTRY_VERIFY_WITH_TIMEOUT(allLapsIndexed(w, *src), kTimeoutMs);

            // 4. Made Main, F9 converts through its chain at once; the graph draws distance traces.
            const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src->id);
            QVERIFY(laps.size() >= 3);
            w.selection().setMain(laps[2]);
            TimeDistanceGraph* graph = uitest::graphOf(w);
            QVERIFY(graph != nullptr);
            int polylines = 0;
            graph->setBuildObserver([&polylines](const plot::ComponentBuild& b) {
                polylines = 0;
                for (const plot::DrawCmd& c : b.list.cmds) {
                    polylines += c.kind == plot::CmdKind::Polyline && c.vertexCount > 1 ? 1 : 0;
                }
            });
            w.canvas().focusComponent(graph);
            uitest::press(w, Qt::Key_W);
            uitest::press(w, Qt::Key_F9);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            QCOMPARE(ws.domain(), plot::Domain::Distance);
            QVERIFY(!ws.noDistance());
            QVERIFY(!w.workspace().zoomNormalised()); // through the chain, in metres
            const auto chain = w.distance().chain(laps[2], Priority::Background);
            const model::Range* r = w.selection().resolve(laps[2]);
            QVERIFY(chain != nullptr && r != nullptr);
            QVERIFY(std::abs(ws.state().zoom.b - chain->distAt(r->end)) < 1e-6);
            QVERIFY(uitest::waitQuiet(*graph));
            graph->setBuildObserver({});
            QVERIFY2(polylines >= 1, "Corr Speed drawn by distance");
            QCOMPARE(graph->inputs().view.domain, plot::Domain::Distance);
            QVERIFY(graph->inputs().slots.front().chain != nullptr);

            // 5. Settings against the module schema.
            QVERIFY(m.validateCorrectedSpeedSettings(w.session().correctedSpeedSettings()).ok());
            QVERIFY(m.validateCorrectedSpeedSettings(m.correctedSpeedModule().defaultSettings()).ok());
            const std::filesystem::path examples =
                uitest::repoRoot() / "schemas" / "examples" / "builtin-esses.corrected_speed_distance-1";
            QVERIFY(m.validateCorrectedSpeedSettings(readJson(examples / "valid" / "default.json")).ok());
            QVERIFY(
                !m.validateCorrectedSpeedSettings(*model::OpaqueJson::parse(R"({"method": "bogus"})")).ok());
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstCorrDistPrefetch)
#include "tst_corr_dist_prefetch.moc"
