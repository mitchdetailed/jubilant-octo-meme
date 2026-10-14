// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The shell's M3 wiring (07 section 6.1; 06 sections 2.1-2.2, 4.1-4.5, 9, 13.6, 14.4, 15), offscreen, on a
// private configuration directory, the repository's data directory and a synthetic log: the zoom summary bar
// above the worksheet tabs following the active worksheet; the Track Map dock (AutoHide on the right, its
// context the active worksheet's); animation (A and the toolbar toggle; the cursor advances with Cursor-only
// changes, so the graph submits no Static build; the controller follows worksheet switches; Data > Animation
// Rate writes options.animation.rate as one undo step); F7's Select Worksheet dialog (type to filter, ENTER
// switches workbook and worksheet); the Units list; CTRL+O's Open Log dialog with its Details pane; full
// screen (docks hidden, ESC leaves it with the docks back); the offset rows (O, CTRL+SHIFT+arrow) committing
// one OffsetRangeCommand after the nudges; cursor.add_annotation; geometry edits on the canvas as merged
// workbook commands; ui.layoutLocked locking the canvas; New Project and Close Project through the prompts;
// Close All; the component rows following the focused component (the tool bar's and the menus' actions
// refresh on a focus change; Copy Equation is the scatter's). Every runtime marks the GUI thread NoBlock as
// esses does (AC-M1-12).

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"
#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/shell_dialogs.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTabBar>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QUndoStack>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

constexpr Micros kSec = 1'000'000;

struct Shell {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt = runtime(4);
    std::unique_ptr<MainWindow> w;
    std::filesystem::path ld;
    std::vector<model::RangeRef> laps;

    bool start(bool withLog = true) {
        if (!config.isValid() || !logs.isValid()) {
            return false;
        }
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            qt::test::SyntheticOptions o;
            o.path = "shell.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            ld = qt::test::writeLd(*qt::test::makeSource(o, rt->cache()), fsPath(logs.path()), "shell");
        }
        w = std::make_unique<MainWindow>(*rt, pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        if (!showAndActivate(*w)) {
            return false;
        }
        if (!withLog) {
            return true;
        }
        w->openLog(ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == 1; }, 30000)) {
            return false;
        }
        laps = w->selection().displayedRanges(w->selection().sources().front()->id);
        return laps.size() == 5;
    }
    ~Shell() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }
    TimeDistanceGraph* graph() const {
        for (ComponentWidget* c : w->canvas().components()) {
            if (auto* g = qobject_cast<TimeDistanceGraph*>(c)) {
                return g;
            }
        }
        return nullptr;
    }
};

} // namespace

