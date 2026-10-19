// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReloadShell (M4 reload package; D-206, D-208; 07 section 7.1 row 2, AC-M4-10; 06 section 6 "Changed files"
// and item 1 "stale badge", section 14.4 file.reload_source; 02 section 1 behaviour 5, section 8 AC-4a; E-5):
// the shell side of changed source files.
//   * The check: QGuiApplication::applicationStateChanged to Qt::ApplicationActive runs checkNow()
//     (SourceLoader::checkChangedFiles; the chunk providers check before each decode batch on their own).
//   * The badge and the row: buildWidgets gives the Data window's model the loader's stale query
//     (DataWindowModel::setStaleQuery with SourceLoader::isStale); SourceLoader::staleChanged refreshes the
//     source row (refreshStale) and the command's enabled state; windowReady wires qt::connectSourceReload,
//     so a stale source gets its Messages row (Import, Warning, open.file_changed) whose action is kCommand
//     with the source id, and a reload restores its selection (DataSelectionModel::sourceReloaded).
//   * The command: kCommand "Reload File" (File menu, data_window context, no default key; keymap row
//     implemented in M4, KEYMAP_TEST in tests/ui/keymap/keymap_m4_reload_cases.hpp). With an argument (the
//     Messages row, the Data window's context menu) it reloads that source; without one the highlighted
//     source when it is stale, else the first stale source in Data-window order. Enabled while any source is
//     stale.
// Files of the package (docs/milestones/M4.md section 3): src/shell/main_window_m4_reload.cpp, this header,
// src/docks/data_window.cpp (Reload File, the badge's look), and the qtmodel/formats/base roots listed there.
#pragma once

#include <esses/model/ids.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QMetaObject>
#include <optional>
#include <vector>

namespace esses::widgets {

class ReloadShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "reload";
    static constexpr const char* kCommand = "file.reload_source";

    explicit ReloadShell(MainWindow& window);
    ~ReloadShell() override;

    // What Qt::ApplicationActive runs: the sources newly marked stale.
    std::vector<model::SourceId> checkNow();
    // file.reload_source on `source`, else the Data window's highlighted source when it is stale, else the
    // first stale source; false when that source is not open or not stale (nothing is reloaded).
    bool reloadSource(const std::optional<model::SourceId>& source = std::nullopt);

    void buildWidgets(QWidget& central) override;
    void registerCommands() override;
    void windowReady() override;
    void teardown() override;

private:
    bool anyStale() const;

    std::vector<QMetaObject::Connection> connections_;
};

} // namespace esses::widgets
