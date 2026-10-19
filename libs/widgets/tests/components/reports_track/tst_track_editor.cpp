// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackEditor (06 section 13.5; AC-M3-04 "default sections obey the 10 m minimum; user sections persist in
// trackmaps/<venue>.track.json and reload"), offscreen, in a project: the lap picker defaulting to Main, the
// GPS generation of the synthetic circuit (fixtures/track/synthetic-circuit; its shape is AC-M3-04 (a),
// measured here through the editor against the true centreline from tools/gen_track_fixtures.py), Accept
// writing the file with default sections as one undoable command, user sections (add, from the cursor span,
// edit, delete, the 10 m minimum refused), regenerating defaults and the track keeping user sections, rename
// and delete of the track file, the auto-generate project option, a missing Speed+LatG input reported. The
// service is the application's (a MathsService and a Dispatcher): its files are read and written on the pool
// (D-210 (a)), so the file checks wait for the writes to land.

#include "components/reports_track/rt_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/track_map_file.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/widgets/components/track_editor.hpp>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QUndoStack>
#include <algorithm>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::rtw;

namespace {

const std::string kVenue = "Synthetic Circuit";

std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::path(ESSES_TEST_BINARY_DIR) / "rt-scratch" /
                                      (name + "-" + std::to_string(QCoreApplication::applicationPid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
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

struct EditorWorld {
    World w;
    std::filesystem::path dir;
    qt::ProjectSession session;
    qt::TrackMapService maps;
    std::shared_ptr<const model::DataSource> src;

    explicit EditorWorld(const std::string& name)
        : dir(scratch(name)), session(pathsFor(dir / "config")),
          maps(&session, &w.maths, &w.rt.dispatcher()) {
        if (!session.loadDefaults().ok() || !session.newProject(dir / "proj", "p").ok()) {
            qFatal("cannot create the project");
        }
        src = publish(w, circuitSource(repoRoot() / "fixtures" / "track" / "synthetic-circuit", kVenue,
                                       *shippedUnits(), w.rt.cache()));
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
    }
};

bool generated(TrackEditor& editor) {
    QSignalSpy ready(&editor, &TrackEditor::previewReady);
    QSignalSpy failed(&editor, &TrackEditor::generationFailed);
    editor.generate();
    return QTest::qWaitFor([&] { return ready.count() > 0 || failed.count() > 0; }, 30000) &&
           ready.count() > 0;
}

const analysis::TrackSection* sectionNamed(const analysis::TrackMap& map, const std::string& name) {
    for (const analysis::TrackSection& s : map.sections) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

class TstTrackEditor : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void generateAcceptAndDefaultSections() {
        EditorWorld e("te-generate");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        QVERIFY(!editor.isModal());
        QVERIFY(editor.lap().has_value());
        QCOMPARE(*editor.lap(), refOf(*e.src, kLaps, "Lap 1")); // Main by default
        QCOMPARE(editor.venue(), QString::fromStdString(kVenue));
        QCOMPARE(static_cast<int>(editor.method()), static_cast<int>(analysis::TrackMethod::Gps));
        QVERIFY(editor.currentMap() == nullptr);

        // Generate (GPS, the chain heads through MathsService) -> the preview.
        QVERIFY(generated(editor));
        QVERIFY(editor.preview() != nullptr);
        QVERIFY(editor.statusText().contains(QStringLiteral("RMS")));
        const CircuitTruth truth = readCircuitTruth(repoRoot() / "fixtures" / "track" / "synthetic-circuit");
        QVERIFY(rmsToPolyline(editor.preview()->centre, truth.centre) <= 2.0); // AC-M3-04 (a)
        QVERIFY(editor.preview()->sections.empty());
        QVERIFY(e.maps.map(kVenue) == nullptr); // nothing written before Accept

        // Accept: one undoable command writes trackmaps/<venue>.track.json with the default sections.
        QSignalSpy written(&editor, &TrackEditor::mapWritten);
        editor.accept();
        QCOMPARE(written.count(), 1);
        QCOMPARE(editor.result(), 0); // modeless: Accept writes and never closes it
        QCOMPARE(e.session.undoStack().count(), 1);
        const std::filesystem::path path = e.maps.pathFor(kVenue);
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(path), 30000);
        const auto map = editor.currentMap();
        QVERIFY(map != nullptr);
        QVERIFY(map->sections.size() >= 7);
        for (const analysis::TrackSection& s : map->sections) {
            QVERIFY(s.lengthM() >= analysis::TrackMap::kMinSectionM - 1e-6); // the 10 m minimum
            QVERIFY(!s.userDefined);
        }
        QCOMPARE(editor.sectionsTable()->rowCount(), static_cast<int>(map->sections.size()));
        // Undo removes the file, redo writes it again.
        e.session.undoStack().undo();
        QVERIFY(editor.currentMap() == nullptr);
        QCOMPARE(editor.sectionsTable()->rowCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!std::filesystem::exists(path), 30000);
        e.session.undoStack().redo();
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(path), 30000);
        QCOMPARE(editor.sectionsTable()->rowCount(), static_cast<int>(map->sections.size()));
    }

    void userSectionsPersistAndReload() {
        EditorWorld e("te-sections");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        QVERIFY(generated(editor));
        editor.accept();
        QVERIFY(editor.currentMap() != nullptr);
        const int base = e.session.undoStack().count();

        QVERIFY(editor.addSection(QStringLiteral("T1 entry"), 150.0, 230.0, QStringLiteral("corner")));
        const auto withUser = editor.currentMap();
        const analysis::TrackSection* user = sectionNamed(*withUser, "T1 entry");
        QVERIFY(user != nullptr);
        QVERIFY(user->userDefined);
        QCOMPARE(user->set, std::string(TrackEditor::kUserSet));
        QCOMPARE(user->id, std::string("u1"));
        QCOMPARE(e.session.undoStack().count(), base + 1);
        // The 10 m minimum and the track bounds are enforced (06 section 13.5 item 4).
        QVERIFY(!editor.addSection(QStringLiteral("Too short"), 300.0, 305.0, QStringLiteral("corner")));
        QVERIFY(editor.lastError().contains(QStringLiteral("10")));
        QVERIFY(
            !editor.addSection(QStringLiteral("Off the end"), 100.0, withUser->lengthM + 50.0, QString()));
        QVERIFY(!editor.addSection(QString(), 100.0, 200.0, QString()));
        QCOMPARE(e.session.undoStack().count(), base + 1);
        // From the cursor span (the shell passes it from the worksheet).
        QVERIFY(!editor.addSectionFromCursorSpan(QStringLiteral("Span")));
        editor.setCursorSpan(std::make_pair(340.0, 300.0));
        QVERIFY(editor.addSectionFromCursorSpan(QStringLiteral("Span")));
        const analysis::TrackSection* span = sectionNamed(*editor.currentMap(), "Span");
        QVERIFY(span != nullptr);
        QCOMPARE(span->startM, 300.0);
        QCOMPARE(span->endM, 340.0);
        // Edit, delete.
        QVERIFY(
            editor.editSection(QStringLiteral("u1"), QStringLiteral("T1 entry"), 160.0, 240.0, QString()));
        QCOMPARE(sectionNamed(*editor.currentMap(), "T1 entry")->startM, 160.0);
        QVERIFY(editor.deleteSection(QStringLiteral("u2")));
        QVERIFY(sectionNamed(*editor.currentMap(), "Span") == nullptr);
        QVERIFY(!editor.deleteSection(QStringLiteral("nope")));

        // AC-M3-04: user sections persist in trackmaps/<venue>.track.json and reload.
        QTRY_COMPARE_WITH_TIMEOUT(e.maps.pendingIo(), 0, 30000);
        {
            std::vector<std::string> log;
            const auto file = project::TrackMapFile::load(e.maps.pathFor(kVenue), log);
            QVERIFY(file.ok());
            const analysis::TrackSection* stored = sectionNamed(file->map, "T1 entry");
            QVERIFY(stored != nullptr && stored->userDefined && stored->startM == 160.0);
            qt::TrackMapService reopened(&e.session);
            QVERIFY(sectionNamed(*reopened.map(kVenue), "T1 entry") != nullptr);
        }

        // Regenerate default sections: one command, the user section kept.
        const int beforeDefaults = e.session.undoStack().count();
        QVERIFY(editor.regenerateDefaultSections());
        QCOMPARE(e.session.undoStack().count(), beforeDefaults + 1);
        QVERIFY(sectionNamed(*editor.currentMap(), "T1 entry") != nullptr);
        // Regenerate the track (Generate + Accept again): the user section is kept as well.
        QVERIFY(generated(editor));
        editor.accept();
        QVERIFY(sectionNamed(*editor.currentMap(), "T1 entry") != nullptr);
        // Undo walks back one step at a time.
        e.session.undoStack().undo();
        e.session.undoStack().undo();
        e.session.undoStack().undo();
        QCOMPARE(sectionNamed(*editor.currentMap(), "T1 entry")->startM, 160.0);
        QVERIFY(sectionNamed(*editor.currentMap(), "Span") != nullptr);
    }

    // D-203 (n): a default section the user edited is theirs (kept by a regeneration, no regenerated default
    // over it); a map write that fails leaves no step on the stack to undo or redo.
    void editedDefaultsSurviveAndFailedWritesLeaveNoStep() {
        EditorWorld e("te-edited");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        QVERIFY(generated(editor));
        editor.accept();
        const auto map = editor.currentMap();
        QVERIFY(map != nullptr);
        const analysis::TrackSection* turn = nullptr;
        for (const analysis::TrackSection& s : map->sections) {
            if (s.category == "corner" && !s.userDefined) {
                turn = &s;
                break;
            }
        }
        QVERIFY(turn != nullptr);
        const QString id = QString::fromStdString(turn->id);
        const double a = turn->startM;
        const double b = turn->endM;
        QVERIFY(editor.editSection(id, QStringLiteral("My corner"), a, b, QString()));
        const analysis::TrackSection* mine = sectionNamed(*editor.currentMap(), "My corner");
        QVERIFY(mine != nullptr && mine->userDefined);
        QVERIFY(editor.regenerateDefaultSections());
        mine = sectionNamed(*editor.currentMap(), "My corner");
        QVERIFY(mine != nullptr);
        for (const analysis::TrackSection& s : editor.currentMap()->sections) {
            if (!s.userDefined && s.set == mine->set) {
                QVERIFY2(!(s.startM < mine->endM && mine->startM < s.endM), s.name.c_str());
            }
        }
        QVERIFY(generated(editor));
        editor.accept(); // regenerate the track: still there
        QVERIFY(sectionNamed(*editor.currentMap(), "My corner") != nullptr);

        // A write that fails (a folder stands where the file goes): reported when it lands, and its step is
        // dropped, so there is none to undo or redo (D-203 (n), on the pool since D-210 (a)).
        QTRY_COMPARE_WITH_TIMEOUT(e.maps.pendingIo(), 0, 30000);
        const auto path = e.maps.pathFor(editor.currentMap()->venue);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::create_directories(path / "blocker", ec);
        QVERIFY(std::filesystem::is_directory(path));
        QUndoStack& stack = e.session.undoStack();
        const int count = stack.count();
        const int index = stack.index();
        QSignalSpy log(&e.maps, &qt::TrackMapService::logMessage);
        QVERIFY(editor.addSection(QStringLiteral("Blocked"), 20.0, 60.0, QStringLiteral("corner")));
        QCOMPARE(stack.count(), count + 1);
        const QUndoCommand* blocked = stack.command(index);
        QVERIFY(blocked != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(blocked->isObsolete(), 30000);
        QVERIFY(std::any_of(log.begin(), log.end(), [](const QList<QVariant>& line) {
            return line.at(0).toString().contains(QStringLiteral("could not be written"));
        }));
        stack.undo(); // drops the obsolete step without running it
        QCOMPARE(stack.count(), count);
        QCOMPARE(stack.index(), index);
        QVERIFY(!stack.canRedo());
        QTRY_COMPARE_WITH_TIMEOUT(e.maps.pendingIo(), 0, 30000);
        std::filesystem::remove_all(path, ec);
    }

    void renameDeleteAndTheProjectOption() {
        EditorWorld e("te-files");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        QVERIFY(generated(editor));
        editor.accept();
        QVERIFY(e.maps.map(kVenue) != nullptr);
        // Rename moves the map to another venue's file, one command.
        QVERIFY(editor.renameVenue(QStringLiteral("Other Circuit")));
        QVERIFY(e.maps.map(kVenue) == nullptr);
        QVERIFY(e.maps.map("Other Circuit") != nullptr);
        QCOMPARE(e.maps.map("Other Circuit")->venue, std::string("Other Circuit"));
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(e.maps.pathFor("Other Circuit")), 30000);
        e.session.undoStack().undo();
        QVERIFY(e.maps.map(kVenue) != nullptr);
        QVERIFY(e.maps.map("Other Circuit") == nullptr);
        QVERIFY(!editor.renameVenue(QString::fromStdString(kVenue)));
        // Delete, undo.
        QVERIFY(editor.deleteMap());
        QTRY_VERIFY_WITH_TIMEOUT(!std::filesystem::exists(e.maps.pathFor(kVenue)), 30000);
        e.session.undoStack().undo();
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(e.maps.pathFor(kVenue)), 30000);
        // "Auto-generate on open": the project option, one command.
        QVERIFY(editor.autoGenerateOnOpen()); // the default
        const int before = e.session.undoStack().index();
        editor.setAutoGenerateOnOpen(false);
        QVERIFY(!editor.autoGenerateOnOpen());
        QCOMPARE(e.session.undoStack().index(), before + 1);
        e.session.undoStack().undo();
        QVERIFY(editor.autoGenerateOnOpen());
    }

    void speedLatGNamesItsMissingInput() {
        EditorWorld e("te-speedlatg");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        editor.setMethod(analysis::TrackMethod::SpeedLatG);
        QCOMPARE(static_cast<int>(editor.method()), static_cast<int>(analysis::TrackMethod::SpeedLatG));
        editor.setLateralChannel(QStringLiteral("G Force Lat"));
        QSignalSpy failed(&editor, &TrackEditor::generationFailed);
        editor.generate();
        // The circuit fixture has no wheel speeds, so no Corr Speed.
        QVERIFY(QTest::qWaitFor([&] { return failed.count() > 0 || editor.waitingForInputs(); }, 20000));
        if (failed.count() == 0) {
            QVERIFY(QTest::qWaitFor([&] { return failed.count() > 0; }, 20000));
        }
        QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("Corr Speed")));
        QVERIFY(editor.preview() == nullptr);
        editor.accept(); // nothing to accept
        QVERIFY(!editor.lastError().isEmpty());
        QCOMPARE(e.session.undoStack().count(), 0);
    }
};

QTEST_MAIN(TstTrackEditor)
#include "tst_track_editor.moc"