class TstShellM3 : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void summaryBarAndTrackMap() {
        Shell sh;
        QVERIFY(sh.start());
        ZoomSummaryBar& bar = sh.w->zoomSummaryBar();
        QVERIFY(bar.isVisible());
        QVERIFY(bar.geometry().bottom() <= sh.w->worksheetTabs().geometry().top()); // above the tabs
        QCOMPARE(bar.worksheet(), sh.w->activeWorksheet());
        sh.w->selectWorksheet(1);
        QCOMPARE(bar.worksheet(), sh.w->activeWorksheet());
        QVERIFY(bar.summary().valid);
        sh.w->selectWorksheet(0);
        // The Track Map dock: registered AutoHide on the right; its context is the active worksheet's.
        const QString dock = QStringLiteral("trackmap");
        QVERIFY(sh.w->docks().dockIds().contains(dock));
        QCOMPARE(sh.w->docks().defaultState(dock), DockState::AutoHide);
        QVERIFY(sh.w->trackMapWindow().context() != nullptr);
        QCOMPARE(&sh.w->trackMapWindow().context()->view, sh.w->activeWorksheet());
        QVERIFY(sh.w->commands().invoke(QStringLiteral("view.track_map_window")));
        QVERIFY(sh.w->docks().isVisible(dock));
        QVERIFY(sh.w->commands().invoke(QStringLiteral("view.track_map_window")));
        QVERIFY(!sh.w->docks().isVisible(dock));
    }

    // AC-M3-10's behaviour half (the CPU per frame is bench/plot_bench's animation row).
    void animation() {
        Shell sh;
        QVERIFY(sh.start());
        sh.w->selection().setMain(sh.laps[2]);
        qt::WorksheetViewModel& ws = *sh.w->activeWorksheet();
        ws.zoomToMainRange();
        ws.setCursor(1 * kSec, qt::CursorSource::Programmatic);
        TimeDistanceGraph* g = sh.graph();
        QVERIFY(g != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(g->staticUpToDate(), 30000);
        const int submitted = g->staticBuildsSubmitted();
        AnimationController& a = sh.w->animation();
        QCOMPARE(a.worksheet(), &ws);
        // The toolbar toggle is data.animate.
        bool onToolbar = false;
        for (QAction* act : sh.w->mainToolBar().actions()) {
            onToolbar = onToolbar || act->objectName() == QLatin1String("data.animate");
        }
        QVERIFY(onToolbar);
        QVERIFY(sh.w->commands().invoke(QStringLiteral("data.animate")));
        QVERIFY(a.animating());
        QVERIFY(ws.animating());
        const Micros t0 = ws.state().cursorT;
        QTRY_VERIFY(ws.state().cursorT > t0 + 100'000);
        QCOMPARE(g->staticBuildsSubmitted(), submitted); // Cursor-only changes: Dynamic repaints only
        // A worksheet switch takes the running animation along.
        sh.w->selectWorksheet(1);
        QCOMPARE(a.worksheet(), sh.w->activeWorksheet());
        QVERIFY(sh.w->activeWorksheet()->animating());
        QVERIFY(!ws.animating());
        QVERIFY(sh.w->commands().invoke(QStringLiteral("data.animate")));
        QVERIFY(!a.animating());
        QVERIFY(!sh.w->activeWorksheet()->animating());
        sh.w->selectWorksheet(0);
        // The end of the outing stops it, and the toggle follows.
        ws.setCursor(126 * kSec, qt::CursorSource::Programmatic);
        a.setRate(4.0);
        a.setAnimating(true);
        QTRY_VERIFY_WITH_TIMEOUT(!a.animating(), 10000);
        // Data > Animation Rate: options.animation.rate, one undo step.
        QMenu* data = sh.w->actions().menu(QStringLiteral("Data"));
        QVERIFY(data != nullptr);
        QMenu* rates = data->findChild<QMenu*>(QStringLiteral("menu.Data/Animation Rate"));
        QVERIFY(rates != nullptr);
        QCOMPARE(rates->actions().size(), qsizetype{5});
        const int base = sh.w->session().undoStack().index();
        rates->actions().at(1)->trigger(); // x0.5
        QCOMPARE(a.rate(), 0.5);
        QCOMPARE(sh.w->session().undoStack().index(), base + 1);
        QCOMPARE(jsonptr::get(jsonptr::fromDoc(sh.w->session().projectFile().options),
                              QStringLiteral("/animation/rate"))
                     .toDouble(),
                 0.5);
        QVERIFY(rates->actions().at(1)->isChecked());
    }

    void dialogsOfTheRows() {
        Shell sh;
        QVERIFY(sh.start());
        // F7: Select Worksheet; type to filter, ENTER switches.
        sh.w->canvas().returnFocus();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F7);
        SelectWorksheetDialog* select = sh.w->selectWorksheetDialog();
        QVERIFY(select != nullptr && select->isVisible());
        QCOMPARE(select->current(), (std::pair<int, int>{0, 0})); // the active one preselected
        const QString second = sh.w->worksheetTabs().tabText(1);
        QTest::keyClicks(&select->filterEdit(), second);
        QCOMPARE(select->current().second, 1);
        QTest::keyClick(&select->filterEdit(), Qt::Key_Return);
        QCOMPARE(sh.w->activeWorksheetIndex(), 1);
        QTRY_VERIFY(sh.w->selectWorksheetDialog() == nullptr);
        // The Units list.
        QVERIFY(sh.w->commands().invoke(QStringLiteral("tools.units_list")));
        UnitsListDialog& units = sh.w->unitsList();
        QVERIFY(units.isVisible());
        QVERIFY(units.table().rowCount() > 50);
        units.setFilter(QStringLiteral("km/h"));
        QVERIFY(units.visibleRows() >= 1 && units.visibleRows() < units.table().rowCount());
        units.close();
        // CTRL+O: the Open Log dialog with the Details pane.
        sh.w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(sh.w.get()));
        sh.w->canvas().returnFocus();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_O, Qt::ControlModifier);
        OpenLogDialog* open = sh.w->openLogDialog();
        QVERIFY(open != nullptr && open->isVisible());
        QCOMPARE(open->fileMode(), QFileDialog::ExistingFiles);
        open->showDetails(QString::fromStdWString(sh.ld.wstring()));
        QVERIFY2(open->details().contains(QStringLiteral("Channels:")), qPrintable(open->details()));
        QVERIFY(open->details().contains(QStringLiteral("Format:")));
        open->reject();
        QTRY_VERIFY(sh.w->openLogDialog() == nullptr);
    }

    void fullScreen() {
        Shell sh;
        QVERIFY(sh.start(false));
        QVERIFY(sh.w->docks().isVisible(QStringLiteral("data")));
        QVERIFY(sh.w->commands().invoke(QStringLiteral("view.fullscreen")));
        QVERIFY(sh.w->fullScreenMode());
        QVERIFY(sh.w->isFullScreen());
        QVERIFY(!sh.w->docks().isVisible(QStringLiteral("data")));
        sh.w->canvas().returnFocus();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape); // view.return_focus leaves it
        QVERIFY(!sh.w->fullScreenMode());
        QVERIFY(!sh.w->isFullScreen());
        QVERIFY(sh.w->docks().isVisible(QStringLiteral("data")));
    }

    void offsetsAndAnnotations() {
        Shell sh;
        QVERIFY(sh.start());
        sh.w->selection().setMain(sh.laps[2]);
        qt::WorksheetViewModel& ws = *sh.w->activeWorksheet();
        QUndoStack& stack = sh.w->session().undoStack();
        TimeDistanceGraph* g = sh.graph();
        sh.w->canvas().focusComponent(g);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
        QVERIFY(ws.state().offsetAxis);
        const int base = stack.index();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!ws.pendingOffsets().empty());
        // 11 offset steps of the active channel's sample period, committed once after the nudges.
        QTRY_COMPARE_WITH_TIMEOUT(stack.index(), base + 1, 5000);
        const Micros expected = 11 * ws.activeSamplePeriod();
        QTRY_VERIFY(sh.w->selection().resolve(sh.w->selection().snapshot().main) != nullptr &&
                    sh.w->selection().resolve(sh.w->selection().snapshot().main)->offsetUs == expected);
        stack.undo();
        QTRY_VERIFY(sh.w->selection().resolve(sh.w->selection().snapshot().main)->offsetUs == 0);
        // cursor.add_annotation: the text prompt, one sidecar edit at the cursor's absolute time.
        MainWindow::Prompts prompts;
        prompts.text = [](const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Late braking"));
        };
        sh.w->setPrompts(prompts);
        ws.setCursor(12 * kSec, qt::CursorSource::Programmatic);
        const int before = stack.index();
        QVERIFY(sh.w->commands().invoke(QStringLiteral("cursor.add_annotation")));
        QCOMPARE(stack.index(), before + 1);
        const auto src = sh.w->selection().source(sh.w->selection().snapshot().main.src);
        QVERIFY(src != nullptr);
        bool found = false;
        for (const model::Annotation& a :
             src->sidecar.layer(sh.w->selection().snapshot().main.layer).annotations) {
            found = found || (a.text == "Late braking" &&
                              a.tUs == sh.w->selection().resolve(sh.w->selection().snapshot().main)->start +
                                           12 * kSec);
        }
        QVERIFY(found);
    }

    void canvasEditsAndLock() {
        Shell sh;
        QVERIFY(sh.start(false));
        QUndoStack& stack = sh.w->session().undoStack();
        WorksheetCanvas& canvas = sh.w->canvas();
        ComponentWidget* c = canvas.components().front();
        canvas.focusComponent(c);
        const int base = stack.index();
        QVERIFY(canvas.nudgeSelection(1, 0));
        QVERIFY(canvas.nudgeSelection(1, 0)); // the second move of the same component merges
        QCOMPARE(stack.index(), base + 1);
        const project::WorksheetDef& stored = sh.w->session().workbooks()[0].worksheets[0];
        QCOMPARE(stored.components.front().rect, c->spec().rect);
        stack.undo();
        QCOMPARE(canvas.components().front(), c); // updated in place
        // ui.layoutLocked locks the canvas.
        project::ProjectFile locked = sh.w->session().projectFile();
        locked.layoutLocked = true;
        stack.push(new qt::SetProjectOptionsCommand(sh.w->session(), locked, QStringLiteral("Lock")));
        QVERIFY(canvas.layoutLocked());
        QVERIFY(!canvas.nudgeSelection(1, 0));
        stack.undo();
        QVERIFY(!canvas.layoutLocked());
    }

    // The component rows are enabled by the focused component's key context, refreshed when the focus moves.
    void componentRowsFollowTheFocus() {
        Shell sh;
        QVERIFY(sh.start());
        qt::ProjectSession& s = sh.w->session();
        s.undoStack().push(new qt::AddComponentsCommand(
            s, s.activeWorkbook(), sh.w->activeWorksheetIndex(),
            {componentOf("scatter", exampleBody("scatter", "empty"), project::Rect{0.5, 0.5, 1.0, 1.0}, 9)},
            QStringLiteral("Add a scatter")));
        ComponentWidget* scatter = nullptr;
        for (ComponentWidget* c : sh.w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String("scatter")) {
                scatter = c;
            }
        }
        QVERIFY(scatter != nullptr && sh.graph() != nullptr);
        QAction* equation = sh.w->actions().action(QStringLiteral("component.copy_equation"));
        QAction* vIn = sh.w->actions().action(QStringLiteral("zoom.v_in"));
        QVERIFY(equation != nullptr && vIn != nullptr);
        sh.w->canvas().focusComponent(sh.graph());
        QVERIFY(!equation->isEnabled());
        QVERIFY(vIn->isEnabled());
        sh.w->canvas().focusComponent(scatter);
        QVERIFY(equation->isEnabled());
        QVERIFY(!vIn->isEnabled());
    }

    void projectsAndCloseAll() {
        Shell sh;
        QVERIFY(sh.start());
        QTemporaryDir projects;
        const QString folder = projects.path() + QStringLiteral("/fresh");
        MainWindow::Prompts prompts;
        prompts.folder = [&](const QString&) { return folder; };
        prompts.text = [](const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Fresh"));
        };
        sh.w->setPrompts(prompts);
        QVERIFY(sh.w->commands().invoke(QStringLiteral("file.new_project")));
        QVERIFY(sh.w->session().isOpen());
        QCOMPARE(sh.w->session().projectFile().name, std::string("Fresh"));
        QVERIFY(std::filesystem::exists(fsPath(folder) / "project.json"));
        QVERIFY(sh.w->commands().invoke(QStringLiteral("file.close_project")));
        QVERIFY(!sh.w->session().isOpen());
        QVERIFY(!sh.w->commands().isEnabled(QStringLiteral("file.close_project")));
        // CTRL+A closes every source.
        if (sh.w->selection().sources().empty()) {
            sh.w->openLog(sh.ld);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!sh.w->selection().sources().empty(), 30000);
        sh.w->canvas().returnFocus();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QTRY_VERIFY(sh.w->selection().sources().empty());
    }
};

QTEST_MAIN(TstShellM3)
#include "tst_shell_m3.moc"
