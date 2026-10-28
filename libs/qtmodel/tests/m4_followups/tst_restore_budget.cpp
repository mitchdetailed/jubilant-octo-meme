// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SessionRestore and the memory budget gate (06 section 18 step 6, D-207 Consequences, docs/milestones/M4.md
// section 10 item 3): an open held at the budget warning has neither published nor failed, so the project's
// selection is restored only once it is answered. Continue restores the selection with the held file in it;
// Cancel restores it without that file. Before, the restore ran as soon as the other opens had landed and the
// held file's selection keys were dropped. The project holds two logs written here: "big" at 1 kHz (about
// 4.2 MB once loaded, tests/m4_budget/tst_memory_budget.cpp works the figure out) above the runtime's 2 MiB
// budget, "small" at 50 Hz (about 0.2 MB) within it; the selection saved with them is Main = lap 2 of big,
// Overlay 1 = lap 3 of small.

#include "../qtmodel_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/chunk_cache.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <memory>
#include <string>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(esses::qt::OpenBudgetCheck)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)
Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

namespace {

constexpr int kTimeoutMs = 60000;

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 2;
    o.cacheBudget = std::size_t{2} << 20; // 2 MiB: big is above it, small within it
    return o;
}

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

// Every task of the runtime's pool has completed and every result it posted has been delivered.
bool quiet(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0 && rt.dispatcher().pending() == 0;
}

// The synthetic log written through the motec.ld writer from a cache of its own.
std::filesystem::path writeLog(const std::filesystem::path& folder, const std::string& name,
                               unsigned rateHz) {
    const ThreadRole::Scope blocking(ThreadRole::Normal);
    store::ChunkCache scratch(std::size_t{256} << 20);
    SyntheticOptions o;
    o.path = name + ".ld";
    o.rateHz = rateHz;
    return writeLd(*makeSource(o, scratch), folder, name);
}

std::unique_ptr<SourceLoader> makeLoader(AppRuntime& rt, bool gate) {
    auto loader = std::make_unique<SourceLoader>(rt.pool(), rt.dispatcher(), shippedUnits());
    loader->setUseSidecars(false);
    loader->setRuleRunner(analysis::RuleRunner::circuitDefault());
    if (gate) {
        loader->setBudgetGate(&rt.cache());
    }
    return loader;
}

// The project of the header comment, saved with its two logs and the selection.
struct Saved {
    std::filesystem::path dir;
    std::filesystem::path folder;
    std::filesystem::path big;
    std::filesystem::path small;
    model::RangeRef bigLap2;
    model::RangeRef smallLap3;
};

void saveProject(AppRuntime& rt, const std::string& name, Saved& out) {
    out.dir = scratchDir(name);
    out.folder = out.dir / "Project";
    std::filesystem::create_directories(out.folder);
    out.big = writeLog(out.folder, "big", 1000);
    out.small = writeLog(out.folder, "small", 50);
    ProjectSession s(pathsFor(out.dir / "config"));
    QVERIFY(s.newProject(out.folder, "Restore").ok());
    std::unique_ptr<SourceLoader> loader = makeLoader(rt, false);
    DataSelectionModel selection(&s);
    connectSourceLoader(*loader, selection, nullptr);
    QSignalSpy published(loader.get(), &SourceLoader::sourcePublished);
    loader->open(out.big);
    loader->open(out.small);
    QTRY_COMPARE_WITH_TIMEOUT(published.count(), 2, kTimeoutMs);
    std::shared_ptr<const model::DataSource> big;
    std::shared_ptr<const model::DataSource> small;
    for (const QList<QVariant>& call : published) {
        const auto src = call.at(0).value<std::shared_ptr<const model::DataSource>>();
        (src->prov.path.filename() == out.big.filename() ? big : small) = src;
    }
    QVERIFY(big != nullptr && small != nullptr);
    s.addRecentSource(*big);
    s.addRecentSource(*small);
    out.bigLap2 = rangeRef(*big, 2);
    out.smallLap3 = rangeRef(*small, 3);
    selection.setMain(out.bigLap2);
    selection.toggleOverlay(out.smallLap3);
    QVERIFY(s.save().ok());
    loader->closeAll();
    QTRY_VERIFY_WITH_TIMEOUT(quiet(rt), kTimeoutMs);
}

