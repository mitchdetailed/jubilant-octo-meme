// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemeSelector (06 sections 2.1, 13.7 and 16; 07 section 5.1 "scheme selector"): the combo of the shipped
// schemes (light, then dark) and the user's schemes (<config>/schemes/*.scheme.json, by name), shown on the
// main toolbar when Options > Colours "Show scheme selector on toolbar" is on (user setting
// ui.schemeSelectorOnToolbar, kToolbarKey) and on the Colours tab. Choosing a scheme switches it live through
// ProjectSession::setScheme, which records it in the user settings (scheme.active) and emits schemeChanged()
// (the shell repaints every worksheet with the Scheme change bit and restyles the docks); an explicit choice
// turns "Follow OS light/dark" (scheme.followOs, kFollowOsKey) off. Every selector follows schemeChanged(),
// so the toolbar combo and the Colours tab always show the active scheme. Scheme selection is view state,
// never on the undo stack (D-032).
//
// User schemes are validated on load (scheme-1); the roles, fonts and palettes one lacks are taken from the
// shipped scheme of its background, one log line each (06 section 16 rule 8), and a file that does not load,
// or whose name is already listed, is left out with a log line (logMessage(); the shell shows them in
// Messages). The Colours tab's Duplicate, Import and Export work on these files (duplicateActive,
// importScheme, exportActive); schemes are edited as JSON in 0.1 (06 section 13.9).
#pragma once

#include <esses/base/result.hpp>
#include <esses/project/scheme.hpp>

#include <QComboBox>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <vector>

namespace esses::qt {
class ProjectSession;
class SettingsStore;
} // namespace esses::qt

namespace esses::widgets {

class SchemeSelector final : public QComboBox {
    Q_OBJECT
public:
    static constexpr const char* kToolbarKey = "ui.schemeSelectorOnToolbar";
    static constexpr const char* kFollowOsKey = "scheme.followOs";
    static constexpr const char* kActiveKey = "scheme.active";

    explicit SchemeSelector(qt::ProjectSession& session, qt::SettingsStore* settings = nullptr,
                            QWidget* parent = nullptr);
    ~SchemeSelector() override;

    // Re-reads the shipped and user schemes (names in display order) and shows the active one.
    void refresh();
    QStringList schemeNames() const;
    // The scheme listed under `name`, or nullptr.
    std::shared_ptr<const project::Scheme> scheme(const QString& name) const;
    // Switches to `name` (an explicit choice: following the OS turns off); false when no such scheme exists.
    bool selectScheme(const QString& name);

    // Start-up (06 section 18 step 2): the scheme the user settings name (scheme.active) when it is a listed
    // user scheme (the session itself picks between the shipped ones), then the shipped scheme of the OS
    // colour scheme when following the OS is on.
    void restoreActive();
    // "Follow OS light/dark" (user setting, off by default): on, the shipped scheme matching the OS colour
    // scheme is applied now and by every applySystemScheme() (the shell calls it with the scheme that
    // QStyleHints::colorSchemeChanged reports).
    bool followsSystem() const;
    void setFollowSystem(bool on);
    // Applies the shipped scheme of the OS colour scheme (QStyleHints::colorScheme()) when following the OS
    // is on and the OS reports light or dark; false when nothing changed.
    bool applySystemScheme();
    // The same for the colour scheme `os` the OS reports (Unknown: nothing to follow), as colorSchemeChanged
    // carries it. QStyleHints::colorScheme() is whatever the platform theme reports; the Qt 6.8
    // QStyleHints::setColorScheme() only asks the theme, which may apply it later or ignore it (offscreen).
    bool applySystemScheme(Qt::ColorScheme os);

    // <config>/schemes (created on demand by the file operations).
    std::filesystem::path userSchemeDir() const;
    // Duplicate (06 section 13.7): writes a copy of the active scheme named "<name> copy" (numbered when that
    // name exists) to the user scheme folder, lists it and selects it; the new file.
    Result<std::filesystem::path> duplicateActive();
    // Import: a valid scheme-1 file whose name is not listed yet is copied (canonical form) into the user
    // scheme folder and listed; its name. Invalid when the name is taken, the loader's error when it does not
    // load.
    Result<QString> importScheme(const std::filesystem::path& file);
    // Export: the active scheme in canonical form.
    Result<void> exportActive(const std::filesystem::path& file) const;

    // The application palette of a scheme (06 section 16 rule 6): windowBackground -> Window and Button (the
    // bevel shades derived from it), dataBackground -> Base and AlternateBase, dataForeground -> WindowText,
    // Text and ButtonText, dataHighlight* -> Highlight and HighlightedText, dataForeground2 ->
    // PlaceholderText and the disabled texts; a role the scheme lacks keeps the derived colour.
    static QPalette palette(const project::Scheme& scheme);

Q_SIGNALS:
    // A user-scheme load or fill message (06 section 16 rule 8).
    void logMessage(const QString& text);

private:
    struct Entry {
        QString name;
        std::shared_ptr<const project::Scheme> scheme;
        std::filesystem::path file; // empty for the shipped schemes
    };
    void showActive();
    bool apply(const QString& name);

    qt::ProjectSession& session_;
    qt::SettingsStore* settings_ = nullptr;
    std::vector<Entry> entries_;
};

} // namespace esses::widgets
