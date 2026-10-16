// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession persistence and recovery (02 sections 6.1-6.8, 06 section 18; D-019, D-070; AC-M2-06 at the
// model level, AC-M2-07 through the session): every project document saves and reopens (channel documents,
// unit maps, maths modules in project order, renamed and removed workbooks retired to backup/), an unreadable
// document is never written over, the journal autosave writes only the documents that differ from disk and
// never touches them, a "killed" session's journal is offered on the next open and replays (the project
// becomes dirty, the disk stays as it was until Save, which clears the journal), the recovery prompt's three
// answers, the autosave timer, and the selection, its single-colour state and the Data-window filters
// restored after a reopen through SessionRestore. D-187 (the M2 review's data-loss cases): a workbook renamed
// only in letter case survives its save, an undone edit is never replayed, a journal put off with Later
// survives this session's edits, autosaves and save, and a save that fails part-way leaves what it was
// saving recoverable. D-190: a save that fails part-way retires no file project.json still names, and an
// autosave whose journal file another instance deleted writes it again.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

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

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::size_t filesIn(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        n += e.is_regular_file() ? 1 : 0;
    }
    return n;
}

// Every project file's bytes (the disk state a journal must never change).
std::map<std::string, std::string> diskState(const std::filesystem::path& folder) {
    std::map<std::string, std::string> out;
    for (const char* sub : {"", "workbooks", "channels", "channels/unitmaps", "maths"}) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(folder / sub, ec)) {
            if (e.is_regular_file()) {
                out[pathToUtf8(e.path().lexically_relative(folder))] = readAll(e.path());
            }
        }
    }
    return out;
}

maths::MathModule userModule(const std::string& id, const std::string& expr) {
    maths::MathModule m;
    m.id = id;
    maths::MathItem item;
    item.name = id + " out";
    item.expr = expr;
    m.items.push_back(std::move(item));
    return m;
}

// A few undoable edits of every channel document, a module and the workbooks.
void editEverything(ProjectSession& s) {
    QUndoStack& stack = s.undoStack();
    project::ChannelOverrides::Entry rpm;
    rpm.dps = 1;
    rpm.displayUnit = std::string("rpm");
    stack.push(new SetOverrideCommand(s, "Engine RPM", rpm));
    project::Aliases aliases = s.aliases();
    aliases.table.rows.push_back(model::AliasRow{"Speed Any", {{"Wheel Speed FL", true}}, -1, std::nullopt});
    stack.push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
    project::Mappings mappings = s.mappings();
    mappings.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
    stack.push(new EditMappingsCommand(s, mappings, QStringLiteral("Mappings")));
    formats::UnitMappingProfile map;
    map.importer = "text.profile:generic.csv";
    map.rules.push_back({formats::UnitMappingProfile::Match::UnitOnly, "", "KPH", "Speed", "km/h", ""});
    stack.push(new SetUnitMapCommand(s, "generic.csv", map));
    stack.push(new EditMathModulesCommand(s, {userModule("user", "'Throttle' * 2"), userModule("extra", "1")},
                                          QStringLiteral("Modules")));
    project::WorkbookFile book = s.workbooks().front();
    book.id = {};
    book.name = "Chassis";
    stack.push(new AddWorkbookCommand(s, book));
}

} // namespace

