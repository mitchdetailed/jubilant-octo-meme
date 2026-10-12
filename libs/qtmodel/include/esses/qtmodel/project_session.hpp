// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession (06 sections 1, 4 and 18; 02 section 6; D-019, D-032): the open workspace (a project folder)
// and its documents: project.json, the workbooks, and the active scheme, keymap and profile. With no project
// open it serves the shipped defaults (data/workbooks/circuit-default.workbook.json, the light/dark scheme,
// the default keymap, the Circuit profile), which is how `esses -l <log>` opens in M1 (07 section 4.5). M1:
// no undo yet (07 section 4.1); edits replace documents and emit documentsChanged(). M2 adds the undo stack
// (see libs/qtmodel/README.md: QUndoStack lives in Qt Gui in Qt 6, which esses_qtmodel may not link), the
// journal and autosave. GUI thread only.
// M1 details: the per-user documents (schemes, keymap with <config>/keymap.json merged over the shipped
// default) are loaded once by loadDefaults() and kept across projects; openProject() reads project.json,
// workbooks/<name>.workbook.json in project order (an unreadable one is skipped and logged; none at all falls
// back to the shipped workbook) and the profile named by the project. newProject() creates the 02 section 6.1
// folder layout with the profile's default workbook. save() writes project.json and every workbook with the
// D-070 backup. Additions: setLastSelection (DataSelectionModel keeps lastSelection current),
// addRecentSource, ruleRunner (the profile's rules), correctedSpeedSettings/Enabled (project.json
// builtinMaths), shippedScheme, log and the logMessage() signal (Messages, origin Project).
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/source.hpp>
#include <esses/project/keymap.hpp>
#include <esses/project/profile.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/workbook_file.hpp>

#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::qt {

class SettingsStore;

class ProjectSession final : public QObject {
    Q_OBJECT
public:
    ProjectSession(Paths paths, SettingsStore* settings = nullptr, QObject* parent = nullptr);
    ~ProjectSession() override;

    // The shipped defaults from paths.dataDir() (no project open). Io/Format/Invalid when a shipped document
    // is missing or malformed.
    Result<void> loadDefaults();
    Result<void> openProject(const std::filesystem::path& folder);
    Result<void> newProject(const std::filesystem::path& folder, const std::string& name);
    Result<void> save();
    void closeProject();

    bool isOpen() const noexcept { return folder_.has_value(); }
    const std::optional<std::filesystem::path>& folder() const noexcept { return folder_; }
    const Paths& paths() const noexcept { return paths_; }

    const project::ProjectFile& projectFile() const noexcept { return project_; }
    const std::vector<project::WorkbookFile>& workbooks() const noexcept { return workbooks_; }
    int activeWorkbook() const noexcept { return activeWorkbook_; }
    void setActiveWorkbook(int index);
    void replaceWorkbook(int index, project::WorkbookFile workbook); // M1: no undo
    std::shared_ptr<const project::Scheme> scheme() const noexcept { return scheme_; }
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
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

Q_SIGNALS:
    void documentsChanged();
    void schemeChanged();
    void projectOpened();
    void projectClosed();
    void logMessage(const QString& text);

private:
    Result<project::WorkbookFile> loadShippedWorkbook(std::vector<std::string>& log) const;
    void publishLog(std::vector<std::string> log);

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
};

} // namespace esses::qt
