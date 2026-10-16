// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-06 in the real shell (06 section 18, 02 section 6.8; D-019, D-172), offscreen:
//   reopenRestoresTheSession  a project with a log opened, a Main lap, an overlay, single colour, a
//   lap-number
//                             Data-window filter and the second worksheet is closed (exit: the view state is
//                             written without a question) and reopened in a new window: the previous data
//                             opens again and the selection, the single-colour state, the filter and the
//                             worksheet come back.
//   killReplaysTheJournal     edits (an Options change through the dialog, a channel added to the graph) and
//   a
//                             selection change; the autosave timer writes the journal; the window is
//                             destroyed without exit handling (a kill): project.json and the workbook are
//                             untouched on disk. The next open shows "Recover unsaved changes?" listing the
//                             journalled documents, and the previous data waits for the answer; Recover
//                             applies the journal (the options, the graph's channel, the journalled
//                             selection; the project is dirty, the disk still untouched) and CTRL+S saves it
//                             and clears the journal.
//   laterAndDiscard           Later keeps the journal untouched (no autosave, an exit writes nothing) and the
//                             next open asks again; Discard deletes it and the documents are those on disk.
// Every runtime marks the GUI thread NoBlock as esses does; no ESSES_ASSERT may fail on any thread.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/project/journal.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/recovery_dialog.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The bytes of every project document on disk (project.json, the workbooks).
std::map<std::string, std::string> diskState(const std::filesystem::path& folder) {
    std::map<std::string, std::string> out;
    out["project.json"] = readAll(folder / "project.json");
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder / "workbooks", ec)) {
        out["workbooks/" + e.path().filename().string()] = readAll(e.path());
    }
    return out;
}

std::size_t journalFiles(const std::filesystem::path& folder) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder / ".journal", ec)) {
        n += e.is_regular_file() ? 1 : 0;
    }
    return n;
}

double fastLapTolerance(const qt::ProjectSession& s) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(s.projectFile().options.compact()))
        .object()
        .value(QStringLiteral("fastLapTolerancePct"))
        .toDouble();
}

TimeDistanceGraph* graphOf(MainWindow& w) {
    for (ComponentWidget* c : w.canvas().components()) {
        if (auto* g = qobject_cast<TimeDistanceGraph*>(c)) {
            return g;
        }
    }
    return nullptr;
}

struct Project {
    QTemporaryDir root;
    QTemporaryDir config;
    std::filesystem::path folder;
    std::filesystem::path ld;

    bool prepare(qt::AppRuntime& rt) {
        if (!root.isValid() || !config.isValid()) {
            return false;
        }
        folder = fsPath(root.path()) / "Spa";
        std::filesystem::create_directories(folder / "logs");
        const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
        qt::test::SyntheticOptions o;
        o.path = "spa.ld";
        o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
        ld = qt::test::writeLd(*qt::test::makeSource(o, rt.cache()), folder / "logs", "spa");
        return true;
    }
};

// A window on the project's configuration, shown and active.
std::unique_ptr<MainWindow> window(qt::AppRuntime& rt, const Project& p) {
    auto w = std::make_unique<MainWindow>(rt, uitest::pathsFor(p.config.path()));
    w->setExitPrompt([] { return MainWindow::ExitAnswer::Cancel; }); // never asked in these cases
    return w;
}

// Opens the project's log in `w` and waits for it; its displayed laps (out lap, 50 s, 48 s, 49 s, in lap).
std::vector<model::RangeRef> openLog(MainWindow& w, const Project& p) {
    w.openLog(p.ld);
    if (!QTest::qWaitFor([&] { return w.selection().sources().size() == 1; }, 30000)) {
        return {};
    }
    return w.selection().displayedRanges(w.selection().sources().front()->id);
}

} // namespace

