// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AboutShell (M4 wave 2, packaging package; D-206, D-212; 06 section 2.2 Help > About; AC-M4-06): registers
// kCommand in the Help menu (a menu command without a keymap row, as help.keyboard_shortcuts, D-203 (ah)) and
// opens the AboutDialog (about_dialog.hpp) window-modal with the window's Paths, so it links the installed
// NOTICES.md and names the settings folder. The portable mode itself (settings beside the executable when
// portable.marker is there) is esses::Paths since M0 (D-055); the dialog says when a run is portable (07
// section 7.5 last line).
#pragma once

#include <esses/widgets/shell/shell_extension.hpp>

#include <QPointer>

namespace esses::widgets {

class AboutDialog;

class AboutShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "about";
    static constexpr const char* kCommand = "help.about";

    explicit AboutShell(MainWindow& window);
    ~AboutShell() override;

    // The dialog while it is open, else nullptr.
    AboutDialog* dialog() const;

    void registerCommands() override; // help.about
    void teardown() override;

private:
    QPointer<AboutDialog> dialog_;
};

} // namespace esses::widgets
