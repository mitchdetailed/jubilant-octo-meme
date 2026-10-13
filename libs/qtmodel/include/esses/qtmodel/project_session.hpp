// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession (06 sections 1, 4 and 18; 02 section 6; D-019, D-032): the open workspace (a project folder)
// and its documents: project.json, the workbooks, the channel documents (overrides-1, aliases-1, mappings-1,
// the unit maps), the maths modules in project order, and the active scheme, keymap and profile. With no
// project open it serves the shipped defaults (data/workbooks/circuit-default.workbook.json, the light/dark
// scheme, the default keymap, the Circuit profile), which is how `esses -l <log>` opens (07 section 4.5).
// Every user-visible document edit is a QUndoCommand on undoStack() (D-032, Qt Gui's undo framework allowed
// here by D-161; undo_commands.hpp); the commands write through the document setters, which emit
// documentsChanged(). View state (selection, filters, the lap-stretch toggle, MRU) is written without the
// stack. GUI thread only.
//
// Loading and saving (02 sections 6.1-6.6): openProject() reads project.json, workbooks/<name>.workbook.json
// in project order (an unreadable one is skipped and logged; none at all falls back to the shipped workbook),
// channels/{overrides,aliases,mappings}.json (absent: empty documents), channels/unitmaps/*.json and
// maths/<id>.maths.json in mathsOrder, and the profile named by the project. A document that exists but
// cannot be read is logged and never written over by save(). save() writes every document whose canonical
// text differs from the file it was read from (D-070 backup of the previous file), removes the workbook,
// unit-map and module files the project no longer names (a backup copy is kept), and clears the journal last
// (02 section 6.8 rule 2). The undo stack is cleared on open and close.
//
// Journal (02 section 6.8, AC-M2-06): every options.general.autosaveMinutes (default 2, 0 = off) autosave()
// appends the full canonical text of every document that differs from disk to <project>/.journal/ (nothing
// when the set is unchanged since the last entry). On open, a pending journal is offered for recovery: with a
// recovery prompt set (setRecoveryPrompt) it is asked at once; otherwise recoveryAvailable() fires after
// projectOpened() and the shell answers with recoverJournal() (the newest entry per document is applied in
// memory, the project becomes dirty, the journal stays until save()) or discardJournal(). Autosave is
// suspended while such a decision is outstanding, so an unanswered prompt leaves the journal untouched.
//
// Additions beyond 06: setLastSelection (DataSelectionModel keeps lastSelection current), addRecentSource,
// ruleRunner (the profile's rules), correctedSpeedSettings/Enabled (project.json builtinMaths),
// shippedScheme, log and logMessage() (Messages, origin Project); M2: WorkbookSet/setWorkbookSet (workbook
// CRUD), setDataWindow (Data-window filters, view state), setLapStretchEnabled (the toolbar toggle, view
// state), setUnits, mappingTable/aliasTable/mathsCompat/lapStretchSettings/looseChannelSearch,
// setRecoveryPrompt, journalReplay, setAutosaveInterval, documentTexts, lapStretchChanged(), dirtyChanged().
#pragma once

#include <esses/analysis/lap_stretch.hpp>
#include <esses/analysis/rule_runner.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/result.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/project/aliases.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/project/journal.hpp>
#include <esses/project/keymap.hpp>
#include <esses/project/mappings.hpp>
#include <esses/project/profile.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QTimer;

namespace esses::qt {

class SettingsStore;

// The workbooks of a session with their file names (project.json "workbooks",
// workbooks/<name>.workbook.json): the document of the workbook CRUD commands (add, remove, rename, reorder).
struct WorkbookSet {
    std::vector<project::WorkbookFile> workbooks;
    std::vector<std::string> names;
    friend bool operator==(const WorkbookSet&, const WorkbookSet&) = default;
};

class ProjectSession final : public QObject {
    Q_OBJECT
public:
    // Answers the recovery prompt (06 section 18): "Recover unsaved changes?".
    using RecoveryPrompt = std::function<project::JournalRecovery(const project::JournalReplay&)>;

    ProjectSession(Paths paths, SettingsStore* settings = nullptr, QObject* parent = nullptr);
    ~ProjectSession() override;

