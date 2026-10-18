// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession (06 sections 1, 4 and 18; 02 sections 6.1-6.2; D-019, D-070): the shipped defaults (the
// state of `esses -l <log>`), the per-user scheme and keymap, a new project's folder layout, open/save round
// trips with backups, skipped workbooks, lastSelection and recent sources, and the options the selection
// reads from project.json.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/project/project_file.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <QSignalSpy>
#include <QTest>
#include <fstream>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

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

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::size_t backups(const std::filesystem::path& folder) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder / "backup", ec)) {
        n += e.is_regular_file() ? 1 : 0;
    }
    return n;
}

} // namespace

class TstProjectSession : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void defaultsAreTheShippedDocuments() {
        const auto dir = scratchDir("ps-defaults");
        ProjectSession session(pathsFor(dir / "config"));
        QSignalSpy docs(&session, &ProjectSession::documentsChanged);
        QSignalSpy scheme(&session, &ProjectSession::schemeChanged);
        QVERIFY(session.loadDefaults().ok());
        QCOMPARE(docs.count(), 1);
        QCOMPARE(scheme.count(), 1);
        QVERIFY(!session.isOpen());
        QCOMPARE(session.workbooks().size(), std::size_t{1});
        QCOMPARE(session.workbooks().front().name, std::string("Circuit"));
        QCOMPARE(session.workbooks().front().worksheets.size(), std::size_t{7}); // M1 two + the M3 five
        QCOMPARE(session.activeWorkbook(), 0);
        QCOMPARE(session.scheme()->name, session.shippedScheme(false)->name);
        QCOMPARE(session.shippedScheme(true)->background, std::string("dark"));
        QVERIFY(session.keymap().bindings.size() > 100);
        QCOMPARE(session.profile().id, std::string("circuit"));
        QVERIFY(!session.ruleRunner().rules().empty());
        QVERIFY(session.correctedSpeedEnabled());
        QCOMPARE(session.workbookPath(0),
                 repoRoot() / "data" / "workbooks" / "circuit-default.workbook.json");
        QVERIFY(!session.save().ok()); // nothing to save without a project
        // A broken data directory is an error, not a crash.
        PathOverrides bad;
        bad.configDir = dir / "config";
        bad.dataDir = dir / "no-data";
        ProjectSession broken(*Paths::resolve(bad));
        QVERIFY(!broken.loadDefaults().ok());
    }

    void userSchemeAndKeymap() {
        const auto dir = scratchDir("ps-user");
        writeFile(dir / "config" / "keymap.json", R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1,
            "name": "mine", "inherits": "default",
            "bindings": [{"command": "data.toggle_domain", "keys": ["F10"], "context": "global"}], "unbind": []})");
        SettingsStore settings(dir / "config" / "settings.json");
        ProjectSession session(pathsFor(dir / "config"), &settings);
        QVERIFY(session.loadDefaults().ok());
        const auto rows = session.keymap().rowsFor("data.toggle_domain");
        QCOMPARE(rows.size(), std::size_t{1});
        QCOMPARE(rows.front()->keys, std::vector<std::string>{"F10"});
        session.setScheme(session.shippedScheme(true));
        QCOMPARE(settings.value(QStringLiteral("scheme.active")).toString(),
                 qs(session.shippedScheme(true)->name));
        ProjectSession next(pathsFor(dir / "config"), &settings);
        QVERIFY(next.loadDefaults().ok());
        QCOMPARE(next.scheme()->background, std::string("dark")); // the remembered scheme
    }

    void newProjectSaveAndReopen() {
        const auto dir = scratchDir("ps-new");
        const std::filesystem::path folder = dir / "Bathurst";
        SettingsStore settings(dir / "config" / "settings.json");
        ProjectSession session(pathsFor(dir / "config"), &settings);
        QSignalSpy opened(&session, &ProjectSession::projectOpened);
        QVERIFY(session.newProject(folder, "Bathurst 2026").ok());
        QCOMPARE(opened.count(), 1);
        QVERIFY(session.isOpen());
        QCOMPARE(*session.folder(), folder);
        for (const char* sub : {"workbooks", "channels", "maths", "trackmaps", "backup", ".local"}) {
            QVERIFY2(std::filesystem::is_directory(folder / sub), sub);
        }
        QVERIFY(std::filesystem::exists(folder / "project.json"));
        QVERIFY(std::filesystem::exists(folder / "workbooks" / "Circuit.workbook.json"));
        QCOMPARE(session.projectFile().name, std::string("Bathurst 2026"));
        QCOMPARE(session.projectFile().workbooks, std::vector<std::string>{"Circuit"});
        QVERIFY(session.projectFile().builtinMaths.count(
                    std::string(maths::CorrectedSpeedDistanceModule::kId)) == 1);
        QCOMPARE(settings.recentProjects().value(0), qs(pathToUtf8(folder)));
        QVERIFY(!session.newProject(folder, "again").ok()); // a project exists there

        // Edit a workbook (M1: no undo) and the view state, save, reopen.
        project::WorkbookFile wb = session.workbooks().front();
        wb.worksheets.front().name = "Renamed";
        QSignalSpy docs(&session, &ProjectSession::documentsChanged);
        session.replaceWorkbook(0, wb);
        QCOMPARE(docs.count(), 1);
        project::SelectionKeys keys;
        project::RangeKey k;
        k.source = {"../logs/a.ld", "xxh3:0123456789abcdef0123456789abcdef"};
        k.groupPath = "Outings:Laps";
        k.startMarker = "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f";
        k.name = "Lap 7";
        keys.main = k;
        keys.singleColourMain = true;
        session.setLastSelection(keys);
        QCOMPARE(docs.count(), 1); // view state: no documentsChanged
        QVERIFY(session.save().ok());
        QVERIFY(session.save().ok()); // same bytes: no backup
        wb.worksheets.front().name = "Renamed twice";
        session.replaceWorkbook(0, wb);
        QVERIFY(session.save().ok());
        QVERIFY(backups(folder) >= 1); // D-070: the previous bytes are kept

        ProjectSession reopened(pathsFor(dir / "config"));
        QVERIFY(reopened.openProject(folder / "project.json").ok()); // the file path works too
        QCOMPARE(reopened.workbooks().front().worksheets.front().name, std::string("Renamed twice"));
        QCOMPARE(reopened.projectFile().lastSelection, keys);
        QCOMPARE(reopened.workbookPath(0), folder / "workbooks" / "Circuit.workbook.json");
        QSignalSpy closed(&reopened, &ProjectSession::projectClosed);
        reopened.closeProject();
        QCOMPARE(closed.count(), 1);
        QVERIFY(!reopened.isOpen());
        QCOMPARE(reopened.workbooks().front().name, std::string("Circuit"));
    }

    void unreadableWorkbooksAreSkipped() {
        const auto dir = scratchDir("ps-skip");
        const std::filesystem::path folder = dir / "P";
        {
            ProjectSession session(pathsFor(dir / "config"));
            QVERIFY(session.newProject(folder, "P").ok());
        }
        // project.json names a missing workbook next to the real one.
        std::vector<std::string> log;
        auto pf = project::ProjectFile::load(folder / "project.json", log);
        QVERIFY(pf.ok());
        pf->workbooks = {"Missing", "Circuit"};
        QVERIFY(pf->save(folder / "project.json").ok());
        ProjectSession session(pathsFor(dir / "config"));
        QSignalSpy logged(&session, &ProjectSession::logMessage);
        QVERIFY(session.openProject(folder).ok());
        QCOMPARE(session.workbooks().size(), std::size_t{1});
        QVERIFY(logged.count() >= 1);
        // No readable workbook at all: the shipped default is shown.
        pf->workbooks = {"Missing"};
        QVERIFY(pf->save(folder / "project.json").ok());
        QVERIFY(session.openProject(folder).ok());
        QCOMPARE(session.workbooks().front().name, std::string("Circuit"));
        // Not a project: an error, the session unchanged.
        QVERIFY(!session.openProject(dir / "nowhere").ok());
        QCOMPARE(*session.folder(), folder);
    }

    void recentSourcesAndSelectionOptions() {
        store::ChunkCache cache{std::size_t{64} << 20}; // outlives every holder of the source's columns
        const auto dir = scratchDir("ps-recent");
        const std::filesystem::path folder = dir / "P";
        SettingsStore settings(dir / "config" / "settings.json");
        ProjectSession session(pathsFor(dir / "config"), &settings);
        QVERIFY(session.newProject(folder, "P").ok());
        // Options read by the selection (02 section 6.2): the tolerance and "select fastest lap on load".
        std::vector<std::string> log;
        auto pf = project::ProjectFile::load(folder / "project.json", log);
        QVERIFY(pf.ok());
        auto options =
            JsonDoc::parse(R"({"fastLapTolerancePct": 2.5, "general": {"selectFastestOnLoad": true}})");
        QVERIFY(options.ok());
        pf->options = *options;
        QVERIFY(pf->save(folder / "project.json").ok());
        DataSelectionModel selection(&session);
        QVERIFY(session.openProject(folder).ok());
        QCOMPARE(selection.fastLapTolerancePct(), 2.5);
        QVERIFY(selection.selectFastestOnLoad());
        // A published source: Main is the fastest lap, and lastSelection follows every change.
        SyntheticOptions o;
        o.path = pathToUtf8(folder / "logs" / "a.ld");
        const auto src = makeSource(o, cache);
        selection.sourcePublished(src);
        QCOMPARE(selection.snapshot().main, rangeRef(*src, 2));
        QVERIFY(session.projectFile().lastSelection.main.has_value());
        QCOMPARE(session.projectFile().lastSelection.main->source.path, std::string("logs/a.ld")); // relative
        QCOMPARE(session.projectFile().lastSelection.main->name, std::string("Lap 2"));
        session.addRecentSource(*src);
        session.addRecentSource(*src);
        QCOMPARE(session.projectFile().recentSources.size(), std::size_t{1});
        QCOMPARE(session.projectFile().recentSources.front().hash, "xxh3:" + src->id.hex());
        QCOMPARE(session.projectFile().recentSources.front().path, std::string("logs/a.ld"));
        QCOMPARE(settings.recentLogs().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstProjectSession)
#include "tst_project_session.moc"
