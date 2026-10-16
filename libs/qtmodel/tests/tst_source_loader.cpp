// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader (ARCHITECTURE_OUTLINE section 2.1, 01 section 4.1; AC-M1-01, AC-M1-10, AC-M1-12): a .ld + .ldx
// written by the motec.ld writer opens on the TaskPool, publishes through the Dispatcher with its laps from
// the circuit rules, registers the built-in maths (Corr Speed / Corr Dist prefetched without a request) and
// the distance indexes, runs the phase-2 annotations, reports its Import Log and failures to Messages,
// commits sidecar edits (trust) to disk, closes cleanly, and never blocks a NoBlock (GUI) thread. The public
// ACC stand-in fixture opens (with distance data) when its generated input is present.

#include "qtmodel_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/registry.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)
Q_DECLARE_METATYPE(esses::model::SourceId)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)

namespace {

std::atomic<int> gAsserts{0};
void recordingHandler(const char*, const char*, int, const char*) {
    gAsserts.fetch_add(1);
}

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

// The application wiring of one workspace (06 section 1): loader -> maths, distance, selection, messages.
struct Workspace {
    AppRuntime& rt;
    MathsService maths;
    DistanceService distance;
    DataSelectionModel selection;
    MessagesModel messages;
    SourceLoader loader;

    explicit Workspace(AppRuntime& runtime)
        : rt(runtime), maths(rt.pool(), rt.dispatcher(), shippedUnits()), distance(maths, rt.dispatcher()),
          loader(rt.pool(), rt.dispatcher(), shippedUnits()) {
        static_cast<void>(maths.loadSettingsSchema(repoRoot() / "data"));
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
        connectSourceLoader(loader, selection, &messages);
    }
};

std::filesystem::path writeSynthetic(const std::string& dirName, store::ChunkCache& cache) {
    SyntheticOptions o;
    o.path = "written.ld";
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    const auto src = makeSource(o, cache);
    return writeLd(*src, scratchDir(dirName), "session");
}

// Every task of the runtime's pool has completed (their results may still wait in the Dispatcher).
bool poolIdle(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0;
}

// Every task of the runtime's pool has completed and every result it posted has been delivered to this
// thread: what the tests wait for instead of a fixed time, which a loaded machine can always outlast.
bool quiet(AppRuntime& rt) {
    return poolIdle(rt) && rt.dispatcher().pending() == 0;
}

// Holds every worker of `pool` until release(), so that a task submitted meanwhile cannot have finished when
// its submitter looks at the Future.
class WorkerHold {
public:
    explicit WorkerHold(TaskPool& pool) : state_(std::make_shared<State>(pool.stats().workers)) {
        for (std::size_t i = 0; i < pool.stats().workers; ++i) {
            pool.run(Priority::Interactive, TaskKey{}, CancelToken{}, [state = state_](const CancelToken&) {
                state->started.count_down();
                state->release.wait();
            });
        }
        state_->started.wait();
    }
    ~WorkerHold() { release(); }
    WorkerHold(const WorkerHold&) = delete;
    WorkerHold& operator=(const WorkerHold&) = delete;
    void release() {
        if (!released_) {
            released_ = true;
            state_->promise.set_value();
        }
    }

private:
    struct State {
        explicit State(std::size_t workers)
            : started(static_cast<std::ptrdiff_t>(workers)), release(promise.get_future().share()) {}
        std::latch started;
        std::promise<void> promise;
        std::shared_future<void> release;
    };
    std::shared_ptr<State> state_;
    bool released_ = false;
};

std::filesystem::path accFixture() {
    const std::filesystem::path rel =
        std::filesystem::path("motec.ld") / "synthetic-acc-dialect" / "input.ld";
    for (const std::filesystem::path& base :
         {pathFromUtf8(ESSES_TEST_BINARY_DIR) / ".." / ".." / "fixture-inputs", repoRoot() / "fixtures"}) {
        std::error_code ec;
        if (std::filesystem::exists(base / rel, ec)) {
            return base / rel;
        }
    }
    return {};
}

} // namespace

