// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// --scheme and --keymap at the start of an instance and a forwarded request's files (06 section 19, D-212):
// a scheme by its name (any case) or "light"/"dark" for the shipped ones; an unknown name is a Messages
// warning and changes nothing; a keymap by name from <config>/keymaps, or by file, on top of the shipped
// default, for this run (the user's settings and keymap.json untouched); a forwarded request for the project
// already open adds its logs without reopening it. Expectations: the shipped scheme names of data/schemes,
// the default row of view.data_window in data/keymaps/default.json (F8), and keymap files this test writes.

#include "app_test_support.hpp"
#include "launch.hpp"
#include "launch_request.hpp"

#include <esses/base/json_doc.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/scheme.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::app;
using namespace esses::app::test;

namespace {

const QString kCommand = QStringLiteral("view.data_window"); // F8 in the shipped keymap
const QString kKey = QStringLiteral("Ctrl+Alt+Shift+F10");   // bound nowhere in it

bool writeKeymap(const QString& path, const QString& name) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QStringLiteral(
                R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "%1", "inherits": "default",
 "bindings": [{"command": "%2", "keys": ["%3"], "context": "global"}]})")
                .arg(name, kCommand, kKey)
                .toUtf8());
    return true;
}

int warningsWithCode(qt::MessagesModel& messages, const QString& code) {
    int n = 0;
    for (int row = 0; row < messages.rowCount(); ++row) {
        const qt::Message& m = messages.at(row);
        if (m.code == code && m.severity == qt::Message::Severity::Warning) {
            ++n;
        }
    }
    return n;
}

struct Shell {
    QTemporaryDir config;
    std::unique_ptr<qt::AppRuntime> rt = runtime(2);
    std::unique_ptr<widgets::MainWindow> w;

    bool start() {
        if (!config.isValid()) {
            return false;
        }
        w = std::make_unique<widgets::MainWindow>(*rt, pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        return showAndActivate(*w);
    }
    ~Shell() {
        w.reset();
        rt->shutdown();
    }
};

} // namespace

