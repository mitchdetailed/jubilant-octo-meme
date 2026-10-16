// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapEditor (06 section 13.3; AC-M2-05 in the editor; D-014, D-016, D-032), offscreen over a written .ld +
// .ldx opened through the real loader: the laps table (number, class, session-time start, duration, trusted,
// provenance; terms from the profile); a beacon inserted by session time, by lap time after a lap, through
// the editor's own controls and at the main cursor (the button and SHIFT+F7's cursor.insert_marker) rebuilds
// the laps synchronously as one undoable command while the trust flag and the alignment offset of a later lap
// follow its start marker; per-beacon time offsets (logged beacons through a marker edit, user beacons
// moved); the beacon offset by distance (one command per Apply, "pending" until it is resolved, 0 removes
// it); restore logged laps (one command that also deletes the offsetDistance op); ignored beacons greyed and
// a device reset as a Partial lap; the source combo and following Main; the Stretch column, the stretch
// report and the lap-stretch toggle shared with the toolbar action (view state, never on the undo stack).

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/project/keymap.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QToolBar>
#include <QUndoStack>
#include <memory>

// The qtmodel synthetic-source helpers, compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::makeSource;
using esses::qt::test::rangeAt;
using esses::qt::test::rangeRef;
using esses::qt::test::repoRoot;
using esses::qt::test::scratchDir;
using esses::qt::test::shippedUnits;
using esses::qt::test::SyntheticOptions;
using esses::qt::test::writeLd;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

namespace {

constexpr Micros kSecond = 1'000'000;

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

// Out lap 40 s, flying laps of 50, 48 and 49 s, in lap 30 s (beacons at 40, 90, 138 and 187 s, end 217 s).
SyntheticOptions laps(const char* path) {
    SyntheticOptions o;
    o.path = path;
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    o.lapLengthScale = {1.0, 1.02, 0.97, 1.10, 1.0};
    o.trackLengthM = 2000.0;
    return o;
}

struct Rig {
    std::filesystem::path root; // first: the scratch directory is made once
    qt::AppRuntime rt;
    qt::ProjectSession session;
    qt::MathsService maths;
    qt::DistanceService distance;
    qt::SourceLoader loader;
    qt::DataSelectionModel selection;
    qt::WorkspaceViewModel workspace;
    std::unique_ptr<qt::LapStretchService> stretch;
    std::filesystem::path dir;

    explicit Rig(const std::string& name)
        : root(scratchDir(name)), rt([] {
              qt::AppRuntimeOptions o;
              o.workers = 4;
              o.cacheBudget = std::size_t{256} << 20;
              o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
              return o;
          }()),
          session(pathsFor(root / "config")), maths(rt.pool(), rt.dispatcher(), shippedUnits()),
          distance(maths, rt.dispatcher()), loader(rt.pool(), rt.dispatcher(), shippedUnits()),
          selection(&session), dir(root / "logs") {
        static_cast<void>(session.loadDefaults());
        static_cast<void>(maths.loadSettingsSchema(repoRoot() / "data"));
        std::filesystem::create_directories(dir);
        loader.setUseSidecars(false);
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
        qt::connectSourceLoader(loader, selection, nullptr); // trust edits become undoable commands
        stretch = std::make_unique<qt::LapStretchService>(loader, distance, shippedUnits(), &session);
    }

    std::shared_ptr<const model::DataSource> open(const SyntheticOptions& o, const std::string& name) {
        std::shared_ptr<const model::DataSource> written;
        {
            ThreadRole::Scope normal(ThreadRole::Normal); // writing the log waits on purpose
            written = makeSource(o, rt.cache());
        }
        std::filesystem::path ld;
        {
            ThreadRole::Scope normal(ThreadRole::Normal);
            ld = writeLd(*written, dir, name);
        }
        QSignalSpy published(&loader, &qt::SourceLoader::sourcePublished);
        loader.open(ld);
        if (!QTest::qWaitFor([&] { return published.count() == 1; }, 30000)) {
            return nullptr;
        }
        return published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
    }
    std::shared_ptr<const model::DataSource> current(const model::SourceId& id) const {
        return loader.source(id);
    }
    QUndoStack& undo() { return session.undoStack(); }
};

QString cell(LapEditor& e, int row, int column, int role = Qt::DisplayRole) {
    return e.table().model()->index(row, column).data(role).toString();
}

int checkState(LapEditor& e, int row) {
    return e.table().model()->index(row, LapEditor::TrustedCol).data(Qt::CheckStateRole).toInt();
}

int rowStartingAt(LapEditor& e, const QString& start) {
    for (int r = 0; r < e.rowCount(); ++r) {
        if (cell(e, r, LapEditor::StartCol) == start) {
            return r;
        }
    }
    return -1;
}

} // namespace

