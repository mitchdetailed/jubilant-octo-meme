// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DocsShell (M4 wave 2, docs package; D-206, D-215; 07 section 7.1 "Docs"; 06 section 2.2 Help > Help
// Contents, section 14.4 help.contents F1): Help > Help Contents (F1, Global) opens the user documentation,
// the Markdown pages of docs/user/ with docs/user/index.md as the start page.
//   * Installed (the packages): the start page is looked for beside the shipped data directory first, then
//     beside the executable, i.e. the first regular file of
//         <Paths::dataDir()>/../docs/user/index.md   (kStartPage relative to the data directory's parent)
//         <Paths::exeDir()>/docs/user/index.md
//     The packaging package installs docs/user/ there as a whole (so its relative links work):
//         Windows installer, portable zip   <install dir>\docs\user\                (beside data\)
//         Linux AppImage or prefix          <prefix>/share/esses/docs/user/         (beside .../data/)
//         macOS bundle                      Esses.app/Contents/Resources/docs/user/ (beside .../data/)
//     The source tree has the same layout (data/ beside docs/user/), so a run with the repository's data
//     directory (the tests, a --data-dir at a checkout) finds the repository's pages.
//   * Not installed (a build tree, whose data directory is <build>/bin/data): the start page in the public
//     repository, onlineUrl(): kRepositoryUrl + "/blob/<ref>/docs/user/index.md" with <ref> the build's tag
//     ("v" + the version when the version is exactly a tag: "0.1.0", "0.1.0-m4") and "main" otherwise (a
//     build after a tag, "0.1.0+3-gabc1234", a dirty build, an unexpected form).
//   * openContents() (what the command runs) hands the installed page's file URL to the URL opener, then
//     the online page when there is no installed page or the opener could not open it (no program for .md
//     files); when neither opens, a Messages warning (origin System, source "Help", code kNotOpenedCode)
//     names both places, the Messages dock is shown and the status bar says so. Tests replace the opener
//     (setUrlOpener), so no test ever starts a browser.
// The command has the keymap row help.contents (implemented in M4; its KEYMAP_TEST is in
// tests/ui/keymap/keymap_m4_docs_cases.hpp). Files of the package (docs/milestones/M4.md section 3):
// src/shell/main_window_m4_docs.cpp, this header, tests/m4_docs/, docs/user/, CLEANROOM.md.
#pragma once

#include <esses/base/paths.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QString>
#include <filesystem>
#include <functional>
#include <optional>

namespace esses::widgets {

class DocsShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "docs";
    static constexpr const char* kCommand = "help.contents";
    // The start page relative to an installation's documentation root (the data directory's parent, or the
    // executable's directory).
    static constexpr const char* kStartPage = "docs/user/index.md";
    // The public repository (the online documentation): the About dialog's constant, one URL for both
    // (D-216).
    static constexpr const char* kRepositoryUrl = widgets::kRepositoryUrl;
    // The code of the Messages warning when no page could be opened.
    static constexpr const char* kNotOpenedCode = "help.not_opened";
    // Opens a URL (the default: QDesktopServices::openUrl); false when it could not.
    using UrlOpener = std::function<bool(const QString& url)>;

    explicit DocsShell(MainWindow& window);
    ~DocsShell() override;

    // Replaces the URL opener; an empty one restores the default.
    void setUrlOpener(UrlOpener opener);
    // The installed start page as a file URL (fully encoded); empty when it is not installed.
    QString contentsUrl() const;
    // Opens the documentation as the command does: the URL that opened, or empty when none did.
    QString openContents();

    // The installed start page for `paths` (the lookup above), or nothing.
    static std::optional<std::filesystem::path> installedStartPage(const Paths& paths);
    // The online start page for a build of `version` (the form of esses::versionInfo().string).
    static QString onlineUrl(const QString& version);

    void registerCommands() override; // kCommand in the Help menu, F1

private:
    bool open(const QString& url) const;

    UrlOpener opener_;
};

} // namespace esses::widgets
