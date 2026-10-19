// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The command line of the demo of 07 section 7.5, line 2 (`esses -w <project> -l <log>`: a project and a log
// from the command line; 06 section 18 steps 3-6; D-212, D-216 (d)), in a real esses process, offscreen,
// started as the installed program is: a project folder made by ProjectSession and, outside it, a log (the
// committed generic.csv fixture copied under a name of its own); `esses --new-instance -w <folder> -l <log>`
// with a private configuration and channel name, so it never meets another Esses. The child opens the project
// (its settings' recent projects name the folder), then the log (its recent logs name it), and keeps its
// window running. What the demo does next in that window (the Budget warning with the estimate, Continue, the
// Performance line, Help > About, the portable layout) is ui.tst_walkthrough_saturday's
// demo75_offscreenWithoutTheInstaller, through the same MainWindow::openProject in process; parsing -w and -l
// is app.tst_launch_request's. The expected paths are the ones the test made; the child's settings are read
// through esses::readWholeFile (the child rewrites them).

#include "app_test_support.hpp"

#include <esses/qtmodel/project_session.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using namespace esses;
using namespace esses::app::test;

namespace {

constexpr int kLong = 120000; // a child esses in a Debug build on a loaded machine

// The paths of `settings.json`'s `mru.<list>` (settings-1: {"mru": {"projects": [...], "logs": [...]}}).
QStringList recentList(const QString& configDir, const QString& list) {
    const QJsonDocument doc =
        QJsonDocument::fromJson(fileText(configDir + QStringLiteral("/settings.json")).toUtf8());
    QStringList out;
    for (const QJsonValue& v : doc.object().value(QStringLiteral("mru")).toObject().value(list).toArray()) {
        out << v.toString();
    }
    return out;
}

// `list` names `path` (the same file or folder, however its separators and case are written).
bool names(const QStringList& list, const QString& path) {
    const QString want = QFileInfo(path).canonicalFilePath();
    for (const QString& p : list) {
        if (!want.isEmpty() && QFileInfo(p).canonicalFilePath() == want) {
            return true;
        }
    }
    return false;
}

} // namespace

class TestDemoCommandLine : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void projectAndLogFromTheCommandLine() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.path() + QStringLiteral("/Saturday demo");
        {
            qt::ProjectSession maker(pathsFor(dir.path() + QStringLiteral("/maker")), nullptr);
            QVERIFY(static_cast<bool>(maker.loadDefaults()));
            QVERIFY(static_cast<bool>(maker.newProject(fsPath(folder), "Saturday demo")));
            maker.closeProject();
        }
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/project.json")));
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/logs")));
        const QString log = dir.path() + QStringLiteral("/logs/demo session.csv");
        QVERIFY(QFile::copy(repoRoot() + QStringLiteral("/fixtures/generic.csv/sparse-gps/input.csv"), log));

        const QString config = dir.path() + QStringLiteral("/config");
        QProcess child;
        child.setProcessEnvironment(childEnvironment(config, uniqueInstanceName()));
        child.setWorkingDirectory(dir.path());
        child.start(essesExe(), {QStringLiteral("--new-instance"), QStringLiteral("-w"), folder,
                                 QStringLiteral("-l"), log});
        QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
        // The project first (06 section 18 step 3), then the log in it (step 4).
        QTRY_VERIFY_WITH_TIMEOUT(names(recentList(config, QStringLiteral("projects")), folder), kLong);
        QTRY_VERIFY_WITH_TIMEOUT(names(recentList(config, QStringLiteral("logs")), log), kLong);
        QCOMPARE(recentList(config, QStringLiteral("projects")).size(), 1);
        QVERIFY2(child.state() != QProcess::NotRunning, child.readAllStandardError().constData());
        stopChild(child);
    }
};

QTEST_MAIN(TestDemoCommandLine)
#include "tst_demo_command_line.moc"