class TstProjectRecovery : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
    }

    void everyDocumentSavesAndReopens() {
        const auto dir = scratchDir("rec-save");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            s.setUnits(shippedUnits());
            QVERIFY(s.newProject(folder, "Recovery").ok());
            QVERIFY(!s.isDirty());
            QSignalSpy dirty(&s, &ProjectSession::dirtyChanged);
            editEverything(s);
            QVERIFY(s.isDirty());
            QCOMPARE(dirty.count(), 1);
            // Undo back to the saved state is clean again; redo dirty.
            s.undoStack().setIndex(0);
            QVERIFY(!s.isDirty());
            s.undoStack().setIndex(s.undoStack().count());
            QVERIFY(s.isDirty());
            const std::string oldBook = s.workbookNames().front();
            QVERIFY(s.save().ok());
            QVERIFY(!s.isDirty());
            QVERIFY(std::filesystem::exists(folder / "channels" / "overrides.json"));
            QVERIFY(std::filesystem::exists(folder / "channels" / "aliases.json"));
            QVERIFY(std::filesystem::exists(folder / "channels" / "mappings.json"));
            QVERIFY(std::filesystem::exists(folder / "channels" / "unitmaps" / "generic.csv.json"));
            QVERIFY(std::filesystem::exists(folder / "maths" / "user.maths.json"));
            QVERIFY(std::filesystem::exists(folder / "maths" / "extra.maths.json"));
            QVERIFY(std::filesystem::exists(folder / "workbooks" / "Chassis.workbook.json"));
            // Saving again with nothing changed rewrites nothing (no new backup).
            const std::size_t backups = filesIn(folder / "backup");
            QVERIFY(s.save().ok());
            QCOMPARE(filesIn(folder / "backup"), backups);
            // A renamed workbook and a removed module: their old files are retired to backup/.
            s.undoStack().push(new RenameWorkbookCommand(s, 0, "General"));
            std::vector<maths::MathModule> modules = s.mathsModules();
            modules.pop_back();
            s.undoStack().push(new EditMathModulesCommand(s, modules, QStringLiteral("Delete module")));
            QVERIFY(s.save().ok());
            QVERIFY(
                !std::filesystem::exists(folder / "workbooks" / pathFromUtf8(oldBook + ".workbook.json")));
            QVERIFY(std::filesystem::exists(folder / "workbooks" / "General.workbook.json"));
            QVERIFY(!std::filesystem::exists(folder / "maths" / "extra.maths.json"));
            QVERIFY(filesIn(folder / "backup") >= backups + 2);
        }
        ProjectSession r(pathsFor(dir / "config"));
        r.setUnits(shippedUnits());
        QVERIFY(r.openProject(folder).ok());
        QVERIFY(!r.isDirty());
        QCOMPARE(r.undoStack().count(), 0);
        QCOMPARE(r.overrides().find("Engine RPM")->dps, std::optional<int>(1));
        QCOMPARE(r.aliases().table.rows.size(), std::size_t{1});
        QCOMPARE(r.mappings().rows.size(), std::size_t{1});
        QCOMPARE(r.unitMaps().count("generic.csv"), std::size_t{1});
        QCOMPARE(r.mathsModules().size(), std::size_t{1});
        QCOMPARE(r.mathsModules().front().id, std::string("user"));
        QCOMPARE(r.mathsModules().front().items.front().expr, std::string("'Throttle' * 2"));
        QVERIFY(r.projectFile().mathsOrder == std::vector<std::string>{"user"});
        QVERIFY(r.workbookNames() == (std::vector<std::string>{"General", "Chassis"}));
        // The mappings as the resolver's table, the compat switch read from the options.
        QCOMPARE(r.mappingTable().rows.size(), std::size_t{1});
        QVERIFY(!r.mathsCompat().unaryMinusAbovePower);
        QVERIFY(!r.looseChannelSearch());
        // Close clears the stack and the documents.
        r.undoStack().push(new ClearOverrideCommand(r, "Engine RPM"));
        r.closeProject();
        QCOMPARE(r.undoStack().count(), 0);
        QVERIFY(r.overrides().channels.empty());
        QVERIFY(r.mathsModules().empty());
    }

    void unreadableDocumentsAreNeverWrittenOver() {
        const auto dir = scratchDir("rec-broken");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Broken").ok());
        }
        writeFile(folder / "channels" / "aliases.json", "{ not json");
        ProjectSession s(pathsFor(dir / "config"));
        QSignalSpy log(&s, &ProjectSession::logMessage);
        QVERIFY(s.openProject(folder).ok());
        QVERIFY(std::any_of(s.log().begin(), s.log().end(), [](const std::string& l) {
            return l.find("aliases.json") != std::string::npos;
        }));
        project::Aliases aliases;
        aliases.table.rows.push_back(model::AliasRow{"A", {{"B", true}}, -1, std::nullopt});
        s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
        QVERIFY(s.save().ok());
        QCOMPARE(readAll(folder / "channels" / "aliases.json"), std::string("{ not json"));
    }

    // 02 section 6.6 rule 2: a migrated document is dirty and the next save rewrites it (backup of the
    // original).
    void migratedProjectIsRewrittenOnSave() {
        const auto dir = scratchDir("rec-migrate");
        const auto folder = dir / "Project";
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(repoRoot() / "apps" / "cli" / "tests" / "data" / "project-v1.json",
                                   folder / "project.json");
        const std::string original = readAll(folder / "project.json");
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.openProject(folder).ok());
        QVERIFY(s.isDirty());
        QCOMPARE(s.projectFile().schemaVersion, project::ProjectFile::kSchemaVersion);
        QVERIFY(s.save().ok());
        QVERIFY(!s.isDirty());
        const std::string rewritten = readAll(folder / "project.json");
        QVERIFY(rewritten != original);
        QVERIFY(rewritten.find("\"schemaVersion\": 2") != std::string::npos);
        QVERIFY(filesIn(folder / "backup") >= 1);
        ProjectSession r(pathsFor(dir / "config"));
        QVERIFY(r.openProject(folder).ok());
        QVERIFY(!r.isDirty());
    }

    // AC-M2-06: a kill after an autosave leaves the files intact; the journal replays on the next open.
    void autosaveJournalsAndRecoversAfterAKill() {
        const auto dir = scratchDir("rec-kill");
        const auto folder = dir / "Project";
        std::vector<std::pair<std::string, std::string>> edited;
        std::map<std::string, std::string> disk;
        {
            ProjectSession s(pathsFor(dir / "config"));
            s.setUnits(shippedUnits());
            QVERIFY(s.newProject(folder, "Kill").ok());
            QVERIFY(s.save().ok());
            disk = diskState(folder);
            QVERIFY(s.autosave().ok()); // nothing differs from disk: nothing written
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
            editEverything(s);
            project::SelectionKeys keys;
            keys.singleColourMain = true;
            s.setLastSelection(keys); // view state, journaled with project.json
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
            QVERIFY(s.autosave().ok()); // the same documents again: no second entry
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
            project::ChannelOverrides::Entry gear;
            gear.dps = 0;
            s.undoStack().push(new SetOverrideCommand(s, "Gear", gear));
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"),
                     std::size_t{1});           // the newest snapshot supersedes the first
            QVERIFY(diskState(folder) == disk); // autosave never rewrites a project file
            edited = s.documentTexts();
            // "kill -9": the session goes away without save().
        }
        QVERIFY(diskState(folder) == disk);
        ProjectSession r(pathsFor(dir / "config"));
        r.setUnits(shippedUnits());
        QSignalSpy offered(&r, &ProjectSession::recoveryAvailable);
        QVERIFY(r.openProject(folder).ok());
        QCOMPARE(offered.count(), 1);
        QVERIFY(r.hasRecoverableJournal());
        const std::optional<project::JournalReplay> replay = r.journalReplay();
        QVERIFY(replay.has_value());
        QVERIFY(replay->conflicts.empty());
        QVERIFY(
            std::any_of(replay->entries.begin(), replay->entries.end(),
                        [](const project::JournalEntry& e) { return e.path == "channels/overrides.json"; }));
        QVERIFY(!r.isDirty());
        QVERIFY(r.overrides().channels.empty()); // nothing applied before the answer
        // While the prompt is open, autosave writes only this session's documents (none differ yet) and
        // never touches the other session's journal.
        QVERIFY(r.autosave().ok());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        QSignalSpy docs(&r, &ProjectSession::documentsChanged);
        QVERIFY(r.recoverJournal().ok());
        QVERIFY(docs.count() >= 1);
        QVERIFY(r.isDirty());
        QVERIFY(r.documentTexts() == edited);
        QVERIFY(r.projectFile().lastSelection.singleColourMain);
        QVERIFY(diskState(folder) == disk); // still untouched until Save
        // Recover adopts the journal (D-187): this session's own copy replaces the replayed file until Save.
        QVERIFY(!r.hasRecoverableJournal());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        QVERIFY(r.save().ok());
        QVERIFY(!r.hasRecoverableJournal());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
        QVERIFY(!r.isDirty());
        ProjectSession again(pathsFor(dir / "config"));
        again.setUnits(shippedUnits());
        QSignalSpy none(&again, &ProjectSession::recoveryAvailable);
        QVERIFY(again.openProject(folder).ok());
        QCOMPARE(none.count(), 0);
        QVERIFY(again.documentTexts() == edited);
    }

    void recoveryPromptAnswers() {
        const auto dir = scratchDir("rec-prompt");
        const auto folder = dir / "Project";
        const auto crash = [&] {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.openProject(folder).ok() || s.newProject(folder, "Prompt").ok());
            project::ChannelOverrides::Entry e;
            e.dps = 3;
            s.undoStack().push(new SetOverrideCommand(s, "Throttle", e));
            QVERIFY(s.autosave().ok());
        };
        crash();
        // Later: the journal stays untouched; this session's autosave writes its own file beside it (D-187).
        {
            ProjectSession s(pathsFor(dir / "config"));
            int asked = 0;
            s.setRecoveryPrompt([&asked](const project::JournalReplay& replay) {
                ++asked;
                return replay.entries.empty() ? project::JournalRecovery::Discard
                                              : project::JournalRecovery::Later;
            });
            QSignalSpy offered(&s, &ProjectSession::recoveryAvailable);
            QVERIFY(s.openProject(folder).ok());
            QCOMPARE(asked, 1);
            QCOMPARE(offered.count(), 0); // the prompt answered instead
            QVERIFY(s.hasRecoverableJournal());
            QVERIFY(s.overrides().channels.empty());
            const std::size_t journal = filesIn(folder / ".journal");
            project::Mappings mappings;
            mappings.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
            s.undoStack().push(new EditMappingsCommand(s, mappings, QStringLiteral("Mappings")));
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"), journal + 1);
        }
        // Recover through the prompt: both sessions' journals (the crash and the edits made after Later).
        {
            ProjectSession s(pathsFor(dir / "config"));
            s.setRecoveryPrompt(
                [](const project::JournalReplay&) { return project::JournalRecovery::Recover; });
            QVERIFY(s.openProject(folder).ok());
            QVERIFY(s.overrides().find("Throttle") != nullptr);
            QCOMPARE(s.mappings().rows.size(), std::size_t{1});
            QVERIFY(s.isDirty());
            QVERIFY(
                s.discardJournal().ok()); // the answer of a later "do not keep" (the edits stay in memory)
            QVERIFY(!s.hasRecoverableJournal());
        }
        crash();
        // Discard: the journal is deleted and the documents are the disk's.
        {
            ProjectSession s(pathsFor(dir / "config"));
            s.setRecoveryPrompt(
                [](const project::JournalReplay&) { return project::JournalRecovery::Discard; });
            QVERIFY(s.openProject(folder).ok());
            QVERIFY(!s.hasRecoverableJournal());
            QVERIFY(s.overrides().channels.empty());
            QVERIFY(!s.isDirty());
            QCOMPARE(s.recoverJournal().error().code, ErrorCode::NotFound);
        }
    }

    // D-187: a rename that changes only the letter case writes the same file on a case-insensitive file
    // system; the retire step must not delete it (it would leave project.json naming a workbook without a
    // file).
    void caseOnlyRenameKeepsTheWorkbook() {
        const auto dir = scratchDir("rec-case-rename");
        const auto folder = dir / "Project";
        std::string renamed;
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Case").ok());
            const std::string name = s.workbookNames().front();
            renamed = name;
            for (char& c : renamed) {
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                         : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            QVERIFY(renamed != name);
            s.undoStack().push(new RenameWorkbookCommand(s, 0, renamed));
            QVERIFY(s.workbookNames().front() == renamed);
            QVERIFY(s.save().ok());
            QVERIFY(std::filesystem::exists(folder / "workbooks" / pathFromUtf8(renamed + ".workbook.json")));
        }
        ProjectSession r(pathsFor(dir / "config"));
        QVERIFY(r.openProject(folder).ok());
        QVERIFY(r.workbookNames() == std::vector<std::string>{renamed});
        QVERIFY(std::none_of(r.log().begin(), r.log().end(),
                             [](const std::string& l) { return l.find("skipped") != std::string::npos; }));
    }

    // D-187: an edit that was journalled and then undone is not replayed after a crash; only the newest
    // snapshot of a session counts.
    void undoneEditIsNotReplayed() {
        const auto dir = scratchDir("rec-undone");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Undone").ok());
            project::Aliases aliases;
            aliases.table.rows.push_back(model::AliasRow{"A", {{"B", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
            QVERIFY(s.autosave().ok());
            s.undoStack().undo(); // back to the disk's aliases
            project::ChannelOverrides::Entry e;
            e.dps = 2;
            s.undoStack().push(new SetOverrideCommand(s, "Throttle", e));
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
            // killed
        }
        ProjectSession r(pathsFor(dir / "config"));
        r.setRecoveryPrompt([](const project::JournalReplay&) { return project::JournalRecovery::Recover; });
        QVERIFY(r.openProject(folder).ok());
        QVERIFY(r.overrides().find("Throttle") != nullptr);
        QVERIFY(r.aliases().table.rows.empty()); // the undone alias does not come back
        // Undoing every edit leaves nothing to journal: the session's older snapshot goes too.
        ProjectSession t(pathsFor(dir / "config2"));
        const auto folder2 = dir / "Project2";
        QVERIFY(t.newProject(folder2, "Undone 2").ok());
        QVERIFY(t.save().ok()); // the disk now holds every document as the session does
        QVERIFY(t.autosave().ok());
        QCOMPARE(filesIn(folder2 / ".journal"), std::size_t{0});
        project::ChannelOverrides::Entry e;
        e.dps = 1;
        t.undoStack().push(new SetOverrideCommand(t, "Gear", e));
        QVERIFY(t.autosave().ok());
        QCOMPARE(filesIn(folder2 / ".journal"), std::size_t{1});
        t.undoStack().undo();
        QVERIFY(t.autosave().ok());
        QCOMPARE(filesIn(folder2 / ".journal"), std::size_t{0});
    }

    // D-187: Later keeps the other session's journal through this session's edits, autosaves, Save and a
    // crash; only Discard on its prompt removes it.
    void laterSurvivesEditsSavesAndCrashes() {
        const auto dir = scratchDir("rec-later");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Later").ok());
            project::Aliases aliases;
            aliases.table.rows.push_back(model::AliasRow{"Deferred", {{"B", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
            QVERIFY(s.autosave().ok()); // killed with this journal
        }
        const auto later = [](const project::JournalReplay&) { return project::JournalRecovery::Later; };
        {
            // Later, an edit, an autosave, Save: the deferred journal stays.
            ProjectSession s(pathsFor(dir / "config"));
            s.setRecoveryPrompt(later);
            QVERIFY(s.openProject(folder).ok());
            project::ChannelOverrides::Entry e;
            e.dps = 1;
            s.undoStack().push(new SetOverrideCommand(s, "Gear", e));
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{2});
            QVERIFY(s.save().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
            QVERIFY(s.hasRecoverableJournal());
            // Leaving without saving drops this session's journal only.
            project::ChannelOverrides::Entry b;
            b.dps = 4;
            s.undoStack().push(new SetOverrideCommand(s, "Brake", b));
            QVERIFY(s.autosave().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{2});
            QVERIFY(s.discardSessionJournal().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        }
        {
            // Later again, an edit, an autosave, a crash: both journals are recoverable.
            ProjectSession s(pathsFor(dir / "config"));
            s.setRecoveryPrompt(later);
            QVERIFY(s.openProject(folder).ok());
            QVERIFY(s.overrides().find("Gear") != nullptr); // saved by the previous session
            project::ChannelOverrides::Entry e;
            e.dps = 5;
            s.undoStack().push(new SetOverrideCommand(s, "Steer", e));
            QVERIFY(s.autosave().ok());
        }
        ProjectSession r(pathsFor(dir / "config"));
        r.setRecoveryPrompt([](const project::JournalReplay&) { return project::JournalRecovery::Recover; });
        QVERIFY(r.openProject(folder).ok());
        QCOMPARE(r.aliases().table.rows.size(), std::size_t{1}); // the deferred journal
        QVERIFY(r.overrides().find("Gear") != nullptr);          // saved
        QVERIFY(r.overrides().find("Steer") != nullptr);         // the crashed later session's journal
        QVERIFY(r.overrides().find("Brake") == nullptr);         // its journal was discarded
        QVERIFY(r.save().ok());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
    }

    // D-187: a save that fails part-way (a file it cannot write) leaves what it was saving in the journal, so
    // a crash after it never replays an older journal entry over a file the save wrote.
    void failedSaveLeavesItsContentRecoverable() {
        const auto dir = scratchDir("rec-failed-save");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Failing").ok());
            project::Aliases v1;
            v1.table.rows.push_back(model::AliasRow{"V1", {{"B", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, v1, QStringLiteral("Aliases")));
            QVERIFY(s.autosave().ok());
            project::Aliases v2 = v1;
            v2.table.rows.push_back(model::AliasRow{"V2", {{"C", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, v2, QStringLiteral("Aliases")));
            project::Mappings mappings;
            mappings.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
            s.undoStack().push(new EditMappingsCommand(s, mappings, QStringLiteral("Mappings")));
            // channels/mappings.json cannot be written (a directory stands in its place); aliases is written
            // before it (02 section 6.8 rule 2 writes project.json last, the channel documents in order).
            std::filesystem::create_directories(folder / "channels" / "mappings.json");
            QVERIFY(!s.save().ok());
            QVERIFY(readAll(folder / "channels" / "aliases.json").find("V2") != std::string::npos);
            // killed
        }
        std::filesystem::remove_all(folder / "channels" / "mappings.json");
        ProjectSession r(pathsFor(dir / "config"));
        project::JournalReplay shown;
        r.setRecoveryPrompt([&shown](const project::JournalReplay& replay) {
            shown = replay;
            return project::JournalRecovery::Recover;
        });
        QVERIFY(r.openProject(folder).ok());
        // D-190: what the failed save did write is on disk already: not replayed, and no conflict.
        QVERIFY(shown.conflicts.empty());
        QCOMPARE(shown.applied, std::vector<std::string>{"channels/aliases.json"});
        // project.json was not written after the failure (02 section 6.8 rule 2): it is replayed.
        QVERIFY(std::none_of(shown.entries.begin(), shown.entries.end(), [](const project::JournalEntry& e) {
            return e.path == "channels/aliases.json";
        }));
        QCOMPARE(r.aliases().table.rows.size(), std::size_t{2}); // v2, not the older autosave's v1
        QCOMPARE(r.mappings().rows.size(), std::size_t{1});
        QVERIFY(r.save().ok());
        QVERIFY(readAll(folder / "channels" / "aliases.json").find("V2") != std::string::npos);
    }

    // D-190: a save killed after its writes and before deleting its journal leaves a journal whose every
    // document is on disk: it is deleted at the next open without a prompt, with or without the shell's.
    void savedJournalIsNotOffered() {
        const auto dir = scratchDir("rec-saved-journal");
        const auto folder = dir / "Project";
        std::filesystem::path kept;
        std::string keptBytes;
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Saved").ok());
            project::Aliases aliases;
            aliases.table.rows.push_back(model::AliasRow{"Saved", {{"B", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
            QVERIFY(s.autosave().ok()); // the same snapshot the save writes ahead
            for (const auto& e : std::filesystem::directory_iterator(folder / ".journal")) {
                kept = e.path();
            }
            keptBytes = readAll(kept);
            QVERIFY(s.save().ok());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
        }
        for (const bool withPrompt : {true, false}) {
            writeFile(kept, keptBytes); // killed before the journal was deleted
            ProjectSession r(pathsFor(dir / "config"));
            int prompts = 0;
            if (withPrompt) {
                r.setRecoveryPrompt([&prompts](const project::JournalReplay&) {
                    ++prompts;
                    return project::JournalRecovery::Recover;
                });
            }
            QSignalSpy available(&r, &ProjectSession::recoveryAvailable);
            QVERIFY(r.openProject(folder).ok());
            QCOMPARE(prompts, 0);
            QCOMPARE(available.count(), 0);
            QVERIFY(!r.isDirty());
            QVERIFY(!r.hasRecoverableJournal());
            QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
            QCOMPARE(r.aliases().table.rows.size(), std::size_t{1});
        }
    }

    // D-190: the D-070 copy is made before the file is replaced; a copy that cannot be made is reported and
    // the saved document gets its baseline (it is on disk), so the next autosave journals nothing for it.
    void failedBackupStillSaves() {
        const auto dir = scratchDir("rec-backup-fails");
        const auto folder = dir / "Project";
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.newProject(folder, "Backup").ok());
        project::Aliases v1;
        v1.table.rows.push_back(model::AliasRow{"V1", {{"B", true}}, -1, std::nullopt});
        s.undoStack().push(new EditAliasesCommand(s, v1, QStringLiteral("Aliases")));
        QVERIFY(s.save().ok());
        std::filesystem::remove_all(folder / "backup");
        writeFile(folder / "backup", "a file where the backup folder belongs");
        project::Aliases v2 = v1;
        v2.table.rows.push_back(model::AliasRow{"V2", {{"C", true}}, -1, std::nullopt});
        s.undoStack().push(new EditAliasesCommand(s, v2, QStringLiteral("Aliases")));
        QSignalSpy logged(&s, &ProjectSession::logMessage);
        QVERIFY(s.save().ok());
        QVERIFY(!s.isDirty());
        QVERIFY(readAll(folder / "channels" / "aliases.json").find("V2") != std::string::npos);
        bool reported = false;
        for (const QList<QVariant>& args : logged) {
            reported = reported || args.at(0).toString().contains(QStringLiteral("without its backup copy"));
        }
        QVERIFY(reported);
        QVERIFY(s.autosave().ok());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
    }

    // D-190: the replay shown by the prompt is the one Recover applies (one replay per open).
    void promptReplayIsTheOneRecovered() {
        const auto dir = scratchDir("rec-replay-once");
        const auto folder = dir / "Project";
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Once").ok());
            project::Aliases aliases;
            aliases.table.rows.push_back(model::AliasRow{"Shown", {{"B", true}}, -1, std::nullopt});
            s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
            QVERIFY(s.autosave().ok());
            // killed
        }
        ProjectSession r(pathsFor(dir / "config"));
        QSignalSpy available(&r, &ProjectSession::recoveryAvailable);
        QVERIFY(r.openProject(folder).ok());
        QCOMPARE(available.count(), 1);
        const std::optional<project::JournalReplay> shown = r.journalReplay();
        QVERIFY(shown.has_value());
        QVERIFY(std::any_of(shown->entries.begin(), shown->entries.end(), [](const project::JournalEntry& e) {
            return e.path == "channels/aliases.json";
        }));
        // Another instance journals a newer version while the prompt is up.
        {
            project::Aliases other;
            other.table.rows.push_back(model::AliasRow{"Later", {{"C", true}}, -1, std::nullopt});
            project::Journal third(folder, uuidRandom());
            Result<project::JournalEntry> e =
                project::JournalEntry::of("channels/aliases.json", other.toJson(), {});
            QVERIFY(e.ok());
            QVERIFY(third.append({*e}).ok());
        }
        QVERIFY(r.recoverJournal().ok());
        QCOMPARE(r.aliases().table.rows.size(), std::size_t{1});
        QCOMPARE(r.aliases().table.rows.front().name, std::string("Shown"));
    }

    // D-190 (amends D-177 (a)): the Maths Editor's save on close writes the modules and their order only; the
    // project's other edits stay unsaved, undoable and journalled.
    void saveMathsModulesLeavesOtherEdits() {
        const auto dir = scratchDir("rec-maths-only");
        const auto folder = dir / "Project";
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.newProject(folder, "Maths").ok());
        const std::string projectBefore = readAll(folder / "project.json");
        project::ChannelOverrides::Entry rpm;
        rpm.dps = 1;
        s.undoStack().push(new SetOverrideCommand(s, "Engine RPM", rpm));
        project::ProjectFile pf = s.projectFile();
        pf.name = "Renamed Elsewhere";
        s.undoStack().push(new SetProjectOptionsCommand(s, pf, QStringLiteral("Options")));
        QVERIFY(s.autosave().ok());
        s.undoStack().push(
            new EditMathModulesCommand(s, {userModule("user", "'Throttle' * 2")}, QStringLiteral("Modules")));
        QVERIFY(s.saveMathsModules().ok());
        QVERIFY(readAll(folder / "maths" / "user.maths.json").find("Throttle") != std::string::npos);
        const std::string projectAfter = readAll(folder / "project.json");
        QVERIFY(projectAfter.find("\"user\"") != std::string::npos);          // the module order
        QVERIFY(projectAfter.find("Renamed Elsewhere") == std::string::npos); // not the other edit
        QVERIFY(projectAfter != projectBefore);
        QVERIFY(!std::filesystem::exists(folder / "channels" / "overrides.json"));
        QVERIFY(s.isDirty());
        QVERIFY(filesIn(folder / ".journal") >= 1); // the other edits stay recoverable
        // A kill now recovers the other edits, and the saved module is not replayed over.
        {
            ProjectSession r(pathsFor(dir / "config"));
            project::JournalReplay shown;
            r.setRecoveryPrompt([&shown](const project::JournalReplay& replay) {
                shown = replay;
                return project::JournalRecovery::Later;
            });
            QVERIFY(r.openProject(folder).ok());
            QVERIFY(shown.conflicts.empty());
            bool overrides = false;
            for (const project::JournalEntry& e : shown.entries) {
                overrides = overrides || e.path == "channels/overrides.json";
                QVERIFY(!e.path.starts_with("maths/"));
            }
            QVERIFY(overrides);
        }
        // Undo still reaches the other edits; a full save writes them.
        s.undoStack().undo(); // the module edit (already on disk)
        s.undoStack().redo();
        QVERIFY(s.save().ok());
        QVERIFY(std::filesystem::exists(folder / "channels" / "overrides.json"));
        QVERIFY(readAll(folder / "project.json").find("Renamed Elsewhere") != std::string::npos);
        QVERIFY(!s.isDirty());
        // A module edit alone leaves the session clean after saveMathsModules.
        s.undoStack().push(new EditMathModulesCommand(
            s, {userModule("user", "'Throttle' * 3"), userModule("extra", "1")}, QStringLiteral("Modules")));
        QVERIFY(s.saveMathsModules().ok());
        QVERIFY(!s.isDirty());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
        // Removing a module retires its file once project.json no longer names it.
        s.undoStack().push(
            new EditMathModulesCommand(s, {userModule("user", "'Throttle' * 3")}, QStringLiteral("Modules")));
        QVERIFY(s.saveMathsModules().ok());
        QVERIFY(!std::filesystem::exists(folder / "maths" / "extra.maths.json"));
        QVERIFY(readAll(folder / "project.json").find("\"extra\"") == std::string::npos);
        QVERIFY(!s.isDirty());
    }

    // D-190 (a), found in its verification: a save that fails part-way retires nothing. project.json was not
    // written and still names the renamed workbook, whose file stays in place until a save that succeeds.
    void failedSaveRetiresNothing() {
        const auto dir = scratchDir("rec-failed-retire");
        const auto folder = dir / "Project";
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.newProject(folder, "Retire").ok());
        const std::string before = s.workbookNames().front();
        QVERIFY(before != "Renamed");
        const auto oldFile = folder / "workbooks" / pathFromUtf8(before + ".workbook.json");
        QVERIFY(std::filesystem::exists(oldFile));
        const std::string oldBytes = readAll(oldFile);
        const auto namedOnDisk = [&folder] {
            std::vector<std::string> log;
            Result<project::ProjectFile> pf = project::ProjectFile::load(folder / "project.json", log);
            return pf ? pf->workbooks : std::vector<std::string>{"<unreadable>"};
        };
        const auto backedUp = [&folder, &before] {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(folder / "backup", ec)) {
                if (pathToUtf8(e.path().filename()).starts_with(before + ".workbook.")) {
                    return true;
                }
            }
            return false;
        };
        s.undoStack().push(new RenameWorkbookCommand(s, 0, "Renamed"));
        project::Aliases aliases;
        aliases.table.rows.push_back(model::AliasRow{"A", {{"B", true}}, -1, std::nullopt});
        s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
        // channels/aliases.json cannot be written (a directory stands in its place); the renamed workbook's
        // file is written before it.
        std::filesystem::create_directories(folder / "channels" / "aliases.json");
        QVERIFY(!s.save().ok());
        QVERIFY(std::filesystem::exists(folder / "workbooks" / "Renamed.workbook.json"));
        QVERIFY(std::filesystem::exists(oldFile));
        QCOMPARE(readAll(oldFile), oldBytes);
        QVERIFY(namedOnDisk() == std::vector<std::string>{before});
        QVERIFY(!backedUp());
        QVERIFY(s.isDirty());
        // Leaving with Discard (another session discards the journal): the project on disk is whole.
        {
            ProjectSession other(pathsFor(dir / "config"));
            other.setRecoveryPrompt(
                [](const project::JournalReplay&) { return project::JournalRecovery::Discard; });
            QVERIFY(other.openProject(folder).ok());
            QVERIFY(other.workbookNames() == std::vector<std::string>{before});
            for (const std::string& line : other.log()) { // every workbook project.json names has its file
                QVERIFY2(!line.starts_with("workbook '") || line.find("skipped") == std::string::npos,
                         line.c_str());
            }
        }
        // Once every file can be written, the save completes and the old file is retired to backup/.
        std::filesystem::remove_all(folder / "channels" / "aliases.json");
        QVERIFY(s.save().ok());
        QVERIFY(!s.isDirty());
        QVERIFY(namedOnDisk() == std::vector<std::string>{"Renamed"});
        QVERIFY(!std::filesystem::exists(oldFile));
        QVERIFY(backedUp());
    }

    // D-190 (d), found in its verification: another instance that answers Discard deletes this session's
    // journal file; the next autosave writes the unchanged set again instead of skipping it.
    void autosaveRewritesADeletedJournal() {
        const auto dir = scratchDir("rec-discarded");
        const auto folder = dir / "Project";
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.newProject(folder, "Discarded").ok());
        project::Aliases aliases;
        aliases.table.rows.push_back(model::AliasRow{"Unsaved", {{"B", true}}, -1, std::nullopt});
        s.undoStack().push(new EditAliasesCommand(s, aliases, QStringLiteral("Aliases")));
        QVERIFY(s.autosave().ok());
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        QVERIFY(s.autosave().ok()); // unchanged, its file in place: nothing written
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        {
            ProjectSession other(pathsFor(dir / "config"));
            other.setRecoveryPrompt(
                [](const project::JournalReplay&) { return project::JournalRecovery::Discard; });
            QVERIFY(other.openProject(folder).ok());
        }
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{0});
        QVERIFY(s.autosave().ok()); // no new edit, but the file is gone: written again
        QCOMPARE(filesIn(folder / ".journal"), std::size_t{1});
        // A kill now still recovers the unsaved edit.
        ProjectSession r(pathsFor(dir / "config"));
        r.setRecoveryPrompt([](const project::JournalReplay&) { return project::JournalRecovery::Recover; });
        QVERIFY(r.openProject(folder).ok());
        QCOMPARE(r.aliases().table.rows.size(), std::size_t{1});
        QCOMPARE(r.aliases().table.rows.front().name, std::string("Unsaved"));
    }

    void autosaveTimerFollowsTheOption() {
        const auto dir = scratchDir("rec-timer");
        const auto folder = dir / "Project";
        ProjectSession s(pathsFor(dir / "config"));
        QVERIFY(s.newProject(folder, "Timer").ok());
        QCOMPARE(s.autosaveInterval(), std::chrono::milliseconds(std::chrono::minutes(2))); // the default
        s.setAutosaveInterval(std::chrono::milliseconds(20));
        project::ChannelOverrides::Entry e;
        e.dps = 2;
        s.undoStack().push(new SetOverrideCommand(s, "Throttle", e));
        QTRY_COMPARE_WITH_TIMEOUT(filesIn(folder / ".journal"), std::size_t{1}, 10000);
        // options.general.autosaveMinutes = 0 turns it off (an undoable options edit).
        s.setAutosaveInterval(std::chrono::milliseconds(0));
        project::ProjectFile pf = s.projectFile();
        QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(pf.options.compact())).object();
        QJsonObject general = o.value(QStringLiteral("general")).toObject();
        general.insert(QStringLiteral("autosaveMinutes"), 0);
        o.insert(QStringLiteral("general"), general);
        pf.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
        s.undoStack().push(new SetProjectOptionsCommand(s, pf, QStringLiteral("Options")));
        QCOMPARE(s.autosaveInterval().count(), 0);
        s.undoStack().undo();
        QCOMPARE(s.autosaveInterval(), std::chrono::milliseconds(std::chrono::minutes(2)));
    }

    // AC-M2-06: the project reopens with the previous selection, single-colour state and Data-window filter.
    void selectionFiltersAndSingleColourSurviveReopen() {
        const auto dir = scratchDir("rec-selection");
        const auto folder = dir / "Project";
        AppRuntimeOptions options;
        options.workers = 4;
        options.cacheBudget = std::size_t{256} << 20;
        AppRuntime rt(options);
        SyntheticOptions so;
        so.path = "restore.ld";
        const auto written = makeSource(so, rt.cache());
        std::filesystem::create_directories(folder);
        const std::filesystem::path ld = writeLd(*written, folder, "restore");
        model::RangeRef lap2;
        {
            ProjectSession s(pathsFor(dir / "config"));
            QVERIFY(s.newProject(folder, "Restore").ok());
            SourceLoader loader(rt.pool(), rt.dispatcher(), shippedUnits());
            loader.setUseSidecars(false);
            DataSelectionModel selection(&s);
            connectSourceLoader(loader, selection, nullptr);
            QSignalSpy published(&loader, &SourceLoader::sourcePublished);
            loader.open(ld);
            QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
            const auto src = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
            s.addRecentSource(*src);
            lap2 = rangeRef(*src, 2);
            selection.setMain(lap2);
            selection.toggleOverlay(rangeRef(*src, 3));
            selection.setSingleColourMain(true);
            FilterSpec bad;
            bad.kind = FilterSpec::Kind::LapNumbers;
            bad.laps = "2-";
            QSignalSpy rejected(&selection, &DataSelectionModel::filterRejected);
            selection.setFilter(src->id, bad);
            QCOMPARE(rejected.count(), 1);
            FilterSpec laps;
            laps.kind = FilterSpec::Kind::LapNumbers;
            laps.laps = "1-3";
            selection.setFilter(src->id, laps);
            QVERIFY(s.save().ok());
            loader.closeAll();
        }
        ProjectSession s(pathsFor(dir / "config"));
        SourceLoader loader(rt.pool(), rt.dispatcher(), shippedUnits());
        loader.setUseSidecars(false);
        DataSelectionModel selection(&s);
        connectSourceLoader(loader, selection, nullptr);
        QVERIFY(s.openProject(folder).ok());
        SessionRestore restore(s, loader, selection);
        QSignalSpy finished(&restore, &SessionRestore::finished);
        QCOMPARE(restore.start(), 1); // the project's previous data (options.general.loadPreviousData)
        QVERIFY(restore.running());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY(!restore.running());
        QVERIFY(selection.hasMain());
        QCOMPARE(selection.snapshot().main, lap2);
        QCOMPARE(selection.snapshot().overlays.size(), std::size_t{1});
        QVERIFY(selection.snapshot().singleColourMain);
        const FilterSpec f = selection.filter(lap2.src);
        QCOMPARE(f.kind, FilterSpec::Kind::LapNumbers);
        QCOMPARE(f.laps, std::string("1-3"));
        // The filters live in project.json dataWindow under the source's hash text (D-162).
        const QJsonObject dw =
            QJsonDocument::fromJson(QByteArray::fromStdString(s.projectFile().dataWindow.compact())).object();
        QVERIFY(dw.value(QStringLiteral("filters"))
                    .toObject()
                    .contains(QStringLiteral("xxh3:") + QString::fromStdString(lap2.src.hex())));
        QCOMPARE(dw.value(QStringLiteral("filters"))
                     .toObject()
                     .value(QStringLiteral("default"))
                     .toObject()
                     .value(QStringLiteral("kind")),
                 QJsonValue(QStringLiteral("all")));
    }
};

QTEST_GUILESS_MAIN(TstProjectRecovery)
#include "tst_project_recovery.moc"