    // The shipped defaults from paths.dataDir() (no project open). Io/Format/Invalid when a shipped document
    // is missing or malformed.
    Result<void> loadDefaults();
    Result<void> openProject(const std::filesystem::path& folder);
    Result<void> newProject(const std::filesystem::path& folder, const std::string& name);
    Result<void> save();
    // The maths modules only (the Maths Editor's save on close, D-177 (a) as amended by D-190): the module
    // files that differ from disk (D-070 backups), the module order written into project.json as it is on
    // disk (the session's other project.json edits stay unsaved), and the files of removed modules retired.
    // Every other document stays as it is, dirty or not, and so do the undo stack and the journal (written
    // ahead first, as by save()); only a session left with nothing unsaved becomes clean and deletes its
    // journal. Errors as save(); Invalid with no project open.
    Result<void> saveMathsModules();
    void closeProject();

    bool isOpen() const noexcept { return folder_.has_value(); }
    const std::optional<std::filesystem::path>& folder() const noexcept { return folder_; }
    const Paths& paths() const noexcept { return paths_; }

    const project::ProjectFile& projectFile() const noexcept { return project_; }
    const std::vector<project::WorkbookFile>& workbooks() const noexcept { return workbooks_; }
    const std::vector<std::string>& workbookNames() const noexcept { return workbookNames_; }
    int activeWorkbook() const noexcept { return activeWorkbook_; }
    void setActiveWorkbook(int index); // view state
    // A workbook edit (the setter of WorkbookCommand); marks the session dirty.
    void replaceWorkbook(int index, project::WorkbookFile workbook);
    std::shared_ptr<const project::Scheme> scheme() const noexcept { return scheme_; }
    void setScheme(std::shared_ptr<const project::Scheme> scheme); // scheme selection: view state
    const project::Keymap& keymap() const noexcept { return keymap_; }
    const project::Profile& profile() const noexcept { return profile_; }
    // The shipped light or dark scheme (null before loadDefaults()).
    std::shared_ptr<const project::Scheme> shippedScheme(bool dark) const noexcept {
        return dark ? dark_ : light_;
    }
    // The file of workbook `index`: <project>/workbooks/<name>.workbook.json, or the shipped file.
    std::filesystem::path workbookPath(int index) const;
    // lastSelection for the next save (view state: no documentsChanged, D-032).
    void setLastSelection(project::SelectionKeys keys);
    // project.json recentSources (newest first, by hash) and the per-user MRU of logs.
    void addRecentSource(const model::DataSource& source);
    // The profile's range rules from <data>/rules (unknown kinds skipped), else the built-in circuit rules.
    analysis::RuleRunner ruleRunner(std::vector<std::string>* skipped = nullptr) const;
    // project.json builtinMaths["esses.corrected_speed_distance"]: settings (null = defaults) and enabled.
    model::OpaqueJson correctedSpeedSettings() const;
    bool correctedSpeedEnabled() const;
    // The messages of the last load (log lines of the document readers, skipped workbooks).
    const std::vector<std::string>& log() const noexcept { return log_; }

    // ---- M2 (06 section 1 rule 4, 02 sections 6.3, 6.8 and 7; D-019, D-032, D-161) ------------------------
    // Every user-visible mutation of a project document is a QUndoCommand pushed here (undo_commands.hpp);
    // view state never is. Cleared when a project opens or closes.
    QUndoStack& undoStack() noexcept { return undoStack_; }
    const QUndoStack& undoStack() const noexcept { return undoStack_; }

    const project::ChannelOverrides& overrides() const noexcept { return overrides_; }
    const project::Aliases& aliases() const noexcept { return aliases_; }
    const project::Mappings& mappings() const noexcept { return mappings_; }
    // channels/unitmaps/<importerStem>.json by importer stem (formats::UnitMappingProfile::fileStem).
    const std::map<std::string, formats::UnitMappingProfile, std::less<>>& unitMaps() const noexcept {
        return unitMaps_;
    }
    // maths/<module>.maths.json in project order (project.json mathsOrder).
    const std::vector<maths::MathModule>& mathsModules() const noexcept { return mathsModules_; }
    WorkbookSet workbookSet() const { return WorkbookSet{workbooks_, workbookNames_}; }