class TstLapEditor : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    void timeText() {
        QCOMPARE(LapEditor::formatTime(65'250'000), QStringLiteral("1:05.250"));
        QCOMPARE(LapEditor::formatTime(3'725'500'000), QStringLiteral("1:02:05.500"));
        QCOMPARE(LapEditor::formatTime(-1'500'000), QStringLiteral("-0:01.500"));
        QCOMPARE(*LapEditor::parseTime(QStringLiteral("1:05.250")), Micros{65'250'000});
        QCOMPARE(*LapEditor::parseTime(QStringLiteral(" 65.25 ")), Micros{65'250'000});
        QCOMPARE(*LapEditor::parseTime(QStringLiteral("1:02:05.5")), Micros{3'725'500'000});
        QVERIFY(!LapEditor::parseTime(QStringLiteral("1:75")).has_value());
        QVERIFY(!LapEditor::parseTime(QStringLiteral("abc")).has_value());
        QVERIFY(!LapEditor::parseTime(QStringLiteral("-3")).has_value());
        QVERIFY(!LapEditor::parseTime(QStringLiteral("1:2:3:4")).has_value());
    }

    void listsTheLapsWithTheProfileTerms() {
        Rig rig("de-laps-list");
        const auto src = rig.open(laps("list.ld"), "list");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        QVERIFY(!e.isModal());
        QCOMPARE(e.windowTitle(), QStringLiteral("Lap Editor"));
        QCOMPARE(e.rangeTerm(), QStringLiteral("Lap"));
        QCOMPARE(e.table().model()->headerData(LapEditor::NumberCol, Qt::Horizontal).toString(),
                 QStringLiteral("Lap"));
        QCOMPARE(e.source(), src->id); // the Main source
        QCOMPARE(e.rowCount(), 5);
        QCOMPARE(cell(e, 0, LapEditor::ClassCol), QStringLiteral("Out"));
        QCOMPARE(checkState(e, 0), int(Qt::Unchecked));
        QCOMPARE(cell(e, 1, LapEditor::NumberCol), QStringLiteral("1"));
        QCOMPARE(cell(e, 1, LapEditor::ClassCol), QStringLiteral("Flying"));
        QCOMPARE(cell(e, 1, LapEditor::StartCol), QStringLiteral("0:40.000"));
        QCOMPARE(cell(e, 1, LapEditor::DurationCol), QStringLiteral("0:50.000"));
        QCOMPARE(checkState(e, 1), int(Qt::Checked));
        QCOMPARE(cell(e, 1, LapEditor::ProvenanceCol), QStringLiteral("Companion"));
        QCOMPARE(cell(e, 4, LapEditor::ClassCol), QStringLiteral("In"));
        QCOMPARE(cell(e, 4, LapEditor::DurationCol), QStringLiteral("0:30.000"));
        QVERIFY(e.table().isColumnHidden(LapEditor::StretchCol)); // no stretch service attached
        QCOMPARE(e.sourceCombo().count(), 1);
        QVERIFY(e.lapAt(1).has_value());
        QCOMPARE(e.rowOf(*e.lapAt(3)), 3);
        QVERIFY(!e.lapAt(9).has_value());
    }

    // AC-M2-05 in the editor: inserts rebuild the laps at once as one command each; trust and offset of a
    // later lap follow its start marker.
    void insertBeaconRebuildsAndKeepsKeyedFlags() {
        Rig rig("de-laps-insert");
        const auto src = rig.open(laps("insert.ld"), "insert");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        e.show();
        QVERIFY(QTest::qWaitForWindowExposed(&e));
        QVERIFY(e.setTrusted(3, false));
        QCOMPARE(rig.undo().count(), 1);
        QCOMPARE(checkState(e, 3), int(Qt::Unchecked));
        QVERIFY(!e.setTrusted(3, false)); // unchanged
        const model::Range lap3 = rangeAt(*rig.current(src->id), 3);
        rig.undo().push(new qt::OffsetRangeCommand(rig.loader, *rig.current(src->id), "Normal",
                                                   "Outings:Laps", lap3, 125'000, 4.5));
        QCOMPARE(rig.undo().count(), 2);

        QVERIFY(e.insertBeaconAt(65 * kSecond));
        QCOMPARE(e.rowCount(), 6); // synchronous: no wait
        QCOMPARE(rig.undo().count(), 3);
        QCOMPARE(rig.undo().undoText(), QStringLiteral("Insert beacon"));
        const int inserted = rowStartingAt(e, QStringLiteral("1:05.000"));
        QCOMPARE(inserted, 2);
        QCOMPARE(cell(e, inserted, LapEditor::ProvenanceCol), QStringLiteral("User"));
        QCOMPARE(cell(e, 1, LapEditor::DurationCol), QStringLiteral("0:25.000"));
        // The former lap 3 (start 2:18.000) is renumbered, keeps its trust flag and its offset.
        const int moved = rowStartingAt(e, QStringLiteral("2:18.000"));
        QCOMPARE(moved, 4);
        QCOMPARE(cell(e, moved, LapEditor::NumberCol), QStringLiteral("4"));
        QCOMPARE(checkState(e, moved), int(Qt::Unchecked));
        const model::Range renumbered =
            rangeAt(*rig.current(src->id), 4); // a copy: the snapshot is a temporary
        QCOMPARE(renumbered.startMarker, lap3.startMarker);
        QCOMPARE(renumbered.offsetUs, Micros{125'000});
        QCOMPARE(renumbered.offsetM, 4.5);
        rig.undo().undo();
        QCOMPARE(e.rowCount(), 5);
        rig.undo().redo();
        QCOMPARE(e.rowCount(), 6);

        // Refusals leave the stack alone and say why.
        QVERIFY(!e.insertBeaconAt(65 * kSecond)); // a beacon is there already
        QVERIFY(!e.lastError().isEmpty());
        QVERIFY(!e.insertBeaconAt(0));
        QVERIFY(!e.insertBeaconAt(300 * kSecond));
        QCOMPARE(rig.undo().count(), 3);

        // By lap time after the lap starting at 1:30.000 (48 s long).
        const int lap2 = rowStartingAt(e, QStringLiteral("1:30.000"));
        QVERIFY(lap2 >= 0);
        QVERIFY(!e.insertBeaconAfterLap(lap2, 48 * kSecond)); // not inside the lap
        QVERIFY(e.insertBeaconAfterLap(lap2, 20 * kSecond));
        QVERIFY(rowStartingAt(e, QStringLiteral("1:50.000")) >= 0);
        QCOMPARE(e.rowCount(), 7);

        // The editor's own controls: session time typed and Insert clicked.
        auto* time = e.findChild<QLineEdit*>(QStringLiteral("insertTime"));
        QVERIFY(time != nullptr);
        time->setText(QStringLiteral("2:30"));
        e.findChild<QPushButton*>(QStringLiteral("insertBeacon"))->click();
        QVERIFY(rowStartingAt(e, QStringLiteral("2:30.000")) >= 0);
        // Lap time after the highlighted lap.
        e.setCurrentRow(rowStartingAt(e, QStringLiteral("0:40.000")));
        e.findChildren<QRadioButton*>().at(1)->setChecked(true);
        time->setText(QStringLiteral("10"));
        e.findChild<QPushButton*>(QStringLiteral("insertBeacon"))->click();
        QVERIFY(rowStartingAt(e, QStringLiteral("0:50.000")) >= 0);
        time->setText(QStringLiteral("nonsense"));
        e.findChild<QPushButton*>(QStringLiteral("insertBeacon"))->click();
        QVERIFY(e.lastError().contains(QStringLiteral("nonsense")));
        QCOMPARE(rig.undo().count(), 6);
    }

    void insertAtTheCursorAndShiftF7() {
        Rig rig("de-laps-cursor");
        const auto src = rig.open(laps("cursor.ld"), "cursor");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        rig.selection.setMain(rangeRef(*src, 2)); // Lap 2 starts at 1:30.000
        rig.workspace.setCursor(10 * kSecond);
        QVERIFY(e.findChild<QPushButton*>(QStringLiteral("insertAtCursor"))->isEnabled());
        QVERIFY(e.insertBeaconAtCursor());
        QVERIFY(rowStartingAt(e, QStringLiteral("1:40.000")) >= 0);
        QCOMPARE(rig.undo().count(), 1);

        qt::CommandRegistry registry;
        registerInsertBeaconCommand(registry, rig.session, rig.loader, rig.selection, rig.workspace);
        const qt::CommandSpec* spec = registry.find(QStringLiteral("cursor.insert_marker"));
        QVERIFY(spec != nullptr);
        QCOMPARE(spec->context, qt::CommandContext::Graph);
        QCOMPARE(spec->defaultKeys, QStringList{QStringLiteral("Shift+F7")});
        QVERIFY(registry.isEnabled(QStringLiteral("cursor.insert_marker")));
        // Main is re-resolved on the new snapshot (the same lap, still starting at 1:30.000).
        rig.workspace.setCursor(5 * kSecond);
        QVERIFY(registry.invoke(QStringLiteral("cursor.insert_marker")));
        QVERIFY(rowStartingAt(e, QStringLiteral("1:35.000")) >= 0);
        QCOMPARE(rig.undo().count(), 2);
        // The cursor beyond the end of the data inserts nothing.
        rig.workspace.setCursor(500 * kSecond);
        QVERIFY(!e.insertBeaconAtCursor());
        QCOMPARE(rig.undo().count(), 2);
        // Without a workspace the editor cannot place at the cursor.
        LapEditor detached(rig.session, rig.loader, rig.selection, nullptr);
        QVERIFY(!detached.findChild<QPushButton*>(QStringLiteral("insertAtCursor"))->isEnabled());
        QVERIFY(!detached.insertBeaconAtCursor());
    }

    void beaconOffsetsAndRestoreLogged() {
        Rig rig("de-laps-offsets");
        const auto src = rig.open(laps("offsets.ld"), "offsets");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        e.show();
        QVERIFY(QTest::qWaitForWindowExposed(&e));
        // The start beacon of the lap starting at 1:30.000 moves 1.5 s earlier (a logged beacon: marker
        // edit).
        const int lap2 = rowStartingAt(e, QStringLiteral("1:30.000"));
        QVERIFY(e.setBeaconTimeOffset(lap2, -1'500'000));
        QCOMPARE(rig.undo().count(), 1);
        QCOMPARE(rig.undo().undoText(), QStringLiteral("Edit beacon"));
        const int edited = rowStartingAt(e, QStringLiteral("1:28.500"));
        QVERIFY(edited >= 0);
        QCOMPARE(cell(e, edited, LapEditor::ProvenanceCol), QStringLiteral("Edited"));
        QCOMPARE(cell(e, edited - 1, LapEditor::DurationCol), QStringLiteral("0:48.500"));
        e.setCurrentRow(edited);
        QCOMPARE(e.findChild<QDoubleSpinBox*>(QStringLiteral("beaconOffset"))->value(), -1.5);
        QVERIFY(!e.setBeaconTimeOffset(edited, -1'500'000)); // already applied
        QVERIFY(!e.setBeaconTimeOffset(0, 1'000'000));       // the out lap starts at the start of the data

        // Beacon offset by distance: one command per Apply; pending until resolved; the laps stay.
        auto* state = e.findChild<QLabel*>(QStringLiteral("offsetDistanceState"));
        QVERIFY(!e.beaconOffsetDistance().has_value());
        e.findChild<QDoubleSpinBox*>(QStringLiteral("offsetDistance"))->setValue(12.5);
        e.findChild<QPushButton*>(QStringLiteral("offsetDistanceApply"))->click();
        QCOMPARE(rig.undo().count(), 2);
        QCOMPARE(*e.beaconOffsetDistance(), 12.5);
        QVERIFY(e.beaconOffsetPending());
        QVERIFY(state->text().contains(QStringLiteral("pending")));
        QCOMPARE(e.rowCount(), 5);
        QVERIFY(!e.applyBeaconOffsetDistance(12.5)); // already applied
        const auto withEdit = rig.current(src->id);  // held: the references below point into it
        const auto& edits = withEdit->sidecar.layer("Normal").markerEdits;
        QVERIFY(std::any_of(edits.begin(), edits.end(), [](const model::MarkerEdit& m) {
            return m.op == model::MarkerEditOp::OffsetDistance && m.dM == 12.5 && m.dMUnit == "m";
        }));

        // A user beacon is moved itself.
        QVERIFY(e.insertBeaconAt(65 * kSecond));
        const int user = rowStartingAt(e, QStringLiteral("1:05.000"));
        QVERIFY(e.setBeaconTimeOffset(user, 1'000'000));
        QVERIFY(rowStartingAt(e, QStringLiteral("1:06.000")) >= 0);
        QCOMPARE(rig.undo().undoText(), QStringLiteral("Move beacon"));
        QCOMPARE(rig.undo().count(), 4);

        // Restore logged laps: one command removes every marker edit (the offsetDistance op included) and
        // every user beacon.
        e.findChild<QPushButton*>(QStringLiteral("restoreLogged"))->click();
        QCOMPARE(rig.undo().count(), 5);
        QCOMPARE(rig.undo().undoText(), QStringLiteral("Restore logged laps"));
        const auto restored = rig.current(src->id);
        const model::SidecarLayer& layer = restored->sidecar.layer("Normal");
        QVERIFY(layer.markerEdits.empty());
        QVERIFY(layer.markers.empty());
        QVERIFY(!e.beaconOffsetDistance().has_value());
        QVERIFY(state->text().contains(QStringLiteral("No offset")));
        QCOMPARE(e.rowCount(), 5);
        QVERIFY(rowStartingAt(e, QStringLiteral("1:30.000")) >= 0);
        QVERIFY(!e.restoreLogged()); // nothing left to restore
        rig.undo().undo();
        QCOMPARE(*e.beaconOffsetDistance(), 12.5);
        QCOMPARE(e.rowCount(), 6);
        // 0 removes the offset by distance.
        QVERIFY(e.applyBeaconOffsetDistance(0.0));
        QVERIFY(!e.beaconOffsetDistance().has_value());
        QVERIFY(!e.applyBeaconOffsetDistance(0.0));
    }

    void ignoredBeaconsAndDeviceResets() {
        Rig rig("de-laps-ignored");
        const auto src = rig.open(laps("ignored.ld"), "ignored");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        // Ignore the beacon at 1:30.000: laps 1 and 2 merge and the beacon is listed greyed.
        const model::Range lap2 = rangeAt(*rig.current(src->id), 2);
        model::MarkerEdit ignore;
        ignore.ref = lap2.startMarker;
        ignore.op = model::MarkerEditOp::Ignore;
        rig.undo().push(new qt::MarkerEditCommand(rig.loader, *rig.current(src->id), "Normal", ignore));
        QCOMPARE(e.rowCount(), 5); // four laps and the ignored beacon
        const int ignored = rowStartingAt(e, QStringLiteral("1:30.000"));
        QVERIFY(ignored >= 0);
        QCOMPARE(cell(e, ignored, LapEditor::ClassCol), QStringLiteral("Ignored"));
        QVERIFY(cell(e, ignored, LapEditor::NumberCol).isEmpty());
        QVERIFY(!e.table().model()->index(ignored, 0).flags().testFlag(Qt::ItemIsEnabled));
        QVERIFY(!e.lapAt(ignored).has_value());
        QVERIFY(!e.setTrusted(ignored, false));
        QCOMPARE(cell(e, ignored - 1, LapEditor::DurationCol), QStringLiteral("1:38.000"));
        rig.undo().undo();
        QCOMPARE(e.rowCount(), 5);
        QCOMPARE(cell(e, rowStartingAt(e, QStringLiteral("1:30.000")), LapEditor::ClassCol),
                 QStringLiteral("Flying"));

        // A device reset inside a lap ends it as a Partial lap.
        model::Marker reset;
        reset.id = model::MarkerId{uuidRandom()};
        reset.cls = model::MarkerClass::RESET;
        reset.t = 100 * kSecond;
        reset.prov = model::Provenance::User;
        rig.undo().push(new qt::InsertMarkerCommand(rig.loader, *rig.current(src->id), "Normal", reset));
        bool partial = false;
        for (int r = 0; r < e.rowCount(); ++r) {
            partial = partial || cell(e, r, LapEditor::ClassCol) == QStringLiteral("Partial");
        }
        QVERIFY(partial);
    }

    void sourceComboAndFollowingMain() {
        Rig rig("de-laps-sources");
        const auto a = rig.open(laps("source-a.ld"), "source-a");
        SyntheticOptions ob = laps("source-b.ld");
        ob.lapSeconds = {30.0, 45.0, 44.0, 20.0};
        ob.lapLengthScale.clear();
        const auto b = rig.open(ob, "source-b");
        QVERIFY(a != nullptr && b != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        QCOMPARE(e.source(), a->id);
        QCOMPARE(e.sourceCombo().count(), 2);
        QSignalSpy rows(&e, &LapEditor::rowsChanged);
        rig.selection.setMain(rangeRef(*b, 1));
        QCOMPARE(e.source(), b->id); // follows Main
        QCOMPARE(e.rowCount(), 4);
        QVERIFY(rows.count() >= 1);
        e.sourceCombo().setCurrentIndex(0);
        Q_EMIT e.sourceCombo().activated(0); // an explicit pick stops following Main
        QCOMPARE(e.source(), a->id);
        QCOMPARE(e.rowCount(), 5);
        rig.selection.setMain(rangeRef(*b, 2));
        QCOMPARE(e.source(), a->id);
        // The cursor insert works on the Main source only: disabled for another source.
        QVERIFY(!e.findChild<QPushButton*>(QStringLiteral("insertAtCursor"))->isEnabled());
        rig.loader.close(a->id);
        QCOMPARE(e.source(), b->id);
        QCOMPARE(e.sourceCombo().count(), 1);
    }

    // The Stretch column, the report and the lap-stretch toggle shared with the toolbar (D-016, D-174).
    void stretchColumnReportAndToolbarToggle() {
        Rig rig("de-laps-stretch");
        const auto src = rig.open(laps("stretch.ld"), "stretch");
        QVERIFY(src != nullptr);
        LapEditor e(rig.session, rig.loader, rig.selection, &rig.workspace);
        e.setLapStretch(rig.stretch.get());
        QVERIFY(!e.table().isColumnHidden(LapEditor::StretchCol));
        rig.stretch->setTrackLength(src->id, 2000.0);
        // 102 % and 97 % of the track are within 5 %: 2000/2040 and 2000/1940; 110 % is outside (blank).
        QTRY_COMPARE_WITH_TIMEOUT(cell(e, 1, LapEditor::StretchCol), QStringLiteral("-1.96 %"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(cell(e, 2, LapEditor::StretchCol), QStringLiteral("+3.09 %"), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            cell(e, 3, LapEditor::StretchCol, Qt::ToolTipRole).contains(QStringLiteral("2200")), 30000);
        QVERIFY(cell(e, 3, LapEditor::StretchCol, Qt::ToolTipRole).contains(QStringLiteral("outside")));
        QVERIFY(cell(e, 3, LapEditor::StretchCol).isEmpty());
        const QStringList report = e.stretchReportText().split(QLatin1Char('\n'));
        QCOMPARE(report.size(), 6); // header and five laps
        QVERIFY(report.at(2).startsWith(QStringLiteral("Lap 1\t")));
        QVERIFY(report.at(2).endsWith(QStringLiteral("\t2000.0\t-1.96\tyes")));
        QVERIFY(report.at(4).endsWith(QStringLiteral("\t2000.0\t\tno"))); // outside the tolerance

        // The toolbar's action and the editor's check box are one state (view state: no undo command).
        qt::CommandRegistry registry;
        qt::KeymapBinder binder(registry);
        CommandActions actions(registry, binder);
        qt::registerLapStretchCommand(registry, *rig.stretch);
        QToolBar bar;
        actions.populateToolBar(bar, {QStringLiteral("data.lap_stretch_toggle")});
        connectLapStretchToggle(actions, *rig.stretch);
        QAction* toggle = actions.action(QStringLiteral("data.lap_stretch_toggle"));
        QVERIFY(toggle != nullptr && bar.actions().contains(toggle));
        QVERIFY(toggle->isCheckable());
        QVERIFY(toggle->isChecked());
        auto* box = e.findChild<QCheckBox*>(QStringLiteral("lapStretchToggle"));
        QVERIFY(box != nullptr && box->isChecked());
        box->click();
        QVERIFY(!rig.stretch->enabled());
        QVERIFY(!toggle->isChecked()); // follows without a refresh by the shell
        QVERIFY(e.table().model()->index(1, LapEditor::StretchCol).data(Qt::ForegroundRole).isValid());
        QTRY_VERIFY_WITH_TIMEOUT(!rangeAt(*rig.current(src->id), 1).stretch.has_value(), 30000);
        toggle->trigger();
        QVERIFY(rig.stretch->enabled());
        QVERIFY(box->isChecked());
        QVERIFY(toggle->isChecked());
        QCOMPARE(rig.undo().count(), 0);

        // The report dialog.
        e.findChild<QPushButton*>(QStringLiteral("stretchReport"))->click();
        auto* dialog = e.findChild<QDialog*>(QStringLiteral("stretchReportDialog"));
        QVERIFY(dialog != nullptr && dialog->isVisible());
        QVERIFY(dialog->findChild<QPlainTextEdit*>()->toPlainText().contains(QStringLiteral("Lap 1")));
        dialog->close();
        e.setLapStretch(nullptr);
        QVERIFY(e.table().isColumnHidden(LapEditor::StretchCol));
    }
};

QTEST_MAIN(TstLapEditor)
#include "tst_lap_editor.moc"