// The project reopened with the gate on: the restore opens both logs, big waits at the budget warning.
struct Reopened {
    explicit Reopened(AppRuntime& runtime, const Saved& saved)
        : rt(runtime), session(pathsFor(saved.dir / "config")), loader(makeLoader(runtime, true)),
          selection(&session) {
        connectSourceLoader(*loader, selection, nullptr);
        exceeded = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::budgetExceeded);
        published = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::sourcePublished);
        logs = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::importLog);
        restore = std::make_unique<SessionRestore>(session, *loader, selection);
        finished = std::make_unique<QSignalSpy>(restore.get(), &SessionRestore::finished);
    }
    ~Reopened() {
        finished.reset();
        restore.reset();
        logs.reset();
        published.reset();
        exceeded.reset();
        loader->closeAll();
        loader.reset(); // its sources and the held one hold columns of the runtime's cache
    }
    Reopened(const Reopened&) = delete;
    Reopened& operator=(const Reopened&) = delete;

    AppRuntime& rt;
    ProjectSession session;
    std::unique_ptr<SourceLoader> loader;
    DataSelectionModel selection;
    std::unique_ptr<QSignalSpy> exceeded;
    std::unique_ptr<QSignalSpy> published;
    std::unique_ptr<QSignalSpy> logs;
    std::unique_ptr<SessionRestore> restore;
    std::unique_ptr<QSignalSpy> finished;
};

// Starts the restore and settles with big held and small published, then makes the restore check again
// whatever order the two opens landed in: a second open of small ends in its open.already_open row, a signal
// SessionRestore checks after. The check it scheduled has run once a timer queued after it has.
void startAndHold(Reopened& r, const Saved& saved) {
    QVERIFY(r.session.openProject(saved.folder).ok());
    QCOMPARE(r.restore->start(), 2);
    QVERIFY(r.restore->running());
    QTRY_VERIFY_WITH_TIMEOUT(r.exceeded->count() == 1 && r.published->count() == 1, kTimeoutMs);
    QCOMPARE(r.loader->pendingOpens(), 0);
    QCOMPARE(r.loader->pendingBudgetOpens(), 1);
    QCOMPARE(r.loader->sources().size(), std::size_t{1});
    QVERIFY(r.loader->sources().front()->prov.path.filename() == saved.small.filename());
    const qsizetype rows = r.logs->count();
    r.loader->open(saved.small);
    QTRY_VERIFY_WITH_TIMEOUT(r.logs->count() > rows && r.loader->pendingOpens() == 0, kTimeoutMs);
    bool turned = false;
    QTimer::singleShot(0, r.restore.get(), [&turned] { turned = true; });
    QTRY_VERIFY_WITH_TIMEOUT(turned, kTimeoutMs);
    QVERIFY(r.restore->running());
    QCOMPARE(r.finished->count(), 0);
}

} // namespace

class TstRestoreBudget : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<OpenBudgetCheck>("esses::qt::OpenBudgetCheck");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
    }

    void continueRestoresTheSelectionWithTheHeldFile() {
        AppRuntime rt(runtimeOptions());
        Saved saved;
        saveProject(rt, "restore-budget-continue", saved);
        if (QTest::currentTestFailed()) {
            return;
        }
        {
            Reopened r(rt, saved);
            startAndHold(r, saved);
            if (QTest::currentTestFailed()) {
                return;
            }
            r.loader->continueOverBudget(r.exceeded->at(0).at(0).toInt());
            QTRY_COMPARE_WITH_TIMEOUT(r.finished->count(), 1, kTimeoutMs);
            QVERIFY(!r.restore->running());
            QCOMPARE(r.loader->sources().size(), std::size_t{2});
            QVERIFY(r.selection.hasMain());
            QCOMPARE(r.selection.snapshot().main, saved.bigLap2);
            QCOMPARE(r.selection.snapshot().overlays.size(), std::size_t{1});
            QCOMPARE(r.selection.snapshot().overlays.front(), saved.smallLap3);
        }
        rt.shutdown();
    }

    void cancelRestoresTheSelectionWithoutIt() {
        AppRuntime rt(runtimeOptions());
        Saved saved;
        saveProject(rt, "restore-budget-cancel", saved);
        if (QTest::currentTestFailed()) {
            return;
        }
        {
            Reopened r(rt, saved);
            startAndHold(r, saved);
            if (QTest::currentTestFailed()) {
                return;
            }
            r.loader->cancelOverBudget(r.exceeded->at(0).at(0).toInt());
            QTRY_COMPARE_WITH_TIMEOUT(r.finished->count(), 1, kTimeoutMs);
            QVERIFY(!r.restore->running());
            QCOMPARE(r.loader->pendingBudgetOpens(), 0);
            QCOMPARE(r.loader->sources().size(), std::size_t{1});
            const plot::SelectionSnapshot& s = r.selection.snapshot();
            QVERIFY(!(r.selection.hasMain() && s.main == saved.bigLap2)); // big is not open
            // Small's lap 3 is in a slot: Overlay 1, or Main when the open had made it Main already.
            QVERIFY(r.selection.overlayNumber(saved.smallLap3) >= 0 || s.main == saved.smallLap3);
        }
        rt.shutdown();
    }
};

QTEST_GUILESS_MAIN(TstRestoreBudget)
#include "tst_restore_budget.moc"