    // The unit table the maths modules are read and written with and the mappings are resolved against;
    // without one, <data>/units.json is loaded on first use.
    void setUnits(std::shared_ptr<const units::UnitRegistry> units);
    std::shared_ptr<const units::UnitRegistry> units() const;
    // The resolver tables of the project's Mappings and Aliases (unit symbols resolved through units()).
    model::MappingTable mappingTable() const;
    model::AliasTable aliasTable() const { return aliases_.table; }
    // options.maths.compat (D-020).
    maths::MathsCompat mathsCompat() const;
    // options.lapStretch (D-016).
    analysis::LapStretchSettings lapStretchSettings() const { return project_.lapStretchSettings(); }
    // options.general.looseChannelSearch (06 section 7 item 3: substring instead of word-prefix search).
    bool looseChannelSearch() const;

    // Document writes used by the undo commands' redo()/undo(); each marks the session dirty and emits
    // documentsChanged(). Not undoable themselves: push a command instead. setProjectFile keeps the session's
    // view state (lastSelection, recentSources, dataWindow) and the members derived from other documents
    // (workbooks, mathsOrder), so undoing an options edit never rewinds them.
    void setProjectFile(project::ProjectFile doc);
    void setOverrides(project::ChannelOverrides doc);
    void setAliases(project::Aliases doc);
    void setMappings(project::Mappings doc);
    // A profile without rules, importer and unknown members removes the unit map of `stem`.
    void setUnitMap(const std::string& stem, formats::UnitMappingProfile doc);
    void setMathsModules(std::vector<maths::MathModule> modules);
    // Workbook CRUD: the whole set (names and documents); the active workbook index is clamped.
    void setWorkbookSet(WorkbookSet set);

    // View state persisted in project.json without the undo stack (02 section 7: filters; 06 section 2.1
    // toolbar): the Data-window filters (dataWindow, 06 section 5.4) and the lap-stretch toggle
    // (options.lapStretch.enabled). setDataWindow emits nothing; setLapStretchEnabled emits
    // lapStretchChanged().
    void setDataWindow(JsonDoc dataWindow);
    void setLapStretchEnabled(bool enabled);

    // ---- journal (02 section 6.8, AC-M2-06, D-187)
    // ---------------------------------------------------------------- Appends the documents that differ from
    // disk to <project>/.journal/ as this session's file (its older files are superseded). Invalid with no
    // project open; ok and nothing written when nothing differs or the set equals the last entry. A journal
    // of another session whose recovery was put off (Later) is never touched.
    Result<void> autosave();
    // Another session's journal is pending on disk for the open project (this session's own autosave files
    // are not recoverable to it).
    bool hasRecoverableJournal() const;
    // The pending journal's replay (paths, conflicts, log) for the recovery prompt; nullopt when none. The
    // replay made when the project opened is reused until it is answered (D-190).
    std::optional<project::JournalReplay> journalReplay() const;
    // Applies the other sessions' journal in memory (the project becomes dirty, the undo stack is cleared,
    // documentsChanged() fires) and adopts it: this session journals the recovered documents as its own and
    // the replayed files are deleted, so they hold until save() and are never replayed twice. The replay is
    // the one journalReplay() showed. NotFound when no journal is pending. A journal that holds only what the
    // files already hold (a save stopped before deleting it) is deleted at open without being offered.
    Result<void> recoverJournal();
    // Deletes the other sessions' journal (the answer Discard to the prompt); this session's own autosave
    // stays.
    Result<void> discardJournal();
    // Deletes this session's own journal files (leaving the project without saving: the edits are dropped,
    // and so is their autosave); a journal put off with Later stays for the next open.
    Result<void> discardSessionJournal();
    // When set, openProject() asks it at once instead of emitting recoveryAvailable() (Later keeps the
    // journal for the next open while this session autosaves its own edits beside it).
    void setRecoveryPrompt(RecoveryPrompt prompt) { recoveryPrompt_ = std::move(prompt); }
    // The autosave timer interval; zero (the default) follows options.general.autosaveMinutes.
    void setAutosaveInterval(std::chrono::milliseconds interval);
    std::chrono::milliseconds autosaveInterval() const;
    // Edits since the last open or save (the undo stack's clean state counts as saved).
    bool isDirty() const noexcept { return dirty_; }
    // Every project document as (project-relative path, canonical text), project.json first; the texts the
    // journal and save() write.
    std::vector<std::pair<std::string, std::string>> documentTexts() const;

Q_SIGNALS:
    void documentsChanged();
    void schemeChanged();
    void projectOpened();
    void projectClosed();
    void logMessage(const QString& text);
    // A non-empty journal was found on open and no recovery prompt is set (the shell asks "Recover unsaved
    // changes?" and calls recoverJournal() or discardJournal()).
    void recoveryAvailable();
    void lapStretchChanged(bool enabled);
    // isDirty() changed.
    void dirtyChanged(bool dirty);

private:
    struct Baseline {                // what a project document's file held at the last load or save
        std::optional<Hash128> disk; // xxh3 of the file bytes (the journal's baseHash); nullopt: absent
        Hash128 text;                // xxh3 of the document's canonical text as read
        bool failed = false;         // the file exists but could not be read: never written over
    };
    struct Loaded; // the documents of one load (project_session.cpp)

