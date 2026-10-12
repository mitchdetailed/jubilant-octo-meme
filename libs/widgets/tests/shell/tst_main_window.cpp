// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MainWindow (06 sections 2-4, 14, 18), offscreen, on a private config directory and the repository's data
// directory: the assembly (menus in the 06 section 2.2 order with key texts, the Main toolbar, the M1 docks,
// the shipped workbook's worksheet tabs, every keymap command registered, planned ones disabled), the view
// hotkeys of batch 1 through the real KeyEventRouter (F8, ESC, V, C, F11/F12, 1-9), session restore of the
// dock layout without a project (<config>/layout.json) and per project (<project>/.local/layout.json, keyed
// by the workbook id), a damaged layout file (info row, default docks), opening a log (-l) into the Data
// window and the global data keys from the worksheet (CTRL+F, F9), and the Channels window ENTER routed to
// the focused component; ESC ends a rubber band in the graph before it returns the focus; a worksheet flip
// (F12, F11) keeps the components of the worksheet left and shows the same ones again; chunk-cache budget
// warnings reach the Messages dock once each, from the newest window, with older windows gone. Every runtime
// marks the GUI thread NoBlock as esses does (AC-M1-12): a blocking wait in the shell aborts the test.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/keymap.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/store/series.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QIdentityProxyModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeView>
#include <memory>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

const QString kData = QStringLiteral("data");
const QString kChannels = QStringLiteral("channels");
const QString kValues = QStringLiteral("values");
const QString kMessages = QStringLiteral("messages");

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

Paths pathsFor(const QString& configDir) {
    PathOverrides o;
    o.configDir = fsPath(configDir);
    o.dataDir = qt::test::repoRoot() / "data";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

std::unique_ptr<qt::AppRuntime> runtime() {
    qt::AppRuntimeOptions o;
    o.workers = 2;
    o.cacheBudget = std::size_t{128} << 20;
    o.markGuiThreadNoBlock = true; // as esses (AC-M1-12); shutdown() restores the role
    return std::make_unique<qt::AppRuntime>(o);
}

// A synthetic circuit log written as .ld + .ldx (writing reads every chunk: a deliberate blocking wait).
std::filesystem::path writeLog(qt::AppRuntime& rt, const QTemporaryDir& dir, const std::string& name) {
    const ThreadRole::Scope blocking(ThreadRole::Normal);
    qt::test::SyntheticOptions o;
    o.path = name + ".ld";
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    const auto src = qt::test::makeSource(o, rt.cache());
    return qt::test::writeLd(*src, fsPath(dir.path()), name);
}

TimeDistanceGraph* graphOf(MainWindow& w) {
    for (ComponentWidget* c : w.canvas().components()) {
        if (auto* g = qobject_cast<TimeDistanceGraph*>(c)) {
            return g;
        }
    }
    return nullptr;
}

void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

bool showAndActivate(QWidget& w) {
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitForWindowActive(&w);
}

bool inCanvas(MainWindow& w) {
    QWidget* f = QApplication::focusWidget();
    return f != nullptr && (f == &w.canvas() || w.canvas().isAncestorOf(f));
}

// Sends a key to the focus widget as the platform would.
void press(MainWindow& w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWidget* target = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : &w;
    QTest::keyClick(target, static_cast<Qt::Key>(key), mods);
}

int countRows(const qt::MessagesModel& m, qt::Message::Severity s, const QString& needle) {
    int n = 0;
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.at(r).severity == s && m.at(r).text.contains(needle, Qt::CaseInsensitive)) {
            ++n;
        }
    }
    return n;
}

} // namespace

