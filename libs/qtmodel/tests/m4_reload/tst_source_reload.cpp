// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Changed source files in esses_qtmodel (M4 reload package, AC-M4-10; 06 section 6 "Changed files", 02
// section 1 behaviour 5; D-208). A .ld + .ldx written by the motec.ld writer opens through SourceLoader with
// the standard wiring (connectSourceLoader) and the reload wiring (connectSourceReload).
//   * staleOnChangeWithReloadRow: a file replaced on disk (renamed aside, written anew: an atomic writer) is
//     stale after checkChangedFiles, once: staleChanged, one Messages row (Import, Warning,
//     open.file_changed, action file.reload_source with the id), the Data window model's StaleRole and
//     dataChanged of its row; a file gone from its path is stale too (its own row text); close() clears the
//     mark.
//   * reloadRestoresTheSelection: reload() of a file whose content changed (one more lap) closes it, reopens
//     it through the pipeline (a new SourceId) and announces sourceReloaded(previous, source): the slots back
//     by RangeId at their positions, the filter and the Data-window position restored, the sidecar edit
//     re-applied.
//   * reloadOfTheSameBytesKeepsTheId: the same bytes under a new mtime reopen as the same SourceId and
//     selection.
//   * reloadEndsWhenTheOpenEnds: a reload whose open fails (the file is gone) or is cancelled (cancelOpens)
//     publishes nothing and is no longer pending; reload() of a source that is not open is NotFound.
//   * selection*: DataSelectionModel::sourceReloaded over in-memory sources: the lap-number fallback and a
//     dropped slot (with its hint), a Main the close moved to another source given back to its overlay slot,
//     two reloads in flight at once in either order, closeAllSources and the bound on parked closes.
//   * sourceIdArguments: the action's argument round-trips; anything else is refused.
// Expectations come from the logs the test writes (lap durations, beacon times) and from the slots it sets,
// never from the code under test. Waits are on conditions (QTRY_*) and on an idle pool with every result
// delivered.

#include "../qtmodel_test_support.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/source_reload.hpp>

#include <QSignalSpy>
#include <QTest>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;
namespace fs = std::filesystem;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)
Q_DECLARE_METATYPE(esses::model::SourceId)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)

namespace {

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

// The application wiring of one workspace (06 section 1) with the M4 reload wiring.
struct Workspace {
    AppRuntime& rt;
    DataSelectionModel selection;
    MessagesModel messages;
    SourceLoader loader;
    DataWindowModel rows;