    // The default workbook of `profile` (the session's profile when null), else the shipped circuit workbook.
    Result<project::WorkbookFile> loadShippedWorkbook(std::vector<std::string>& log,
                                                      const project::Profile* profile = nullptr) const;
    void publishLog(std::vector<std::string> log);
    Result<Loaded> loadDocuments(const std::filesystem::path& folder,
                                 const std::map<std::string, std::string, std::less<>>* overlay,
                                 std::vector<std::string>& log) const;
    void adopt(Loaded&& loaded);
    void markDirty();
    void setDirty(bool dirty);
    void restartAutosaveTimer();
    void offerRecovery();
    // save() and saveMathsModules(): the write-ahead journal file, one document's write (D-070 backup first,
    // its baseline after), the retirement of unnamed files `within` a part of the project, the clean state.
    void journalAhead();
    Result<void> writeIfChanged(const std::string& rel, const std::string& text);
    Result<void> retireUnnamed(const std::set<std::string, std::less<>>& current,
                               const std::function<bool(std::string_view)>& within);
    bool hasUnsavedDocuments() const;
    void markSaved();
    Result<void> applyReplay(const project::JournalReplay& replay);
    // Appends every document that differs from disk as this session's journal file; `force` writes it even
    // when it equals the last one (save()'s write-ahead copy, a recovery's adoption).
    Result<void> journalDirty(bool force);
    project::ProjectFile projectForSave() const;

    Paths paths_;
    SettingsStore* settings_ = nullptr;
    std::optional<std::filesystem::path> folder_;
    project::ProjectFile project_;
    std::vector<project::WorkbookFile> workbooks_;
    std::vector<std::string> workbookNames_; // file names (project.json "workbooks") of workbooks_
    int activeWorkbook_ = 0;
    std::shared_ptr<const project::Scheme> scheme_;
    std::shared_ptr<const project::Scheme> light_;
    std::shared_ptr<const project::Scheme> dark_;
    project::Keymap keymap_;
    project::Profile profile_;
    bool defaultsLoaded_ = false;
    std::vector<std::string> log_;
    // M2
    QUndoStack undoStack_;
    project::ChannelOverrides overrides_;
    project::Aliases aliases_;
    project::Mappings mappings_;
    std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps_;
    std::vector<maths::MathModule> mathsModules_;
    mutable std::shared_ptr<const units::UnitRegistry> units_;
    std::map<std::string, Baseline, std::less<>> baseline_; // by project-relative path
    std::unique_ptr<project::Journal> journal_;
    std::map<std::string, Hash128, std::less<>> lastJournaled_;
    std::optional<project::JournalReplay> replayCache_; // the open's replay until the prompt is answered
    RecoveryPrompt recoveryPrompt_;
    bool recoveryOutstanding_ = false;
    bool recovered_ = false; // documents came from the journal or a migration: dirty until saved
    QTimer* autosaveTimer_ = nullptr;
    std::chrono::milliseconds autosaveOverride_{0};
    bool dirty_ = false;
};

} // namespace esses::qt