class TstMainWindow : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { registerBuiltinComponents(); }

    void assembles() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            QCOMPARE(w.messages().count(qt::Message::Severity::Error), 0);
            // Menus in the 06 section 2.2 order; items show their key (display only).
            QStringList titles;
            for (QAction* a : w.menuBar()->actions()) {
                titles << a->text();
            }
            QCOMPARE(titles, CommandActions::menuOrder());
            QMenu* view = w.actions().menu(QStringLiteral("View"));
            QVERIFY(view != nullptr);
            QAction* f8 = w.actions().action(QStringLiteral("view.data_window"));
            QVERIFY(view->actions().contains(f8));
            QCOMPARE(f8->text(), QStringLiteral("Data Window\tF8"));
            QVERIFY(f8->shortcut().isEmpty()); // keys are routed by the KeyEventRouter only
            // The Main toolbar: open, save | time/distance, overlays, single colour.
            QStringList ids;
            for (QAction* a : w.mainToolBar().actions()) {
                ids << (a->isSeparator() ? QStringLiteral("|") : a->objectName());
            }
            QCOMPARE(ids, (QStringList{QStringLiteral("file.open_log"), QStringLiteral("file.save_project"),
                                       QStringLiteral("|"), QStringLiteral("data.toggle_domain"),
                                       QStringLiteral("data.overlays_toggle"),
                                       QStringLiteral("data.single_colour_main")}));
            QVERIFY(w.actions().action(QStringLiteral("data.toggle_domain"))->isCheckable());
            QAction* overlays = w.actions().action(QStringLiteral("data.overlays_toggle"));
            QVERIFY(overlays->isCheckable());
            QVERIFY(overlays->isChecked());                                                 // overlays shown
            QVERIFY(!w.actions().action(QStringLiteral("file.save_project"))->isEnabled()); // no project
            // Every keymap command is registered; planned ones are greyed with their milestone.
            for (const project::KeyBinding& row : w.session().keymap().bindings) {
                QVERIFY2(w.commands().find(QString::fromStdString(row.command)) != nullptr,
                         row.command.c_str());
            }
            QAction* print = w.actions().action(QStringLiteral("file.print"));
            QVERIFY(!print->isEnabled());
            QVERIFY(print->toolTip().contains(QStringLiteral("planned for v1.0")));
            QVERIFY(w.commands().dumpJson().contains(QStringLiteral("\"view.data_window\"")));
            // The M1 docks with their 06 section 3.2 defaults.
            QCOMPARE(w.docks().dockIds(), (QStringList{kData, kChannels, kValues, kMessages}));
            QCOMPARE(w.docks().state(kData), DockState::Docked);
            QCOMPARE(w.docks().state(kMessages), DockState::AutoHide);
            // The shipped workbook: two worksheets, "Overlay" active (its activeWorksheet), components
            // placed.
            QCOMPARE(w.worksheetCount(), 2);
            QCOMPARE(w.worksheetTabs().count(), 2);
            QCOMPARE(w.worksheetTabs().tabText(0), QStringLiteral("Overlay"));
            QCOMPARE(w.worksheetTabs().tabText(1), QStringLiteral("Driver"));
            QCOMPARE(w.activeWorksheetIndex(), 0);
            QVERIFY(w.activeWorksheet() != nullptr);
            QCOMPARE(w.canvas().components().size(), 2);
            QCOMPARE(w.workbookSelector().count(), 1);
            QVERIFY(w.statusField(QStringLiteral("t")) != nullptr);
            QVERIFY(w.valuesWindow().worksheet() == w.activeWorksheet());
            QCOMPARE(w.windowTitle(), QStringLiteral("Esses"));
        }
        rt->shutdown();
    }

    void viewHotkeys() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            w.returnFocus();
            QVERIFY(inCanvas(w));

            press(w, Qt::Key_F8); // view.data_window: show and focus the Data dock
            QCOMPARE(w.docks().focusedDockId(), kData);
            QVERIFY(QApplication::focusWidget() == &w.dataWindow().tree());
            press(w, Qt::Key_Escape); // view.return_focus
            QVERIFY(inCanvas(w));

            press(w, Qt::Key_V); // view.values_window: toggle, focus unchanged
            QCOMPARE(w.docks().state(kValues), DockState::Closed);
            QVERIFY(inCanvas(w));
            press(w, Qt::Key_V);
            QCOMPARE(w.docks().state(kValues), DockState::Docked);

            press(w, Qt::Key_C); // view.channels_window: the caret in the search box
            QCOMPARE(w.docks().focusedDockId(), kChannels);
            QVERIFY(QApplication::focusWidget() == &w.channelsWindow().search());
            // Plain letters typed there stay in the search box (no V toggling, no worksheet switch).
            press(w, Qt::Key_V);
            press(w, Qt::Key_2);
            QCOMPARE(w.channelsWindow().search().text(), QStringLiteral("v2"));
            QCOMPARE(w.docks().state(kValues), DockState::Docked);
            QCOMPARE(w.activeWorksheetIndex(), 0);
            press(w, Qt::Key_Escape);
            QVERIFY(inCanvas(w));
            QCOMPARE(w.channelsWindow().search().text(), QStringLiteral("v2")); // the search text is kept

            press(w, Qt::Key_F12); // view.next_worksheet
            QCOMPARE(w.activeWorksheetIndex(), 1);
            QCOMPARE(w.worksheetTabs().currentIndex(), 1);
            QVERIFY(w.valuesWindow().worksheet() == w.activeWorksheet());
            press(w, Qt::Key_F12); // wraps
            QCOMPARE(w.activeWorksheetIndex(), 0);
            press(w, Qt::Key_F11); // view.prev_worksheet, wraps
            QCOMPARE(w.activeWorksheetIndex(), 1);
            QVERIFY(inCanvas(w)); // the switch keeps the focus in the worksheet
            press(w, Qt::Key_1);  // view.worksheet_n
            QCOMPARE(w.activeWorksheetIndex(), 0);
            press(w, Qt::Key_2);
            QCOMPARE(w.activeWorksheetIndex(), 1);
            press(w, Qt::Key_9); // no ninth worksheet: no-op
            QCOMPARE(w.activeWorksheetIndex(), 1);
            // F8 also works from a dock (function keys go to the binder first).
            press(w, Qt::Key_C);
            QVERIFY(QApplication::focusWidget() == &w.channelsWindow().search());
            press(w, Qt::Key_F8);
            QCOMPARE(w.docks().focusedDockId(), kData);
            // A tab click switches worksheets as well.
            w.worksheetTabs().setCurrentIndex(0);
            QCOMPARE(w.activeWorksheetIndex(), 0);
        }
        rt->shutdown();
    }

    void sessionRestoresDockLayout() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow a(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(a));
            a.docks().setState(kValues, DockState::Floating);
            a.docks().setState(kData, DockState::AutoHide);
            a.docks().setState(kChannels, DockState::Closed);
            a.valuesWindow().setAllChannels(true);
            a.selectWorksheet(1);
            a.close(); // closeEvent writes the layout (exit)
        }
        const QString file = config.filePath(QStringLiteral("layout.json"));
        QVERIFY(QFile::exists(file));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(root.value(QStringLiteral("$schema")).toString(), QStringLiteral("urn:esses:layout-1"));
            QCOMPARE(
                root.value(QStringLiteral("active")).toObject().value(QStringLiteral("worksheet")).toString(),
                QStringLiteral("Driver"));
            QVERIFY(
                root.value(QStringLiteral("perspectives")).toObject().contains(QStringLiteral("_default")));
            QCOMPARE(root.value(QStringLiteral("docks"))
                         .toObject()
                         .value(kValues)
                         .toObject()
                         .value(QStringLiteral("state"))
                         .toString(),
                     QStringLiteral("Floating"));
        }
        {
            MainWindow b(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(b));
            QCOMPARE(b.docks().state(kValues), DockState::Floating);
            QCOMPARE(b.docks().state(kData), DockState::AutoHide);
            QCOMPARE(b.docks().state(kChannels), DockState::Closed);
            QCOMPARE(b.docks().state(kMessages), DockState::AutoHide);
            QCOMPARE(b.activeWorksheetIndex(), 1);
            QVERIFY(b.valuesWindow().allChannels());
            QVERIFY(b.valuesWindow().allChannelsToggle().isChecked());
            // Reset Dock Windows: the defaults, written at once.
            QVERIFY(b.commands().invoke(QStringLiteral("view.reset_docks")));
            QCOMPARE(b.docks().state(kValues), DockState::Docked);
            QCOMPARE(b.docks().state(kData), DockState::Docked);
            QCOMPARE(b.docks().state(kChannels), DockState::Docked);
        }
        {
            MainWindow c(*rt, pathsFor(config.path()));
            QCOMPARE(c.docks().state(kValues), DockState::Docked);
            QCOMPARE(c.docks().state(kData), DockState::Docked);
        }
        rt->shutdown();
    }

    void projectLayoutIsPerProject() {
        QTemporaryDir config;
        QTemporaryDir projects;
        const std::filesystem::path folder = fsPath(projects.filePath(QStringLiteral("Spa")));
        auto rt = runtime();
        QString workbookId;
        {
            MainWindow a(*rt, pathsFor(config.path()));
            QVERIFY(a.session().newProject(folder, "Spa").ok());
            QVERIFY(a.openProject(folder / "project.json")); // its project.json is accepted too
            QCOMPARE(a.windowTitle(), QStringLiteral("Spa - Esses"));
            QVERIFY(a.actions().action(QStringLiteral("file.save_project"))->isEnabled());
            QCOMPARE(a.layoutFilePath(), folder / ".local" / "layout.json");
            workbookId = QString::fromStdString(a.session().workbooks().front().id.str());
            a.docks().setState(kMessages, DockState::Docked);
            a.docks().setState(kValues, DockState::Closed);
        } // the destructor writes the layout (exit)
        {
            QFile f(QString::fromStdWString((folder / ".local" / "layout.json").wstring()));
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            QVERIFY(root.value(QStringLiteral("perspectives")).toObject().contains(workbookId));
            QCOMPARE(
                root.value(QStringLiteral("active")).toObject().value(QStringLiteral("workbook")).toString(),
                workbookId);
        }
        {
            MainWindow b(*rt, pathsFor(config.path()));
            // Without the project: the per-user layout (written when `a` left it), i.e. the defaults.
            QCOMPARE(b.docks().state(kMessages), DockState::AutoHide);
            QCOMPARE(b.docks().state(kValues), DockState::Docked);
            QVERIFY(b.openProject(folder));
            QCOMPARE(b.docks().state(kMessages), DockState::Docked);
            QCOMPARE(b.docks().state(kValues), DockState::Closed);
            QVERIFY(!b.openProject(fsPath(projects.filePath(QStringLiteral("missing")))));
            QCOMPARE(b.messages().count(qt::Message::Severity::Error), 1);
            QVERIFY(b.session().isOpen()); // the open project stays
        }
        rt->shutdown();
    }

    void damagedLayoutResetsTheDocks() {
        QTemporaryDir config;
        {
            QFile f(config.filePath(QStringLiteral("layout.json")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ this is not json");
        }
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QCOMPARE(countRows(w.messages(), qt::Message::Severity::Info, QStringLiteral("layout")), 1);
            QCOMPARE(w.docks().state(kData), DockState::Docked);
            QCOMPARE(w.docks().state(kMessages), DockState::AutoHide);
        }
        // A perspective of another dock backend (e.g. written by the ADS build) is refused the same way.
        {
            QFile f(config.filePath(QStringLiteral("layout.json")));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(
                R"({"$schema":"urn:esses:layout-1","schemaVersion":1,"perspectives":{"_default":{"ads":"AAAA"}}})");
        }
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QCOMPARE(countRows(w.messages(), qt::Message::Severity::Info, QStringLiteral("dock layout")), 1);
            QCOMPARE(w.docks().state(kData), DockState::Docked);
        }
        rt->shutdown();
    }

    void opensALogAndDrivesItWithKeys() {
        QTemporaryDir config;
        QTemporaryDir logs;
        auto rt = runtime();
        {
            const std::filesystem::path ld = writeLog(*rt, logs, "shell-open");

            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            w.openLog(ld);
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, 30000);
            QCOMPARE(w.dataWindow().viewModel().rowCount(), 1);
            QVERIFY(w.selection().hasMain());
            QVERIFY(w.settings().recentLogs().size() == 1);
            // The Import Log reached the Messages dock.
            bool imported = false;
            for (int r = 0; r < w.messages().rowCount(); ++r) {
                imported = imported || w.messages().at(r).origin == qt::Message::Origin::Import;
            }
            QVERIFY(imported);

            // CTRL+F and F9 from the worksheet (global context).
            w.returnFocus();
            QVERIFY(inCanvas(w));
            press(w, Qt::Key_F, Qt::ControlModifier);
            const model::Range* main = w.selection().resolve(w.selection().snapshot().main);
            QVERIFY(main != nullptr);
            QCOMPARE(main->duration(), Micros{48'000'000});
            QVERIFY(*w.dataWindow().highlightedRange() == w.selection().snapshot().main);
            QCOMPARE(w.activeWorksheet()->domain(), plot::Domain::Time);
            press(w, Qt::Key_F9);
            QCOMPARE(w.activeWorksheet()->domain(), plot::Domain::Distance);
            QVERIFY(w.actions().action(QStringLiteral("data.toggle_domain"))->isChecked());
            press(w, Qt::Key_F4); // overlays hidden: the toolbar button follows
            QVERIFY(w.selection().snapshot().overlaysHidden);
            QVERIFY(!w.actions().action(QStringLiteral("data.overlays_toggle"))->isChecked());
            // The toolbar button runs the same command.
            w.actions().action(QStringLiteral("data.overlays_toggle"))->trigger();
            QVERIFY(!w.selection().snapshot().overlaysHidden);
            QVERIFY(w.actions().action(QStringLiteral("data.overlays_toggle"))->isChecked());

            // The Channels window: ENTER on a channel goes to the focused component (06 section 7 item 4).
            ComponentWidget* focused = w.canvas().focusedComponent();
            QVERIFY(focused != nullptr);
            press(w, Qt::Key_C);
            QTRY_VERIFY(!w.channelsWindow().visibleChannels().isEmpty());
            QTest::keyClicks(&w.channelsWindow().search(), QStringLiteral("thrott"));
            QCOMPARE(w.channelsWindow().currentChannel(), QStringLiteral("Throttle"));
            QSignalSpy requested(&w.channelsWindow(), &ChannelsWindow::addChannelRequested);
            press(w, Qt::Key_Return);
            QCOMPARE(requested.count(), 1);
            QVERIFY(focused->acceptsChannelDrop()); // the time_distance graph
            QVERIFY(focused->channelNames().contains(QStringLiteral("Throttle")));

            // A channel dragged from the Channels window onto the graph (06 section 7 item 5).
            QMimeData mime;
            mime.setData(QString::fromLatin1(ChannelsWindow::kMimeType),
                         qt::ChannelsModel::channelsMime({QStringLiteral("Wheel Speed FL")}, false));
            const QPoint at = focused->rect().center();
            QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(focused, &enter);
            QVERIFY(enter.isAccepted());
            QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(focused, &drop);
            QVERIFY(drop.isAccepted());
            QVERIFY(focused->channelNames().contains(QStringLiteral("Wheel Speed FL")));
            // The edit landed in the session's workbook (saved with the project).
            bool stored = false;
            for (const project::ComponentSpec& c :
                 w.session().workbooks().front().worksheets.front().components) {
                stored = stored || c.body.compact().find("Wheel Speed FL") != std::string::npos;
            }
            QVERIFY(stored);
        }
        rt->shutdown();
    }

    // 04 section 8.6: ESC in the graph drops a rubber band in progress (view.return_focus offers ESC to the
    // focused component first); the zoom is unchanged and the focus stays in the graph. The band follows the
    // pointer between the double-click and the closing click without a button held.
    void escEndsTheRubberBand() {
        QTemporaryDir config;
        QTemporaryDir logs;
        auto rt = runtime();
        {
            const std::filesystem::path ld = writeLog(*rt, logs, "shell-band");
            MainWindow w(*rt, pathsFor(config.path()));
            w.resize(1400, 900);
            QVERIFY(showAndActivate(w));
            w.openLog(ld);
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, 30000);
            TimeDistanceGraph* g = graphOf(w);
            QVERIFY(g != nullptr);
            w.canvas().focusComponent(g);
            QTRY_VERIFY_WITH_TIMEOUT(g->staticUpToDate() && !g->layout().groups.empty(), 30000);
            const plot::RectPx panel = g->layout().groups.front().panel;
            const auto dpr = static_cast<double>(g->layout().dpr);
            const double y = (static_cast<double>(panel.y) + 0.5 * static_cast<double>(panel.h)) / dpr;
            const double x1 = (static_cast<double>(panel.x) + 0.3 * static_cast<double>(panel.w)) / dpr;
            const double x2 = (static_cast<double>(panel.x) + 0.6 * static_cast<double>(panel.w)) / dpr;
            const plot::XSpan zoom = w.activeWorksheet()->state().zoom;
            sendMouse(g, QEvent::MouseButtonPress, {x1, y}, Qt::LeftButton, Qt::LeftButton);
            sendMouse(g, QEvent::MouseButtonRelease, {x1, y}, Qt::LeftButton, Qt::NoButton);
            sendMouse(g, QEvent::MouseButtonDblClick, {x1, y}, Qt::LeftButton, Qt::LeftButton);
            sendMouse(g, QEvent::MouseButtonRelease, {x1, y}, Qt::LeftButton, Qt::NoButton);
            QVERIFY(g->rubberBandActive());
            QVERIFY(g->hasMouseTracking()); // button-less moves reach the graph while the band is open
            sendMouse(g, QEvent::MouseMove, {x2, y}, Qt::NoButton, Qt::NoButton);
            QTRY_VERIFY(g->hasFocus());
            const Micros cursorBefore = w.activeWorksheet()->state().cursorT;
            press(w, Qt::Key_Escape);
            QVERIFY(!g->rubberBandActive());
            QVERIFY(!g->hasMouseTracking());
            QCOMPARE(w.activeWorksheet()->state().zoom, zoom);
            QVERIFY(g->hasFocus());
            QCOMPARE(w.activeWorksheet()->state().cursorT, cursorBefore);
            // The next ESC is view.return_focus as before (the focus stays in the worksheet).
            press(w, Qt::Key_Escape);
            QVERIFY(inCanvas(w));
        }
        rt->shutdown();
    }

    // D-058 (04 section 6.2): a worksheet switched away from keeps its components hidden; switching back
    // shows the same widgets with their Static images at once (no blank frame; a rebuild follows only because
    // the shared zoom went through the distance worksheet and back, D-133). Selecting the shown worksheet
    // again does nothing; a workbook change (rebuildWorksheets) destroys them.
    void worksheetFlipKeepsComponents() {
        QTemporaryDir config;
        QTemporaryDir logs;
        auto rt = runtime();
        {
            const std::filesystem::path ld = writeLog(*rt, logs, "shell-flip");
            MainWindow w(*rt, pathsFor(config.path()));
            w.resize(1400, 900);
            QVERIFY(showAndActivate(w));
            QVERIFY(w.worksheetCount() >= 2);
            w.openLog(ld);
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, 30000);
            QPointer<TimeDistanceGraph> g = graphOf(w);
            QVERIFY(!g.isNull());
            // Quiet first: the lap indexes and every late result have arrived and no build is pending.
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, 30000);
            int submitted = -1;
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    const bool quiet = g->staticUpToDate() && !g->layers().staticImage().isNull() &&
                                       g->staticBuildsSubmitted() == submitted;
                    submitted = g->staticBuildsSubmitted();
                    QTest::qWait(150);
                    return quiet;
                }(),
                30000);
            const int first = w.activeWorksheetIndex();
            w.returnFocus();
            press(w, Qt::Key_F12);
            QCOMPARE(w.activeWorksheetIndex(), (first + 1) % w.worksheetCount());
            QVERIFY(!g.isNull());     // kept, not destroyed
            QVERIFY(!g->isVisible()); // hidden with its worksheet
            QCOMPARE(w.canvas().keptWorksheetCount(), 1);
            press(w, Qt::Key_F11);
            QCOMPARE(w.activeWorksheetIndex(), first);
            QVERIFY(!g.isNull());
            QCOMPARE(graphOf(w), g.data()); // the same widget again
            QVERIFY(g->isVisible());
            QVERIFY(!g->layers().staticImage().isNull()); // its Static image was kept: painted at once
            QTRY_VERIFY_WITH_TIMEOUT(g->staticUpToDate(), 30000);
            QVERIFY(g->staticBuildsSubmitted() <= submitted + 1);
            // The shown worksheet selected again: nothing changes, nothing is rebuilt.
            const int settled = g->staticBuildsSubmitted();
            w.selectWorksheet(first);
            QCOMPARE(graphOf(w), g.data());
            QTest::qWait(100);
            QCOMPARE(g->staticBuildsSubmitted(), settled);
            // A project or workbook change rebuilds the worksheets: every component is destroyed.
            w.canvas().clear();
            QVERIFY(g.isNull());
            QCOMPARE(w.canvas().keptWorksheetCount(), 0);
        }
        rt->shutdown();
    }

    // D-011, 06 section 11: a chunk-cache budget warning is one Budget row (the developer-log record of the
    // same event is not posted again as a System row). With two windows on one runtime, closing the older
    // one leaves the newer window's hook and Messages sink in place and removes its own sink.
    void budgetWarningsReachMessagesOnce() {
        QTemporaryDir config;
        QTemporaryDir second;
        QTemporaryDir logs;
        auto rt = runtime();
        {
            auto older = std::make_unique<MainWindow>(*rt, pathsFor(config.path()));
            MainWindow newer(*rt, pathsFor(second.path()));
            older.reset(); // its sink and its hook ownership go with it; the newer window keeps its own
            std::shared_ptr<const model::DataSource> src;
            {
                const ThreadRole::Scope blocking(ThreadRole::Normal);
                qt::test::SyntheticOptions o;
                o.path = "budget.ld";
                src = qt::test::makeSource(o, rt->cache());
                rt->cache().setBudgetBytes(1);
                const store::Column& col = store::seriesValues(src->layers.front().channels.front().series);
                auto pinned = col.require(0, 0, Priority::Interactive, CancelToken{});
                QVERIFY(pinned.ok());
            }
            const auto rows = [&newer](qt::Message::Origin origin) {
                int n = 0;
                for (int r = 0; r < newer.messages().rowCount(); ++r) {
                    n += newer.messages().at(r).origin == origin &&
                                 newer.messages().at(r).severity == qt::Message::Severity::Warning
                             ? 1
                             : 0;
                }
                return n;
            };
            QTRY_COMPARE(rows(qt::Message::Origin::Budget), 1);
            QTest::qWait(100);
            QCOMPARE(rows(qt::Message::Origin::Budget), 1);
            QCOMPARE(rows(qt::Message::Origin::System), 0); // no second row for the same warning
            rt->cache().setBudgetBytes(std::size_t{128} << 20);
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstMainWindow)
#include "tst_main_window.moc"