    explicit Workspace(AppRuntime& runtime)
        : rt(runtime), loader(rt.pool(), rt.dispatcher(), shippedUnits()), rows(selection) {
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        connectSourceLoader(loader, selection, &messages);
        connectSourceReload(loader, selection, &messages);
        rows.setStaleQuery([this](const model::SourceId& id) { return loader.isStale(id); });
        QObject::connect(&loader, &SourceLoader::staleChanged, &rows,
                         [this](const model::SourceId& id, bool) { rows.refreshStale(id); });
    }
};

// Every task of the runtime's pool has completed and every result it posted has been delivered.
bool quiet(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0 && rt.dispatcher().pending() == 0;
}

// A circuit log: an out lap, flying laps and an in lap of these durations.
fs::path writeLog(AppRuntime& rt, const fs::path& dir, const std::string& name, std::vector<double> laps) {
    fs::create_directories(dir);
    SyntheticOptions o;
    o.path = name + ".ld";
    o.lapSeconds = std::move(laps);
    return writeLd(*makeSource(o, rt.cache()), dir, name);
}

// Renames a file, again for up to 5 s while it is refused: a virus scanner may hold a file just written
// without sharing delete access for a moment.
bool renameRetrying(const fs::path& from, const fs::path& to) {
    return QTest::qWaitFor(
        [&] {
            std::error_code ec;
            fs::rename(from, to, ec);
            return !ec;
        },
        5000);
}

// What an atomic writer does to an open log (02 section 1 behaviour 5): the mapped file is renamed aside,
// which its share mode allows, and the new file takes the name, its modification time two seconds after the
// old one's. `replacement` is written into a scratch directory first and its .ld bytes (and .ldx) copied in.
void replaceOnDisk(const fs::path& log, const fs::path& replacementLd) {
    const Result<std::string> bytes = readWholeFile(replacementLd);
    QVERIFY(bytes.ok());
    fs::path aside = log;
    aside += ".aside";
    std::error_code ec;
    fs::remove(aside, ec);
    QVERIFY(renameRetrying(log, aside));
    {
        std::ofstream out(log, std::ios::binary | std::ios::trunc);
        out.write(bytes.value().data(), static_cast<std::streamsize>(bytes.value().size()));
    }
    fs::path ldx = replacementLd;
    ldx.replace_extension(".ldx");
    fs::path logLdx = log;
    logLdx.replace_extension(".ldx");
    if (fs::exists(ldx)) {
        fs::copy_file(ldx, logLdx, fs::copy_options::overwrite_existing, ec);
        QVERIFY2(!ec, ec.message().c_str());
    }
    fs::last_write_time(log, fs::last_write_time(aside) + std::chrono::seconds(2), ec);
    QVERIFY2(!ec, ec.message().c_str());
}

std::shared_ptr<const model::DataSource> openAndWait(Workspace& w, const fs::path& ld) {
    QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
    w.loader.open(ld);
    if (!QTest::qWaitFor([&] { return published.count() == 1; }, 30000)) {
        return nullptr;
    }
    return published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
}

// The Messages rows with the stale code.
std::vector<Message> staleRows(const MessagesModel& m) {
    std::vector<Message> out;
    for (int i = 0; i < m.rowCount(); ++i) {
        if (m.at(i).code == QStringLiteral("open.file_changed")) {
            out.push_back(m.at(i));
        }
    }
    return out;
}

QModelIndex sourceRow(const DataWindowModel& rows, const model::SourceId& id) {
    for (int r = 0; r < rows.rowCount(); ++r) {
        const QModelIndex i = rows.index(r, DataWindowModel::NameCol);
        if (rows.sourceAt(i) == id) {
            return i;
        }
    }
    return {};
}

model::RangeRef withSource(model::RangeRef r, const model::SourceId& id) {
    r.src = id;
    return r;
}

} // namespace

