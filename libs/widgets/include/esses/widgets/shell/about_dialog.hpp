// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The About dialog and the build identification (M4 wave 2, packaging package; D-206; 07 section 7.1
// "Packaging", AC-M4-06 "the About dialog shows the Qt LGPL notice and the exact source URL", AC-M4-09
// "esses --version prints version, commit, Qt version and build date", demo 7.5 line 4; 06 section 2.2 Help >
// About "versions of Esses, Qt, ADS; licence notices link"; 08 section 7; D-006 dynamic Qt under the LGPL).
//   * buildInfo(): the fields both use: the version, commit and build date stamped by cmake/version.cmake
//     (esses::versionInfo), the Qt version run against (qVersion()) and built against (QT_VERSION_STR), the
//     docking library and its version.
//   * versionReport(): what `esses --version` prints (apps/app/src/main.cpp prints exactly this, then a
//     newline). Its first line stays "esses <version>" (the console probe of app.version_console looks for
//     it).
//   * qtLicenceNotice(): the notice that Qt is used under the LGPL v3 with where its source and licence text
//     are (NOTICES.md, the Qt source offer of 08 section 7).
//   * sourceUrl(): the exact source of this build (the public repository at the build's tag or commit).
// Files of the package (docs/milestones/M4.md section 3): src/shell/m4_about/,
// src/shell/main_window_m4_about.cpp, this header and about_shell.hpp, apps/app/src/main.cpp. M4-STUB: the
// texts below are placeholders.
#pragma once

#include <QDialog>
#include <QString>

class QLabel;

namespace esses::widgets {

struct BuildInfo {
    QString appName;     // ESSES_APP_NAME ("Esses")
    QString version;     // "0.1.0", "0.1.0+3-gabc1234"
    QString commit;      // the git sha, "unknown" outside a checkout
    QString buildDate;   // ISO date
    QString qtRuntime;   // qVersion()
    QString qtBuild;     // QT_VERSION_STR
    QString dockLibrary; // "ADS 5.1.1" or "QDockWidget" (M4-STUB: empty)
};

// Final apart from dockLibrary (D-206).
BuildInfo buildInfo();
// `esses --version`. M4-STUB: "esses <version>" only.
QString versionReport();
// M4-STUB: a placeholder line.
QString qtLicenceNotice();
// M4-STUB: the repository URL without the tag or commit.
QString sourceUrl();

class AboutDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override;

    // The text the dialog shows (tests read it). M4-STUB: the version report and the placeholders.
    QString text() const;

private:
    QLabel* label_ = nullptr;
};

} // namespace esses::widgets
