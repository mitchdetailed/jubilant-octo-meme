// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OptionsDialog (Tools > Options, tools.options; 06 section 13.7, 02 section 6.2, D-019, D-020, D-022,
// D-032). Tabs:
//   General   save on exit, autosave interval (minutes, 0 = off), load previous data with the project, select
//             the fastest lap on load, loose channel-name search, auto-show Messages on error, reduce CPU
//             during animation (project options.general.*), the memory budget (user setting
//             memory.chunkCacheBudgetBytes in GB of 2^30 bytes, one decimal, at least 1 GB; shown as the
//             shell applies it, else the default of a quarter of the RAM; M4 budget package), language (user
//             setting "language"; English only in 0.1)
//   Analysis  autoscale with untrusted and with overlay data, axis time format and lap-time format, fast-lap
//             tolerance %, lap-distance stretching tolerance %, maths precision (single = F32, double = F64)
//             and "unary minus above power" (project options autoscale.*, timeFormat, lapTimeFormat,
//             fastLapTolerancePct, lapStretch.tolerancePct, maths.precision, maths.compat.*)
//   Colours   the SchemeSelector (a live switch), Duplicate, Import..., Export..., show the selector on the
//             toolbar and follow OS light/dark (user settings ui.schemeSelectorOnToolbar, scheme.followOs)
//   Fonts     read-only list of the active scheme's font roles (06 section 16; edited in the scheme file in
//   0.1) Keys      read-only keymap table (command, context, keys, default keys, status) with Import...,
//   Export...
//             and Reveal of <config>/keymap.json; an imported keymap applies at the next start
// Every field's object name is its option key ("general.autosaveMinutes", "maths.precision", ...; user
// settings by their settings key). The fields show the project's options (02 section 6.2 defaults for absent
// members) when the dialog is shown and after an undo while no edit is pending. apply() (Apply, and OK before
// it closes) writes the members whose value changed as one SetProjectOptionsCommand ("Change options") on the
// session's undo stack, never touching the view state in project.json (lastSelection, dataWindow, the
// lap-stretch toggle), and the user settings to SettingsStore; a field that does not hold a valid value
// refuses Apply and OK with the reason (06 section 13: editors refuse to close while a field is invalid).
// The shell opens the dialog window-modal and remembers its geometry in layout.json under kGeometryKey.
#pragma once

#include <esses/base/result.hpp>

#include <QDialog>
#include <QString>
#include <filesystem>
#include <functional>
#include <memory>

class QTabWidget;

namespace esses::qt {
class ProjectSession;
class SettingsStore;
} // namespace esses::qt

namespace esses::widgets {

class SchemeSelector;

class OptionsDialog final : public QDialog {
    Q_OBJECT
public:
    enum class Tab : int { General, Analysis, Colours, Fonts, Keys };
    static constexpr const char* kGeometryKey = "optionsDialog";
    // Shows a folder in the system's file manager (Duplicate, Reveal); the default opens it with
    // QDesktopServices.
    using RevealHandler = std::function<void(const std::filesystem::path& folder)>;

    OptionsDialog(qt::ProjectSession& session, qt::SettingsStore* settings = nullptr,
                  QWidget* parent = nullptr);
    ~OptionsDialog() override;

    void showTab(Tab tab);
    QTabWidget& tabs() noexcept { return *tabs_; }
    SchemeSelector& schemeSelector();

    // Re-reads every field from the project options and the user settings (pending edits are dropped).
    void reload();
    // Edits made since the last reload() or apply().
    bool hasPendingEdits() const;
    // Writes the edited project options (one undo command when a member changed) and the user settings;
    // false, with errorText() set and shown, when a field is invalid.
    bool apply();
    QString errorText() const;

    void setRevealHandler(RevealHandler reveal);
    // The Keys tab's actions (the buttons ask for the file first): a keymap-1 file whose rows merge with the
    // shipped keymap without a conflict is written to <config>/keymap.json (it applies at the next start);
    // the effective keymap is written to `file`; <config> is revealed.
    Result<void> importKeymap(const std::filesystem::path& file);
    Result<void> exportKeymap(const std::filesystem::path& file) const;
    void revealKeymap();

Q_SIGNALS:
    // apply() wrote the options and the settings.
    void applied();

protected:
    void showEvent(QShowEvent* event) override;
    void done(int result) override;

private:
    struct Impl;
    void buildGeneral();
    void buildAnalysis();
    void buildColours();
    void buildFonts();
    void buildKeys();
    void reloadFields();
    void fillFonts();
    void fillKeys();
    void markEdited();
    bool validate(QString& error) const;

    std::unique_ptr<Impl> d_;
    QTabWidget* tabs_ = nullptr;
};

} // namespace esses::widgets