class TstSourceReload : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
    }

    void staleOnChangeWithReloadRow() {
        AppRuntime rt(runtimeOptions());
        const fs::path dir = scratchDir("reload-stale");
        const fs::path a = writeLog(rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
        const fs::path b = writeLog(rt, dir / "b", "session-b", {30.0, 45.0, 44.0, 20.0});
        {
            Workspace w(rt);
            w.loader.setUseSidecars(false);
            const auto srcA = openAndWait(w, a);
            const auto srcB = openAndWait(w, b);
            QVERIFY(srcA != nullptr && srcB != nullptr);
            QVERIFY(w.loader.checkChangedFiles().empty()); // nothing changed yet
            QVERIFY(w.loader.staleSources().empty());
            QVERIFY(!w.rows.data(sourceRow(w.rows, srcA->id), DataWindowModel::StaleRole).toBool());

            QSignalSpy stale(&w.loader, &SourceLoader::staleChanged);
            QSignalSpy rowChanged(&w.rows, &QAbstractItemModel::dataChanged);
            const auto other = writeLog(rt, dir / "next", "session-a", {40.0, 50.0, 48.0, 49.0, 47.0, 30.0});
            replaceOnDisk(a, other);
            QCOMPARE(w.loader.checkChangedFiles(), std::vector<model::SourceId>{srcA->id});
            QCOMPARE(stale.count(), 1);
            QCOMPARE(stale.first().at(0).value<model::SourceId>(), srcA->id);
            QCOMPARE(stale.first().at(1).toBool(), true);
            QVERIFY(w.loader.isStale(srcA->id));
            QVERIFY(!w.loader.isStale(srcB->id));
            QCOMPARE(w.loader.staleSources(), std::vector<model::SourceId>{srcA->id});
            // The badge: StaleRole on a's row only, and its row announced.
            QVERIFY(w.rows.data(sourceRow(w.rows, srcA->id), DataWindowModel::StaleRole).toBool());
            QVERIFY(!w.rows.data(sourceRow(w.rows, srcB->id), DataWindowModel::StaleRole).toBool());
            QVERIFY(w.rows.data(sourceRow(w.rows, srcA->id), Qt::ToolTipRole)
                        .toString()
                        .contains(QStringLiteral("Changed on disk")));
            QVERIFY(!rowChanged.isEmpty());
            QCOMPARE(rowChanged.last().at(0).value<QModelIndex>().row(), sourceRow(w.rows, srcA->id).row());
            // One Messages row with the Reload action and the source id.
            std::vector<Message> rows = staleRows(w.messages);
            QCOMPARE(rows.size(), std::size_t{1});
            QCOMPARE(rows[0].severity, Message::Severity::Warning);
            QCOMPARE(rows[0].origin, Message::Origin::Import);
            QCOMPARE(rows[0].action, QStringLiteral("file.reload_source"));
            QCOMPARE(rows[0].actionArgs, QVariantList{QString::fromStdString(srcA->id.hex())});
            QCOMPARE(rows[0].source, QString::fromStdString(pathToUtf8(a)));
            QVERIFY(rows[0].text.startsWith(QStringLiteral("file changed on disk")));
            // Checked again: nothing new (the mark is sticky, the row is not repeated).
            QVERIFY(w.loader.checkChangedFiles().empty());
            QCOMPARE(stale.count(), 1);
            QCOMPARE(staleRows(w.messages).size(), std::size_t{1});

            // A file gone from its path (deleted, moved, a share that dropped) is stale too.
            QVERIFY(renameRetrying(b, dir / "b" / "moved.ld"));
            QCOMPARE(w.loader.checkChangedFiles(), std::vector<model::SourceId>{srcB->id});
            rows = staleRows(w.messages);
            QCOMPARE(rows.size(), std::size_t{2});
            QVERIFY(rows[1].text.contains(QStringLiteral("can no longer be read")));
            QCOMPARE(w.loader.staleSources(), (std::vector<model::SourceId>{srcA->id, srcB->id}));

            // Closing a source clears its mark.
            stale.clear();
            w.loader.close(srcB->id);
            QCOMPARE(stale.count(), 1);
            QCOMPARE(stale.first().at(1).toBool(), false);
            QVERIFY(!w.loader.isStale(srcB->id));
            QVERIFY(QTest::qWaitFor([&] { return quiet(rt); }, 30000));
        }
        rt.shutdown();
    }

    void reloadRestoresTheSelection() {
        AppRuntime rt(runtimeOptions());
        const fs::path dir = scratchDir("reload-restore");
        const fs::path a = writeLog(rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
        const fs::path b = writeLog(rt, dir / "b", "session-b", {30.0, 45.0, 44.0, 20.0});
        {
            Workspace w(rt);
            const auto srcA = openAndWait(w, a);
            const auto srcB = openAndWait(w, b);
            QVERIFY(srcA != nullptr && srcB != nullptr);
            // a: out lap, laps 1-3 (50, 48, 49 s), in lap; b: out lap, laps 1-2, in lap.
            const model::RangeRef mainA = rangeRef(*srcA, 2);
            const model::RangeRef overlayA = rangeRef(*srcA, 3);
            const model::RangeRef overlay0B = rangeRef(*srcB, 1);
            const model::RangeRef overlayB = rangeRef(*srcB, 2);
            w.selection.setMain(mainA);
            w.selection.toggleOverlay0(overlay0B);
            w.selection.toggleOverlay(overlayA);
            w.selection.toggleOverlay(overlayB);
            w.selection.setFilter(srcA->id, FilterSpec{FilterSpec::Kind::Fastest3, {}});
            w.selection.setTrusted(rangeRef(*srcA, 1),
                                   false); // a sidecar edit, written when the source closes
            QVERIFY(!rangeAt(*w.loader.source(srcA->id), 1).trusted);
            QCOMPARE(w.selection.sources().front()->id, srcA->id);

            // a is replaced by a session with one more flying lap (47 s) before the in lap: the first laps
            // keep their beacons (so their RangeIds), the content and so the SourceId change.
            replaceOnDisk(a, writeLog(rt, dir / "next", "session-a", {40.0, 50.0, 48.0, 49.0, 47.0, 30.0}));
            QCOMPARE(w.loader.checkChangedFiles(), std::vector<model::SourceId>{srcA->id});

            QSignalSpy reloaded(&w.loader, &SourceLoader::sourceReloaded);
            QSignalSpy stale(&w.loader, &SourceLoader::staleChanged);
            QVERIFY(w.loader.reload(srcA->id).ok());
            QCOMPARE(w.loader.pendingReloads(), 1);
            QVERIFY(w.loader.source(srcA->id) == nullptr); // closed at once
            QVERIFY(!w.loader.isStale(srcA->id));
            QCOMPARE(stale.count(), 1); // the mark went with the closed source
            QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 30000);
            QCOMPARE(w.loader.pendingReloads(), 0);
            QCOMPARE(reloaded.first().at(0).value<model::SourceId>(), srcA->id);
            const auto next = reloaded.first().at(1).value<std::shared_ptr<const model::DataSource>>();
            QVERIFY(next != nullptr);
            QVERIFY(!(next->id == srcA->id)); // other content, another SourceId
            QCOMPARE(next->prov.path, a);
            QCOMPARE(rangeCount(*next), std::size_t{6});
            QVERIFY(!w.loader.isStale(next->id));

            // The selection: the same ranges by RangeId on the new source, in the same slots and positions.
            const plot::SelectionSnapshot& s = w.selection.snapshot();
            QVERIFY(s.main == withSource(mainA, next->id));
            QVERIFY(w.selection.resolve(s.main) != nullptr);
            QCOMPARE(w.selection.resolve(s.main)->start, rangeAt(*srcA, 2).start);
            QVERIFY(s.overlay0 && *s.overlay0 == overlay0B);
            QCOMPARE(s.overlays.size(), std::size_t{2});
            QVERIFY(s.overlays[0] == withSource(overlayA, next->id));
            QVERIFY(s.overlays[1] == overlayB);
            // Its filter and Data-window position.
            QCOMPARE(w.selection.filter(next->id), (FilterSpec{FilterSpec::Kind::Fastest3, {}}));
            QCOMPARE(w.selection.sources().front()->id, next->id);
            QCOMPARE(w.rows.sourceAt(w.rows.index(0, 0)), std::optional<model::SourceId>(next->id));
            // The sidecar re-applied: lap 1 is still untrusted.
            QVERIFY(!rangeAt(*w.selection.source(next->id), 1).trusted);
            QVERIFY(QTest::qWaitFor([&] { return quiet(rt); }, 30000));
        }
        rt.shutdown();
    }

    void reloadOfTheSameBytesKeepsTheId() {
        AppRuntime rt(runtimeOptions());
        const fs::path dir = scratchDir("reload-same");
        const fs::path a = writeLog(rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
        const fs::path copy = dir / "copy" / "session-a.ld";
        fs::create_directories(copy.parent_path());
        fs::copy_file(a, copy);
        {
            Workspace w(rt);
            w.loader.setUseSidecars(false);
            const auto src = openAndWait(w, a);
            QVERIFY(src != nullptr);
            w.selection.setMain(rangeRef(*src, 3));
            w.selection.toggleOverlay(rangeRef(*src, 1));
            const plot::SelectionSnapshot before = w.selection.snapshot();
            replaceOnDisk(a, copy); // the same bytes in a newer file
            QCOMPARE(w.loader.checkChangedFiles(), std::vector<model::SourceId>{src->id});
            QSignalSpy reloaded(&w.loader, &SourceLoader::sourceReloaded);
            QVERIFY(w.loader.reload(src->id).ok());
            QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 30000);
            const auto next = reloaded.first().at(1).value<std::shared_ptr<const model::DataSource>>();
            QCOMPARE(next->id, src->id); // the same bytes: the same SourceId
            QVERIFY(next != src);        // a new snapshot of the reopened file
            QVERIFY(w.selection.snapshot().main == before.main);
            QVERIFY(w.selection.snapshot().overlays == before.overlays);
            QVERIFY(!w.loader.isStale(src->id));
            QVERIFY(QTest::qWaitFor([&] { return quiet(rt); }, 30000));
        }
        rt.shutdown();
    }

    void reloadEndsWhenTheOpenEnds() {
        AppRuntime rt(runtimeOptions());
        const fs::path dir = scratchDir("reload-ends");
        const fs::path a = writeLog(rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
        const fs::path b = writeLog(rt, dir / "b", "session-b", {30.0, 45.0, 44.0, 20.0});
        {
            Workspace w(rt);
            w.loader.setUseSidecars(false);
            QSignalSpy reloaded(&w.loader, &SourceLoader::sourceReloaded);
            QSignalSpy failed(&w.loader, &SourceLoader::openFailed);
            QCOMPARE(w.loader.reload(model::SourceId{}).error().code, ErrorCode::NotFound);

            // A file gone: the reopen fails, and the reload with it.
            const auto srcA = openAndWait(w, a);
            QVERIFY(srcA != nullptr);
            QVERIFY(renameRetrying(a, dir / "a" / "gone.ld"));
            QCOMPARE(w.loader.checkChangedFiles(), std::vector<model::SourceId>{srcA->id});
            QVERIFY(w.loader.reload(srcA->id).ok());
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 30000);
            QCOMPARE(w.loader.pendingReloads(), 0);
            QVERIFY(w.selection.source(srcA->id) == nullptr);

            // A reload whose open is cancelled (a project change, Close All) ends at once.
            const auto srcB = openAndWait(w, b);
            QVERIFY(srcB != nullptr);
            QVERIFY(w.loader.reload(srcB->id).ok());
            QCOMPARE(w.loader.pendingReloads(), 1);
            w.loader.cancelOpens();
            QCOMPARE(w.loader.pendingReloads(), 0);
            QVERIFY(QTest::qWaitFor([&] { return quiet(rt); }, 30000));
            QCOMPARE(reloaded.count(), 0);
            QVERIFY(w.loader.sources().empty());
        }
        rt.shutdown();
    }

    // The 06 section 5.6 fallbacks after a reload: a lap whose start beacon moved is found by its number and
    // class; a lap that no longer exists leaves its slot with a hint.
    void selectionFallsBackToTheLapNumber() {
        AppRuntime rt(runtimeOptions());
        {
            SyntheticOptions o;
            o.path = "a.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            const auto a = makeSource(o, rt.cache());
            o.path = "a-changed.ld"; // another SourceId; lap 2 is a second shorter, so lap 3 starts earlier
            o.lapSeconds = {40.0, 50.0, 47.0, 49.0, 30.0};
            const auto changed = makeSource(o, rt.cache());
            o.path = "a-short.ld"; // only one flying lap left
            o.lapSeconds = {40.0, 50.0, 30.0};
            const auto shorter = makeSource(o, rt.cache());

            DataSelectionModel sel;
            QSignalSpy hints(&sel, &DataSelectionModel::hint);
            sel.sourcePublished(a);
            sel.setMain(rangeRef(*a, 2));       // lap 2: its start beacon (90 s) is in both files
            sel.toggleOverlay(rangeRef(*a, 3)); // lap 3: starts at 138 s, at 137 s in the changed file
            sel.closeSource(a->id);
            QVERIFY(!sel.hasMain());
            sel.sourceReloaded(a->id, changed);
            QVERIFY(sel.snapshot().main == rangeRef(*changed, 2));
            QCOMPARE(rangeRef(*changed, 2).range, rangeRef(*a, 2).range); // the same RangeId
            QCOMPARE(sel.snapshot().overlays.size(), std::size_t{1});
            QVERIFY(!(rangeRef(*changed, 3).range == rangeRef(*a, 3).range)); // another start marker
            QVERIFY(sel.snapshot().overlays[0] == rangeRef(*changed, 3));     // found by lap number 3
            QCOMPARE(rangeAt(*changed, 3).number, rangeAt(*a, 3).number);
            QCOMPARE(hints.count(), 0);

            // The short file has no lap 3: the overlay leaves its slot with a hint. Main follows its start
            // beacon (90 s, the short file's second beacon too) by RangeId: there it starts the in lap.
            sel.closeSource(changed->id);
            sel.sourceReloaded(changed->id, shorter);
            QVERIFY(sel.snapshot().overlays.empty());
            QCOMPARE(hints.count(), 1);
            QVERIFY(
                hints.last().at(0).toString().contains(QString::fromStdString(rangeAt(*changed, 3).name)));
            QVERIFY(sel.snapshot().main == rangeRef(*shorter, 2));
            QCOMPARE(rangeAt(*shorter, 2).start, rangeAt(*changed, 2).start);
            QCOMPARE(rangeAt(*shorter, 2).lapClass, model::LapClass::In);
        }
        rt.shutdown();
    }

    // A Main that the close moved onto another source goes back to the overlay slot it held.
    void selectionGivesBackAMovedMain() {
        AppRuntime rt(runtimeOptions());
        {
            SyntheticOptions o;
            o.path = "a.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            const auto a = makeSource(o, rt.cache());
            o.path = "b.ld";
            o.lapSeconds = {30.0, 45.0, 44.0, 20.0};
            const auto b = makeSource(o, rt.cache());
            DataSelectionModel sel;
            sel.sourcePublished(a);
            sel.sourcePublished(b);
            sel.setMain(rangeRef(*a, 2));
            sel.toggleOverlay(rangeRef(*b, 0)); // b's first range: where Main goes when a closes
            sel.toggleOverlay(rangeRef(*a, 1));
            sel.toggleOverlay(rangeRef(*b, 2));
            sel.closeSource(a->id);
            QVERIFY(sel.snapshot().main == rangeRef(*b, 0)); // moved out of overlay 1 into Main
            QCOMPARE(sel.snapshot().overlays.size(), std::size_t{1});
            QCOMPARE(sel.sources().front()->id, b->id);
            sel.sourceReloaded(a->id, a); // the same file again (the same SourceId)
            QVERIFY(sel.snapshot().main == rangeRef(*a, 2));
            QCOMPARE(sel.snapshot().overlays.size(), std::size_t{3});
            QVERIFY(sel.snapshot().overlays[0] == rangeRef(*b, 0)); // back in its slot
            QVERIFY(sel.snapshot().overlays[1] == rangeRef(*a, 1));
            QVERIFY(sel.snapshot().overlays[2] == rangeRef(*b, 2));
            QCOMPARE(sel.sources().front()->id, a->id); // and a first again
        }
        rt.shutdown();
    }

    // Two reloads in flight at once (a held Main, b an overlay), their reopens published in either order.
    void selectionTwoReloadsInFlight() {
        for (const bool aFirst : {true, false}) {
            AppRuntime rt(runtimeOptions());
            {
                SyntheticOptions o;
                o.path = "a.ld";
                o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
                const auto a = makeSource(o, rt.cache());
                o.path = "b.ld";
                o.lapSeconds = {30.0, 45.0, 44.0, 20.0};
                const auto b = makeSource(o, rt.cache());
                DataSelectionModel sel;
                sel.sourcePublished(a);
                sel.sourcePublished(b);
                sel.setMain(rangeRef(*a, 2));
                sel.toggleOverlay(rangeRef(*b, 0));
                sel.toggleOverlay(rangeRef(*b, 2));
                sel.closeSource(a->id); // Main moves onto b's first range
                sel.closeSource(b->id); // and leaves with b: nothing is left
                QVERIFY(!sel.hasMain());
                if (aFirst) {
                    sel.sourceReloaded(a->id, a);
                    sel.sourceReloaded(b->id, b);
                } else {
                    sel.sourceReloaded(b->id, b);
                    sel.sourceReloaded(a->id, a);
                }
                QVERIFY2(sel.snapshot().main == rangeRef(*a, 2), aFirst ? "a first" : "b first");
                QCOMPARE(sel.snapshot().overlays.size(), std::size_t{2});
                QVERIFY(sel.snapshot().overlays[0] == rangeRef(*b, 0));
                QVERIFY(sel.snapshot().overlays[1] == rangeRef(*b, 2));
                QCOMPARE(sel.sources().size(), std::size_t{2});
                QCOMPARE(sel.sources()[0]->id, a->id);
                QCOMPARE(sel.sources()[1]->id, b->id);
            }
            rt.shutdown();
        }
    }

    // Nothing is parked across closeAllSources, and only the newest kParkedCloses closes are kept.
    void selectionParkingIsBounded() {
        AppRuntime rt(runtimeOptions());
        {
            std::vector<std::shared_ptr<const model::DataSource>> logs;
            for (std::size_t i = 0; i <= DataSelectionModel::kParkedCloses; ++i) {
                SyntheticOptions o;
                o.path = "log" + std::to_string(i) + ".ld";
                o.lapSeconds = {40.0, 50.0, 48.0, 30.0};
                logs.push_back(makeSource(o, rt.cache()));
            }
            DataSelectionModel sel;
            sel.sourcePublished(logs[0]);
            sel.setMain(rangeRef(*logs[0], 2));
            sel.closeAllSources();
            sel.sourceReloaded(logs[0]->id, logs[0]); // published, but nothing to restore
            QVERIFY(sel.snapshot().main == rangeRef(*logs[0], 0));
            sel.closeAllSources();

            for (const auto& l : logs) {
                sel.sourcePublished(l);
            }
            sel.toggleOverlay(rangeRef(*logs[0], 2));
            for (const auto& l : logs) {
                sel.closeSource(l->id); // kParkedCloses + 1 closes: the first is no longer parked
            }
            sel.sourceReloaded(logs[0]->id, logs[0]);
            QVERIFY(sel.snapshot().overlays.empty());
            QVERIFY(sel.snapshot().main == rangeRef(*logs[0], 0));
        }
        rt.shutdown();
    }

    void sourceIdArguments() {
        model::SourceId id;
        id.hi = 0x0123456789abcdefULL;
        id.lo = 0xfedcba9876543210ULL;
        QCOMPARE(sourceIdArgument(id), QStringLiteral("0123456789abcdeffedcba9876543210"));
        QCOMPARE(parseSourceIdArgument(sourceIdArgument(id)), std::optional<model::SourceId>(id));
        QVERIFY(!parseSourceIdArgument(QStringLiteral("0123")).has_value());
        QVERIFY(!parseSourceIdArgument(QStringLiteral("0123456789ABCDEFfedcba9876543210")).has_value());
        QVERIFY(!parseSourceIdArgument(QStringLiteral("0x23456789abcdeffedcba9876543210")).has_value());
        QVERIFY(!parseSourceIdArgument(QString()).has_value());
    }
};

QTEST_GUILESS_MAIN(TstSourceReload)
#include "tst_source_reload.moc"