class TstProjectRecovery : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() { registerBuiltinComponents(); }

    void reopenRestoresTheSession() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        model::RangeRef main;
        model::RangeRef overlay;
        {
            auto w = window(*rt, p);
            QVERIFY(w->session().newProject(p.folder, "Spa").ok());
            QVERIFY(w->openProject(p.folder));
            QVERIFY(uitest::showAndActivate(*w));
            const std::vector<model::RangeRef> laps = openLog(*w, p);
            QCOMPARE(laps.size(), std::size_t{5});
            main = laps[2];
            overlay = laps[3];
            w->selection().setMain(main);
            w->selection().toggleOverlay(overlay);
            w->selection().setSingleColourMain(true);
            qt::FilterSpec filter;
            filter.kind = qt::FilterSpec::Kind::LapNumbers;
            filter.laps = "1-3";
            w->selection().setFilter(main.src, filter);
            w->selectWorksheet(1);
            QVERIFY(!w->session().isDirty()); // view state only
            QVERIFY(w->close());              // exit: the view state is written without a question
        }
        {
            auto w = window(*rt, p);
            QVERIFY(w->openProject(p.folder));
            QVERIFY(uitest::showAndActivate(*w));
            QTRY_VERIFY_WITH_TIMEOUT(!w->sessionRestore().running() && w->selection().hasMain(), 30000);
            QCOMPARE(w->selection().sources().size(), std::size_t{1}); // the previous data
            QVERIFY(w->selection().snapshot().main == main);
            QCOMPARE(w->selection().snapshot().overlays, std::vector<model::RangeRef>{overlay});
            QVERIFY(w->selection().snapshot().singleColourMain);
            const qt::FilterSpec f = w->selection().filter(main.src);
            QCOMPARE(f.kind, qt::FilterSpec::Kind::LapNumbers);
            QCOMPARE(f.laps, std::string("1-3"));
            QCOMPARE(w->activeWorksheetIndex(), 1);
            QVERIFY(w->recoveryDialog() == nullptr); // no journal, no prompt
        }
        rt->shutdown();
    }

    void killReplaysTheJournal() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        model::RangeRef lap3;
        std::map<std::string, std::string> disk;
        {
            auto w = window(*rt, p);
            QVERIFY(w->session().newProject(p.folder, "Spa").ok());
            QVERIFY(w->openProject(p.folder));
            QVERIFY(uitest::showAndActivate(*w));
            const std::vector<model::RangeRef> laps = openLog(*w, p);
            QCOMPARE(laps.size(), std::size_t{5});
            QVERIFY(w->close()); // the log becomes the project's previous data (written, nothing dirty)
            QVERIFY(uitest::showAndActivate(*w));
            disk = diskState(p.folder);
            QVERIFY(disk.size() >= 2);
            lap3 = laps[3];

            // An Options edit through the dialog and a channel added to the graph: two document edits.
            w->actions().action(QStringLiteral("tools.options"))->trigger();
            OptionsDialog& options = w->optionsDialog();
            QTRY_VERIFY(options.isVisible());
            options.findChild<QDoubleSpinBox*>(QStringLiteral("fastLapTolerancePct"))->setValue(3.5);
            options.accept();
            QVERIFY(!options.isVisible());
            QTRY_VERIFY(graphOf(*w) != nullptr);
            graphOf(*w)->addChannel(QStringLiteral("Throttle"), true);
            QCOMPARE(w->session().undoStack().count(), 2);
            w->selection().setMain(lap3); // view state, journalled with project.json
            QVERIFY(w->windowTitle().contains(QLatin1Char('*')));

            // The autosave timer (2 minutes by default; shortened here) writes the journal, never the files.
            w->session().setAutosaveInterval(std::chrono::milliseconds(50));
            QTRY_VERIFY_WITH_TIMEOUT(journalFiles(p.folder) >= 1, 10000);
            QVERIFY(diskState(p.folder) == disk);
            // A kill: no exit handling, no save, no layout write.
            w->setSaveLayoutOnExit(false);
        }
        QVERIFY(diskState(p.folder) == disk); // project.json and the workbook are intact
        QVERIFY(journalFiles(p.folder) >= 1);

        auto w = window(*rt, p);
        QVERIFY(w->openProject(p.folder));
        QVERIFY(uitest::showAndActivate(*w));
        QTRY_VERIFY(w->recoveryDialog() != nullptr && w->recoveryDialog()->isVisible());
        RecoveryDialog& prompt = *w->recoveryDialog();
        QCOMPARE(QApplication::activeModalWidget(), &prompt);
        QVERIFY(prompt.documents().contains(QStringLiteral("project.json")));
        bool workbook = false;
        for (const QString& d : prompt.documents()) {
            workbook = workbook || d.startsWith(QStringLiteral("workbooks/"));
        }
        QVERIFY(workbook);
        // Steps 5-6 wait for the answer: the previous data is not open yet.
        QTest::qWait(50);
        QVERIFY(w->selection().sources().empty());
        QVERIFY(!w->sessionRestore().running());
        QCOMPARE(fastLapTolerance(w->session()), 1.0);

        prompt.button(project::JournalRecovery::Recover).click();
        QVERIFY(w->recoveryDialog() == nullptr || !w->recoveryDialog()->isVisible());
        QCOMPARE(fastLapTolerance(w->session()), 3.5);
        QVERIFY(w->session().isDirty());
        QVERIFY(w->windowTitle().contains(QLatin1Char('*')));
        QTRY_VERIFY(graphOf(*w) != nullptr &&
                    graphOf(*w)->channelNames().contains(QStringLiteral("Throttle")));
        // The previous data opens and the journalled selection comes back.
        QTRY_VERIFY_WITH_TIMEOUT(!w->sessionRestore().running() && w->selection().hasMain(), 30000);
        QVERIFY(w->selection().snapshot().main == lap3);
        QVERIFY(diskState(p.folder) == disk); // nothing written before Save
        QVERIFY(journalFiles(p.folder) >= 1);

        // CTRL+S (file.save_project) writes the documents and clears the journal.
        w->returnFocus();
        uitest::press(*w, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!w->session().isDirty());
        QCOMPARE(journalFiles(p.folder), std::size_t{0});
        QVERIFY(!(diskState(p.folder) == disk));
        std::vector<std::string> log;
        const auto saved = project::ProjectFile::load(p.folder / "project.json", log);
        QVERIFY(saved.ok());
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(saved->options.compact()))
                     .object()
                     .value(QStringLiteral("fastLapTolerancePct"))
                     .toDouble(),
                 3.5);
        w.reset();
        rt->shutdown();
    }

    void laterAndDiscard() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        std::map<std::string, std::string> disk;
        {
            auto w = window(*rt, p);
            QVERIFY(w->session().newProject(p.folder, "Spa").ok());
            QVERIFY(w->openProject(p.folder));
            disk = diskState(p.folder);
            QTRY_VERIFY(graphOf(*w) != nullptr);
            graphOf(*w)->addChannel(QStringLiteral("Throttle"), true);
            QVERIFY(w->session().autosave().ok());
            QCOMPARE(journalFiles(p.folder), std::size_t{1});
            w->setSaveLayoutOnExit(false); // killed
        }
        {
            auto w = window(*rt, p);
            QVERIFY(w->openProject(p.folder));
            QVERIFY(uitest::showAndActivate(*w));
            QTRY_VERIFY(w->recoveryDialog() != nullptr && w->recoveryDialog()->isVisible());
            w->recoveryDialog()->button(project::JournalRecovery::Later).click();
            QVERIFY(!w->session().isDirty());
            QVERIFY(!graphOf(*w)->channelNames().contains(QStringLiteral("Throttle")));
            // Autosave stays suspended and an exit writes nothing: the journal is kept for the next open.
            QVERIFY(w->session().autosave().ok());
            QCOMPARE(journalFiles(p.folder), std::size_t{1});
            QVERIFY(w->close());
            QVERIFY(diskState(p.folder) == disk);
            QCOMPARE(journalFiles(p.folder), std::size_t{1});
        }
        {
            auto w = window(*rt, p);
            QVERIFY(w->openProject(p.folder));
            QVERIFY(uitest::showAndActivate(*w));
            QTRY_VERIFY(w->recoveryDialog() != nullptr && w->recoveryDialog()->isVisible());
            w->recoveryDialog()->button(project::JournalRecovery::Discard).click();
            QCOMPARE(journalFiles(p.folder), std::size_t{0});
            QVERIFY(!w->session().isDirty());
            QVERIFY(!graphOf(*w)->channelNames().contains(QStringLiteral("Throttle")));
            QVERIFY(diskState(p.folder) == disk);
        }
        {
            auto w = window(*rt, p);
            QVERIFY(w->openProject(p.folder));
            QTest::qWait(50);
            QVERIFY(w->recoveryDialog() == nullptr); // nothing left to recover
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstProjectRecovery)
#include "tst_project_recovery.moc"