class TestLaunchOptions : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void schemeByNameOrShippedAlias() {
        Shell sh;
        QVERIFY(sh.start());
        QCOMPARE(applyScheme(*sh.w, QStringLiteral("ESSES DARK")), QString());
        QCOMPARE(QString::fromStdString(sh.w->session().scheme()->name), QStringLiteral("Esses Dark"));
        QCOMPARE(applyScheme(*sh.w, QStringLiteral("light")), QString());
        QCOMPARE(QString::fromStdString(sh.w->session().scheme()->name), QStringLiteral("Esses Light"));
        QCOMPARE(applyScheme(*sh.w, QStringLiteral("Dark")), QString());
        QCOMPARE(QString::fromStdString(sh.w->session().scheme()->name), QStringLiteral("Esses Dark"));
    }

    void unknownSchemeIsAWarning() {
        Shell sh;
        QVERIFY(sh.start());
        const std::string before = sh.w->session().scheme()->name;
        const QString problem = applyScheme(*sh.w, QStringLiteral("no such scheme"));
        QVERIFY(problem.contains(QStringLiteral("no such scheme")));
        QVERIFY(problem.contains(QStringLiteral("Esses Light"))); // lists what there is
        QCOMPARE(sh.w->session().scheme()->name, before);
        QCOMPARE(warningsWithCode(sh.w->messages(), QStringLiteral("launch.scheme")), 1);
    }

    void keymapByNameFromTheConfigFolder() {
        Shell sh;
        QVERIFY(sh.start());
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{QStringLiteral("F8")});
        QVERIFY(
            writeKeymap(sh.config.path() + QStringLiteral("/keymaps/laptop.json"), QStringLiteral("laptop")));
        QCOMPARE(applyKeymap(*sh.w, QStringLiteral("laptop")), QString());
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{kKey});
        QVERIFY(!QFileInfo::exists(sh.config.path() + QStringLiteral("/keymap.json"))); // this run only
        QCOMPARE(sh.w->keymap().keysFor(QStringLiteral("view.channels_window")),
                 QStringList{QStringLiteral("C")});
    }

    void keymapByFile() {
        Shell sh;
        QVERIFY(sh.start());
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        const QString file = elsewhere.path() + QStringLiteral("/my keys.json");
        QVERIFY(writeKeymap(file, QStringLiteral("mine")));
        LaunchRequest request;
        request.keymap = file;
        QCOMPARE(applyStartOptions(*sh.w, request), QStringList{});
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{kKey});
    }

    void unknownOrBrokenKeymapIsAWarning() {
        Shell sh;
        QVERIFY(sh.start());
        QVERIFY(!applyKeymap(*sh.w, QStringLiteral("nope")).isEmpty());
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{QStringLiteral("F8")});
        const QString broken = sh.config.path() + QStringLiteral("/keymaps/broken.json");
        QVERIFY(QDir().mkpath(QFileInfo(broken).absolutePath()));
        {
            QFile f(broken);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ not json");
        }
        QVERIFY(!applyKeymap(*sh.w, QStringLiteral("broken")).isEmpty());
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{QStringLiteral("F8")});
        QCOMPARE(warningsWithCode(sh.w->messages(), QStringLiteral("launch.keymap")), 2);
    }

    void startOptionsTogether() {
        Shell sh;
        QVERIFY(sh.start());
        QVERIFY(
            writeKeymap(sh.config.path() + QStringLiteral("/keymaps/laptop.json"), QStringLiteral("laptop")));
        LaunchRequest request;
        request.scheme = QStringLiteral("dark");
        request.keymap = QStringLiteral("laptop");
        QCOMPARE(applyStartOptions(*sh.w, request), QStringList{});
        QCOMPARE(QString::fromStdString(sh.w->session().scheme()->name), QStringLiteral("Esses Dark"));
        QCOMPARE(sh.w->keymap().keysFor(kCommand), QStringList{kKey});
        request.scheme = QStringLiteral("x");
        request.keymap = QStringLiteral("y");
        QCOMPARE(applyStartOptions(*sh.w, request).size(), 2);
    }

    // A forwarded request naming the project that is open adds its logs (06 section 19 item 6); no reopen.
    void forwardedRequestForTheOpenProjectAddsItsLogs() {
        Shell sh;
        QVERIFY(sh.start());
        QTemporaryDir projects;
        QVERIFY(projects.isValid());
        const QString folder = projects.path() + QStringLiteral("/Saturday");
        {
            qt::ProjectSession maker(pathsFor(sh.config.path()), nullptr);
            QVERIFY(static_cast<bool>(maker.loadDefaults()));
            QVERIFY(static_cast<bool>(maker.newProject(fsPath(folder), "Saturday")));
            maker.closeProject();
        }
        QVERIFY(sh.w->openProject(fsPath(folder)));
        QTRY_VERIFY(sh.w->session().isOpen());
        QSignalSpy reopened(&sh.w->session(), &qt::ProjectSession::projectOpened);
        QSignalSpy opened(sh.w.get(), &widgets::MainWindow::sourceOpened);
        const QString csv = repoRoot() + QStringLiteral("/fixtures/generic.csv/sparse-gps/input.csv");
        LaunchRequest r;
        r.project = folder + QStringLiteral("/project.json");
        r.logs = QStringList{csv};
        openRequestedFiles(*sh.w, r, LaunchKind::Forwarded);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(QDir::cleanPath(opened.at(0).at(0).toString()), QDir::cleanPath(csv));
        QCOMPARE(reopened.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(sh.w->loader().sources().size(), std::size_t{1}, 60000);
    }

    // Regression (PR #5 review U15, D-217): a forwarded switch to another project that the user cancels at
    // "Save the changes?" opens none of its logs into the project that stays; an Info row says why.
    void cancelledProjectSwitchOpensNoLogs() {
        Shell sh;
        QVERIFY(sh.start());
        QTemporaryDir projects;
        QVERIFY(projects.isValid());
        const QString here = projects.path() + QStringLiteral("/Here");
        const QString there = projects.path() + QStringLiteral("/There");
        QVERIFY(sh.w->session().newProject(fsPath(here), "Here").ok());
        QVERIFY(sh.w->session().newProject(fsPath(there), "There").ok());
        QVERIFY(sh.w->openProject(fsPath(here)));
        int asked = 0;
        sh.w->setExitPrompt([&asked] {
            ++asked;
            return widgets::MainWindow::ExitAnswer::Cancel;
        });
        project::ProjectFile next = sh.w->session().projectFile(); // save on exit off, then an edit
        QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
        QJsonObject general = o.value(QStringLiteral("general")).toObject();
        general.insert(QStringLiteral("saveOnExit"), false);
        o.insert(QStringLiteral("general"), general);
        next.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
        sh.w->session().setProjectFile(std::move(next));
        QVERIFY(sh.w->session().isDirty());
        QSignalSpy opened(sh.w.get(), &widgets::MainWindow::sourceOpened);
        LaunchRequest r;
        r.project = there;
        r.logs = QStringList{repoRoot() + QStringLiteral("/fixtures/generic.csv/sparse-gps/input.csv")};
        openRequestedFiles(*sh.w, r, LaunchKind::Forwarded);
        QCOMPARE(asked, 1);
        QVERIFY(sh.w->projectOpenCancelled());
        QCOMPARE(sh.w->session().projectFile().name, std::string("Here"));
        QCOMPARE(opened.count(), 0); // nothing of the other project's logs opened here
        int rows = 0;
        for (int row = 0; row < sh.w->messages().rowCount(); ++row) {
            rows += sh.w->messages().at(row).code == QStringLiteral("launch.cancelled") ? 1 : 0;
        }
        QCOMPARE(rows, 1);
        sh.w->setExitPrompt([] { return widgets::MainWindow::ExitAnswer::Discard; });
    }
};

QTEST_MAIN(TestLaunchOptions)
#include "tst_launch_options.moc"