class TstSourceLoader : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
    }

    void opensPublishesAndPrefetches() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("open", rt.cache());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        QSignalSpy replaced(&w.loader, &SourceLoader::sourceReplaced);
        w.loader.open(ld);
        QCOMPARE(w.loader.pendingOpens(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QCOMPARE(w.loader.pendingOpens(), 0);
        QCOMPARE(logs.count(), 1);
        const auto src = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
        // The loader holds the published snapshot or the newest replacement of it. The publication prefetches
        // Corr Speed, and phase 2 (stint / pitStop, below) then replaces the snapshot; without the slow MSVC
        // debug runtime (Clang, GCC) that whole chain can complete within the wait slice in which the
        // publication arrived, before this line runs.
        if (replaced.isEmpty()) {
            QCOMPARE(w.loader.source(src->id), src);
        } else {
            const auto newest = replaced.last().at(0).value<std::shared_ptr<const model::DataSource>>();
            QCOMPARE(w.loader.source(src->id), newest);
            QCOMPARE(newest->id, src->id);
            QVERIFY(newest->version > src->version);
        }
        QCOMPARE(w.selection.sources().size(), std::size_t{1});
        // The beacons came back from the .ldx: an out lap, three flying laps and an in lap.
        QCOMPARE(rangeCount(*src), std::size_t{5});
        QCOMPARE(rangeAt(*src, 1).lapClass, model::LapClass::Flying);
        QVERIFY(w.selection.hasMain());
        // AC-M1-10: Corr Speed / Corr Dist registered and evaluated without a request; every lap indexed.
        QVERIFY(w.maths.isMathsChannel(src->id, "Normal", "Corr Dist"));
        QTRY_COMPARE_WITH_TIMEOUT(w.distance.state(src->id), DistanceState::Ready, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            w.distance.chain(rangeRef(*w.loader.source(src->id), 2), Priority::Interactive) != nullptr &&
                w.distance.chain(rangeRef(*w.loader.source(src->id), 2), Priority::Interactive)->complete(),
            30000);
        // Phase 2 (stint / pitStop) annotated the laps once Corr Speed was evaluated: stint 1 everywhere.
        QTRY_COMPARE_WITH_TIMEOUT(rangeAt(*w.loader.source(src->id), 1).stint, std::uint16_t{1}, 30000);
        QCOMPARE(w.selection.source(src->id), w.loader.source(src->id));
        QVERIFY(!rangeAt(*w.loader.source(src->id), 2).pitStop);
        // CTRL+F on the opened file: the fastest trusted flying lap (Lap 2, 48 s).
        w.selection.selectFastest();
        QCOMPARE(w.selection.snapshot().main, rangeRef(*w.loader.source(src->id), 2));
    }

    // 02 section 4.4 rule 13: a sidecar commit (trust override) that publishes a newer snapshot while the
    // phase-2 run is on a worker neither loses the annotations nor the override. The run's result belongs to
    // an older snapshot with the same marker set: its stint/pitStop are merged by (range id, start, end) into
    // the newest snapshot instead of being dropped (or replacing that snapshot's tree).
    void phase2SurvivesACommitWhileItRuns() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("phase2-commit", rt.cache());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        bool committed = false;
        model::RangeRef lap1;
        const std::string corrSpeed = caseFold("Corr Speed");
        // Connected after the loader's own handler (Workspace), so phase 2 has just been submitted when this
        // runs; its result can only be delivered after this handler returns (it is posted to this thread).
        QObject::connect(&w.maths, &MathsService::channelReady, &w.maths, [&](const maths::ChannelKey& key) {
            if (committed || key.nameCf != corrSpeed || w.loader.sources().empty()) {
                return;
            }
            const auto src = w.loader.sources().front();
            QCOMPARE(rangeAt(*src, 1).stint, std::uint16_t{0}); // not annotated yet
            lap1 = rangeRef(*src, 1);
            committed = true;
            w.selection.setTrusted(lap1, false);
        });
        w.loader.open(ld);
        QTRY_VERIFY_WITH_TIMEOUT(committed, 30000);
        const model::SourceId id = lap1.src;
        QVERIFY(!rangeAt(*w.loader.source(id), 1).trusted);
        // The annotations land on the committed snapshot (stint 1 on every lap) and the override survives.
        QTRY_COMPARE_WITH_TIMEOUT(rangeAt(*w.loader.source(id), 1).stint, std::uint16_t{1}, 30000);
        QCOMPARE(rangeAt(*w.loader.source(id), 3).stint, std::uint16_t{1});
        QVERIFY(!rangeAt(*w.loader.source(id), 1).trusted);
        QVERIFY(!w.selection.resolve(lap1)->trusted);
        QCOMPARE(w.selection.source(id), w.loader.source(id));
    }

    // No channelReady() of a new source before its sourcePublished(). MathsService::require applies an
    // evaluation that has already finished at once and announces channelReady() on the spot; the loader's
    // distance prefetch requires Corr Dist, so when a worker had finished Corr Speed and Corr Dist before the
    // prefetch, both were announced inside the publication, and a listener (the trust edit of
    // phase2SurvivesACommitWhileItRuns, on Windows CI) met a source the selection did not hold yet. The
    // window is forced here: when the built-ins are registered, Corr Dist is required and this thread waits
    // until the workers have evaluated it, so it is finished but not applied when the loader reaches the
    // prefetch.
    void noChannelIsAnnouncedBeforeItsSource() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("announce-order", rt.cache());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        const std::string corrDist = caseFold("Corr Dist");
        bool forced = false;
        QObject::connect(&w.maths, &MathsService::statusChanged, &w.maths, [&](const maths::ChannelKey& key) {
            if (forced || key.nameCf != corrDist) {
                return;
            }
            forced = true;
            {
                WorkerHold hold(rt.pool()); // so that this require cannot apply (and announce) at once itself
                static_cast<void>(w.maths.require(key, Priority::Visible));
            }
            // This thread is not NoBlock here; the results wait in the Dispatcher.
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!poolIdle(rt) && std::chrono::steady_clock::now() < until) {
                std::this_thread::yield();
            }
        });
        std::vector<std::pair<std::string, bool>> announced; // the channel, and whether its source was known
        QObject::connect(&w.maths, &MathsService::channelReady, &w.maths, [&](const maths::ChannelKey& key) {
            announced.emplace_back(key.nameCf,
                                   published.count() == 1 && w.selection.source(key.src) != nullptr);
        });
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QVERIFY(forced);
        const auto known = [&announced](const std::string& name) {
            return std::any_of(announced.begin(), announced.end(),
                               [&](const auto& a) { return a.first == name; });
        };
        QTRY_VERIFY_WITH_TIMEOUT(known(corrDist) && known(caseFold("Corr Speed")), 30000);
        for (const auto& [name, sourceKnown] : announced) {
            QVERIFY2(sourceKnown, ("channelReady('" + name + "') before its source was published").c_str());
        }
    }

    void secondOpenOfTheSameFile() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("twice", rt.cache());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        const int rows = w.messages.rowCount();
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(w.loader.pendingOpens(), 0, 30000);
        QCOMPARE(published.count(), 1);
        QCOMPARE(w.loader.sources().size(), std::size_t{1});
        QCOMPARE(w.messages.rowCount(), rows + 1);
        QCOMPARE(w.messages.at(w.messages.rowCount() - 1).code, QStringLiteral("open.already_open"));
    }

    void failuresReachMessages() {
        AppRuntime rt(runtimeOptions());
        Workspace w(rt);
        QSignalSpy failed(&w.loader, &SourceLoader::openFailed);
        QSignalSpy firstError(&w.messages, &MessagesModel::firstError);
        w.loader.open(scratchDir("missing") / "nothing-here.ld");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 30000);
        QCOMPARE(w.loader.pendingOpens(), 0);
        QCOMPARE(w.messages.count(Message::Severity::Error), 1);
        QCOMPARE(firstError.count(), 1);
        QVERIFY(w.loader.sources().empty());
    }

    // setTrusted -> commitSidecar: the snapshot is rebuilt, the sidecar is written next to the log, and a new
    // open reads it back (D-015 precedence).
    void sidecarEditsPersist() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("sidecar", rt.cache());
        model::SourceId id;
        {
            Workspace w(rt);
            QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
            QSignalSpy replaced(&w.loader, &SourceLoader::sourceReplaced);
            w.loader.open(ld);
            QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
            const auto src = w.loader.source(
                published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id);
            id = src->id;
            const model::RangeRef lap1 = rangeRef(*src, 1);
            QVERIFY(w.selection.resolve(lap1)->trusted);
            w.selection.setTrusted(lap1, false);
            QVERIFY(replaced.count() >= 1);
            QVERIFY(!w.selection.resolve(lap1)->trusted);
            QVERIFY(!rangeAt(*w.loader.source(id), 1).trusted);
            // Written by a Background task within 500 ms (D-190).
            QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(model::SidecarDocument::pathFor(ld)), 10000);
            QVERIFY(w.loader.commitSidecar(nullptr, {}).error().code == ErrorCode::NotFound);
            QSignalSpy closed(&w.loader, &SourceLoader::sourceClosed);
            w.loader.close(id);
            QCOMPARE(closed.count(), 1);
            QVERIFY(w.selection.sources().empty());
            QVERIFY(!w.maths.isMathsChannel(id, "Normal", "Corr Dist"));
            QCOMPARE(w.distance.state(id), DistanceState::Unknown);
        }
        Workspace again(rt);
        QSignalSpy published(&again.loader, &SourceLoader::sourcePublished);
        again.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QVERIFY(!rangeAt(*again.loader.source(id), 1).trusted); // read back from the sidecar
    }

    // D-190 (02 section 4 sidecar rule 4): an edit never writes the sidecar on the GUI thread; the edits of
    // 500 ms coalesce into one Background write of the newest document; a closing source writes its last
    // edit at once.
    void sidecarWritesAreCoalesced() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("sidecar-coalesce", rt.cache());
        const std::filesystem::path sidecar = model::SidecarDocument::pathFor(ld);
        Workspace w(rt);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        const model::SourceId id =
            published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id;
        const model::RangeRef lap1 = rangeRef(*w.loader.source(id), 1);
        w.selection.setTrusted(lap1, false);
        QVERIFY(!std::filesystem::exists(sidecar)); // nothing written by the edit itself
        w.selection.setTrusted(lap1, true);
        w.selection.setTrusted(lap1, false);
        QVERIFY(!std::filesystem::exists(sidecar));
        // readWholeFile shares delete access on Windows: a std::ifstream (which does not) open at the instant
        // of a Background write would make that write's rename fail ("sidecar.write_failed"), and is itself
        // refused while the renaming handle is still open.
        const auto fileText = [&sidecar] { return readWholeFile(sidecar).valueOr(std::string()); };
        // The newest edit, awaited by content: the file can exist before the newest edit reaches it.
        QTRY_COMPARE_WITH_TIMEOUT(fileText(), *w.loader.source(id)->sidecar.toJson(), 10000);
        // The source closes before its next write is due: the edit is written on the way out.
        w.selection.setTrusted(lap1, true);
        const model::SidecarDocument last = w.loader.source(id)->sidecar;
        w.loader.close(id);
        if (last.empty()) {
            QVERIFY(!std::filesystem::exists(sidecar)); // an empty document deletes the file (D-069)
        } else {
            QCOMPARE(fileText(), *last.toJson());
        }
        Workspace again(rt);
        QSignalSpy reopened(&again.loader, &SourceLoader::sourcePublished);
        again.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(reopened.count(), 1, 30000);
        QVERIFY(rangeAt(*again.loader.source(id), 1).trusted); // the last edit, read back
    }

    // D-190: closeAll() cancels the opens in flight; a result that arrives afterwards is dropped.
    void closeAllCancelsOpensInFlight() {
        AppRuntime rt(runtimeOptions());
        const std::filesystem::path ld = writeSynthetic("cancel-open", rt.cache());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy failed(&w.loader, &SourceLoader::openFailed);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        w.loader.open(ld);
        QCOMPARE(w.loader.pendingOpens(), 1);
        // The result can only reach this thread through its event loop, which has not run yet.
        w.loader.closeAll();
        QCOMPARE(w.loader.pendingOpens(), 0);
        QCOMPARE(logs.count(), 1);
        QVERIFY(logs.first().at(1).value<std::vector<formats::ImportLogEntry>>().at(0).code ==
                "open.cancelled");
        // The cancelled open has finished on its worker and its result has reached this thread (to be
        // dropped), however long it takes: after a fixed wait a slow open could still deliver afterwards.
        QTRY_VERIFY_WITH_TIMEOUT(quiet(rt), 30000);
        QCOMPARE(published.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(w.loader.sources().empty());
        // The loader opens again afterwards.
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
    }

    // AC-M1-12: the open pipeline, publication, maths and distance work on a NoBlock thread without a wait.
    void theGuiThreadNeverBlocks() {
#if ESSES_ASSERTS_ENABLED
        std::filesystem::path ld;
        {
            store::ChunkCache cache(std::size_t{64}
                                    << 20); // the writer reads columns: not on a NoBlock thread
            ld = writeSynthetic("noblock", cache);
        }
        const AssertHandler previous = setAssertHandler(&recordingHandler);
        gAsserts.store(0);
        {
            AppRuntimeOptions o = runtimeOptions();
            o.markGuiThreadNoBlock = true;
            AppRuntime rt(o);
            Workspace w(rt);
            w.loader.setUseSidecars(false);
            QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
            w.loader.open(ld);
            QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
            const auto id = published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance.state(id), DistanceState::Ready, 30000);
            w.loader.closeAll();
        }
        ThreadRole::set(ThreadRole::Normal);
        setAssertHandler(previous);
        QCOMPARE(gAsserts.load(), 0);
#else
        QSKIP("assertions are compiled out in this configuration");
#endif
    }

    // The public stand-in of the M1 data path (fixtures/motec.ld/synthetic-acc-dialect): out lap, 12 flying
    // laps, in lap; CTRL+F picks a trusted flying lap.
    void accDialectFixtureOpens() {
        const std::filesystem::path ld = accFixture();
        if (ld.empty()) {
            QSKIP("synthetic-acc-dialect/input.ld is not generated (esses-gen-fixtures --inputs-only)");
        }
        AppRuntime rt(runtimeOptions());
        Workspace w(rt);
        w.loader.setUseSidecars(false);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        w.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 60000);
        const auto src = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
        QCOMPARE(rangeCount(*src), std::size_t{14});
        QCOMPARE(rangeAt(*src, 0).lapClass, model::LapClass::Out);
        QCOMPARE(rangeAt(*src, 13).lapClass, model::LapClass::In);
        w.selection.selectFastest();
        const model::Range* main = w.selection.resolve(w.selection.snapshot().main);
        QVERIFY(main != nullptr);
        QCOMPARE(main->lapClass, model::LapClass::Flying);
        QVERIFY(main->trusted);
        // Corr Speed comes from SPEED through the default alternate list ("Speed"), so the distance indexes
        // exist without any request (AC-M1-01's F9 step on this file).
        QTRY_VERIFY_WITH_TIMEOUT(w.distance.state(src->id) == DistanceState::Ready ||
                                     w.distance.state(src->id) == DistanceState::NoDistance,
                                 60000);
        QCOMPARE(w.distance.state(src->id), DistanceState::Ready);
    }
};

QTEST_GUILESS_MAIN(TstSourceLoader)
#include "tst_source_loader.moc"
