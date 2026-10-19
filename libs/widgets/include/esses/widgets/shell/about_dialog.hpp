// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The About dialog and the build identification (M4 wave 2, packaging package; D-206, D-212; 07 section 7.1
// "Packaging", AC-M4-06 "the About dialog shows the Qt LGPL notice and the exact source URL", AC-M4-09
// "esses --version prints version, commit, Qt version and build date", demo 7.5 line 4; 06 section 2.2
// Help > About "versions of Esses, Qt, ADS; licence notices link"; 08 sections 7 and 9; D-006 dynamic Qt
// under the LGPL).
//   * buildInfo(): the fields both use: the version, commit and build date stamped by cmake/version.cmake
//     (esses::versionInfo), the Qt version run against (qVersion()) and built against (QT_VERSION_STR), the
//     docking library in use ("ADS <version>" from ADS's own version header, or "QDockWidget" when the build
//     has no ADS or ESSES_DOCK_BACKEND=qdockwidget chose it) and the release tag of a release build.
//   * versionReport(): what `esses --version` prints (apps/app/src/main.cpp prints exactly this, then a
//     newline). Its first line stays "esses <version>" (the console probe of app.version_console looks for
//     it); then one "<field>: <value>" line each for commit, Qt, build date, dock library and source.
//   * qtLicenceNotice(): 08 section 9 item 2's statement "This application uses Qt <version> under the
//     GNU LGPL v3", where the licence texts are, that Qt may be replaced, and the exact URL of the source
//     archive of the Qt version the application runs with (qtSourceArchiveUrl, 08 section 9 item 3).
//   * dockLibraryNotice(): the same for the Qt Advanced Docking System (LGPL-2.1-or-later, its tag URL);
//     empty in a build without ADS.
//   * sourceUrl(): the exact source of this build (D-212): the public repository's tag URL in a release build
//     (`<repository>/tree/<tag>`; the tag comes from ESSES_RELEASE_TAG, which tools/packaging/CMakeLists.txt
//     defines for this file only when a release is configured with -DESSES_RELEASE_TAG=v<version>), else the
//     commit URL (`<repository>/commit/<sha>`), else the repository when the commit is unknown.
//   * noticesUrl(paths): NOTICES.md as installed beside the data folder (`<data>/../NOTICES.md`: the
//     installation folder on Windows, share/esses on Linux, the bundle's Resources on macOS), else the
//     repository's copy at the build's tag or commit.
// The dialog shows all of it as text with links (a Copy button puts text() on the clipboard) and, given the
// window's Paths, the settings folder and whether the run is portable (portable.marker beside the executable,
// esses::Paths; 07 section 7.5 last line).
// Files of the package (docs/milestones/M4.md section 3): src/shell/m4_about/,
// src/shell/main_window_m4_about.cpp, this header and about_shell.hpp, apps/app/.
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;

namespace esses {
class Paths;
} // namespace esses

namespace esses::widgets {

// The public repository (the source URLs and the notices link of 08 section 9 item 3; D-212).
inline constexpr const char* kRepositoryUrl = "https://github.com/mitchdetailed/jubilant-octo-meme";

struct BuildInfo {
    QString appName;     // ESSES_APP_NAME ("Esses")
    QString version;     // "0.1.0", "0.1.0+3-gabc1234"
    QString commit;      // the git sha, "unknown" outside a checkout
    QString buildDate;   // ISO date
    QString qtRuntime;   // qVersion()
    QString qtBuild;     // QT_VERSION_STR
    QString dockLibrary; // "ADS 5.1.1" or "QDockWidget": the dock backend in use
    QString releaseTag;  // "v0.1.0" in a release build (ESSES_RELEASE_TAG), else empty
};

// Final (D-206; dockLibrary and releaseTag filled by D-212).
BuildInfo buildInfo();
// `esses --version`: "esses <version>", then the lines "commit: ", "Qt: <runtime> (built with <build>)",
// "build date: ", "dock library: " and "source: " (no trailing newline).
QString versionReport();
// The Qt LGPL notice with the source archive of the Qt version in use.
QString qtLicenceNotice();
// The docking library's LGPL notice; empty without ADS.
QString dockLibraryNotice();
// The exact source of this build (see the file comment).
QString sourceUrl();
// The rule behind sourceUrl(): a non-empty `releaseTag` gives <repository>/tree/<releaseTag>; else a commit
// of 7 to 40 hexadecimal digits (a "-dirty" suffix ignored) gives <repository>/commit/<sha>; else the
// repository.
QString sourceUrlFor(const QString& releaseTag, const QString& commit);
// https://download.qt.io/official_releases/qt/<major>.<minor>/<version>/single/qt-everywhere-src-<version>.tar.xz
QString qtSourceArchiveUrl(const QString& qtVersion);
// NOTICES.md beside the data folder when it is installed there (a file URL), else the repository's copy.
QString noticesUrl(const Paths& paths);

class AboutDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
    // With the window's paths: the notices link of the installation and the settings folder (portable or
    // not).
    AboutDialog(const Paths& paths, QWidget* parent);
    ~AboutDialog() override;

    // The text the dialog shows, as plain text (tests read it; Copy puts it on the clipboard). Its first line
    // is versionReport()'s.
    QString text() const;
    // The rich text of the label (links as <a href>).
    QString html() const;
    // The notices link shown (noticesUrl of the paths, else the repository's copy).
    QString noticesLink() const { return notices_; }
    QPushButton* copyButton() const { return copy_; }

private:
    void build(const Paths* paths);

    QLabel* label_ = nullptr;
    QPushButton* copy_ = nullptr;
    QString text_;
    QString notices_;
};

} // namespace esses::widgets
