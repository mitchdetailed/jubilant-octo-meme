// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The shell's M2 wiring (06 sections 1, 2.1-2.2, 13.7, 14.2, 16, 18; D-032), offscreen, on a private
// configuration directory and the repository's data directory: the Edit menu's Undo/Redo, Tools > Options and
// the Data menu's lap-stretch toggle; the toolbar (lap stretch, the scheme selector shown by Options >
// Colours, undo, redo); component edits from the canvas are undoable commands, CTRL+Z / CTRL+Y and the
// toolbar undo and redo them and the canvas follows; CTRL+Z in a text field is the field's own undo; the
// lap-stretch toggle is view state; the Options dialog opens window-modal (the keymap pauses) and its
// geometry goes to layout.json; a scheme switch reaches the worksheet's graph, the Data window and the
// application palette; leaving a dirty project saves it (save on exit) or asks Save / Discard / Cancel, an
// exit with view-state changes writes them, and the Messages dock follows Options > General "auto-show
// Messages on error"; the RecoveryDialog's list and answers. Every runtime marks the GUI thread NoBlock as
// esses does (AC-M1-12).

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/journal.hpp>
#include <esses/project/rgba.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/recovery_dialog.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QUndoStack>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

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
    o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
    return std::make_unique<qt::AppRuntime>(o);
}

std::filesystem::path writeLog(qt::AppRuntime& rt, const QTemporaryDir& dir, const std::string& name) {
    const ThreadRole::Scope blocking(ThreadRole::Normal); // writing reads every chunk
    qt::test::SyntheticOptions o;
    o.path = name + ".ld";
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    const auto src = qt::test::makeSource(o, rt.cache());
    return qt::test::writeLd(*src, fsPath(dir.path()), name);
}

bool showAndActivate(QWidget& w) {
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitForWindowActive(&w);
}

void press(MainWindow& w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWidget* target = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : &w;
    QTest::keyClick(target, static_cast<Qt::Key>(key), mods);
}

TimeDistanceGraph* graphOf(MainWindow& w) {
    for (ComponentWidget* c : w.canvas().components()) {
        if (auto* g = qobject_cast<TimeDistanceGraph*>(c)) {
            return g;
        }
    }
    return nullptr;
}

QStringList toolbarIds(MainWindow& w) {
    QStringList ids;
    for (QAction* a : w.mainToolBar().actions()) {
        ids << (a->isSeparator() ? QStringLiteral("|") : a->objectName());
    }
    return ids;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

QJsonObject jsonFile(const std::filesystem::path& p) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(readAll(p))).object();
}

// One undoable options edit (options.reports.probeSec), as the Options dialog would push it.
void editOptions(qt::ProjectSession& session, double probeSec) {
    project::ProjectFile next = session.projectFile();
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
    QJsonObject reports = o.value(QStringLiteral("reports")).toObject();
    reports.insert(QStringLiteral("probeSec"), probeSec);
    o.insert(QStringLiteral("reports"), reports);
    next.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
    session.undoStack().push(
        new qt::SetProjectOptionsCommand(session, std::move(next), QStringLiteral("Probe")));
}

// Sets a general option without the undo stack (a project as configured earlier).
void setGeneral(qt::ProjectSession& session, const char* name, bool value) {
    project::ProjectFile next = session.projectFile();
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
    QJsonObject general = o.value(QStringLiteral("general")).toObject();
    general.insert(QLatin1String(name), value);
    o.insert(QStringLiteral("general"), general);
    next.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
    session.setProjectFile(std::move(next));
}

// options.reports.probeSec of a project.json on disk (0 when absent).
double probeSecOnDisk(const std::filesystem::path& folder) {
    return jsonFile(folder / "project.json")
        .value(QStringLiteral("options"))
        .toObject()
        .value(QStringLiteral("reports"))
        .toObject()
        .value(QStringLiteral("probeSec"))
        .toDouble();
}

QColor colourOf(const project::Scheme& s, const char* role) {
    const project::Rgba c = s.roles.at(role);
    return QColor(project::red(c), project::green(c), project::blue(c), project::alpha(c));
}

} // namespace

class TstShellM2 : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { registerBuiltinComponents(); }

    void menusAndToolbar() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            QMenu* edit = w.actions().menu(QStringLiteral("Edit"));
            QAction* undo = w.actions().action(QStringLiteral("edit.undo"));
            QAction* redo = w.actions().action(QStringLiteral("edit.redo"));
            QVERIFY(edit != nullptr && edit->actions().contains(undo) && edit->actions().contains(redo));
            QCOMPARE(undo->text(), QStringLiteral("Undo\tCtrl+Z"));
            QCOMPARE(redo->text(), QStringLiteral("Redo\tCtrl+Y"));
            QVERIFY(!undo->isEnabled() && !redo->isEnabled()); // nothing to undo yet
            QAction* options = w.actions().action(QStringLiteral("tools.options"));
            QVERIFY(w.actions().menu(QStringLiteral("Tools"))->actions().contains(options));
            QVERIFY(options->isEnabled());
            QAction* stretch = w.actions().action(QStringLiteral("data.lap_stretch_toggle"));
            QVERIFY(w.actions().menu(QStringLiteral("Data"))->actions().contains(stretch));
            QVERIFY(stretch->isCheckable() && stretch->isChecked()); // on by default (D-016)
            QCOMPARE(toolbarIds(w),
                     (QStringList{QStringLiteral("file.open_log"), QStringLiteral("file.save_project"),
                                  QStringLiteral("|"), QStringLiteral("data.toggle_domain"),
                                  QStringLiteral("data.overlays_toggle"),
                                  QStringLiteral("data.single_colour_main"),
                                  QStringLiteral("data.lap_stretch_toggle"), QStringLiteral("|"),
                                  QStringLiteral("schemeSelector"), QStringLiteral("|"),
                                  QStringLiteral("edit.undo"), QStringLiteral("edit.redo")}));
            // The scheme selector shows on the toolbar only when Options > Colours turns it on.
            QAction* selector = w.mainToolBar().findChild<QAction*>(QStringLiteral("schemeSelector"));
            QVERIFY(selector != nullptr);
            QVERIFY(!selector->isVisible());
            w.settings().setValue(QString::fromLatin1(SchemeSelector::kToolbarKey), true);
            QVERIFY(selector->isVisible());
            QVERIFY(w.schemeSelector().schemeNames().size() >= 2);
            w.settings().setValue(QString::fromLatin1(SchemeSelector::kToolbarKey), false);
            QVERIFY(!selector->isVisible());
        }
        rt->shutdown();
    }

    void componentEditsAreUndoable() {
        QTemporaryDir config;
        QTemporaryDir logs;
        auto rt = runtime();
        {
            const std::filesystem::path ld = writeLog(*rt, logs, "shell-undo");
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            w.openLog(ld);
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, 30000);
            const project::WorkbookFile before = w.session().workbooks().front();
            const project::WorksheetDef shownBefore = w.canvas().worksheet();
            TimeDistanceGraph* g = graphOf(w);
            QVERIFY(g != nullptr);
            QVERIFY(!g->channelNames().contains(QStringLiteral("Throttle")));
            QUndoStack& stack = w.session().undoStack();

            // An edit through the component is one command; the toolbar and the menu offer its undo.
            g->addChannel(QStringLiteral("Throttle"), true);
            QCOMPARE(stack.count(), 1);
            const project::WorkbookFile after = w.session().workbooks().front();
            QVERIFY(!(after == before));
            QAction* undo = w.actions().action(QStringLiteral("edit.undo"));
            QAction* redo = w.actions().action(QStringLiteral("edit.redo"));
            QVERIFY(undo->isEnabled());
            QVERIFY(!redo->isEnabled());
            QVERIFY(undo->toolTip().contains(stack.undoText()));
            // A selection change is never on the stack (D-032).
            w.selection().setSingleColourMain(true);
            QCOMPARE(stack.count(), 1);

            // CTRL+Z from the worksheet: the session's workbook and the canvas go back.
            w.canvas().focusComponent(graphOf(w));
            graphOf(w)->setFocus(Qt::OtherFocusReason);
            press(w, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(stack.index(), 0);
            QVERIFY(w.session().workbooks().front() == before);
            QVERIFY(w.canvas().worksheet() == shownBefore);
            QVERIFY(!graphOf(w)->channelNames().contains(QStringLiteral("Throttle")));
            QVERIFY(redo->isEnabled());
            QVERIFY(!undo->isEnabled());
            // Redo from the toolbar, undo again from the Data window (a global key), CTRL+Y from there.
            redo->trigger();
            QVERIFY(w.session().workbooks().front() == after);
            QVERIFY(graphOf(w)->channelNames().contains(QStringLiteral("Throttle")));
            w.docks().focus(QStringLiteral("data"));
            press(w, Qt::Key_Z, Qt::ControlModifier);
            QVERIFY(w.session().workbooks().front() == before);
            press(w, Qt::Key_Y, Qt::ControlModifier);
            QVERIFY(w.session().workbooks().front() == after);
            QVERIFY(w.selection().snapshot().singleColourMain); // view state untouched by undo/redo
        }
        rt->shutdown();
    }

    void textFieldsKeepTheirUndo() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            editOptions(w.session(), 7.0);
            QUndoStack& stack = w.session().undoStack();
            QCOMPARE(stack.index(), 1);
            w.returnFocus();
            press(w, Qt::Key_C); // the Channels window's search box
            QLineEdit& search = w.channelsWindow().search();
            QTRY_VERIFY(QApplication::focusWidget() == &search);
            QTest::keyClicks(&search, QStringLiteral("abc"));
            QCOMPARE(search.text(), QStringLiteral("abc"));
            press(w, Qt::Key_Z, Qt::ControlModifier); // text undo (06 section 13.2 item 6)
            QVERIFY(search.text() != QStringLiteral("abc"));
            QCOMPARE(stack.index(), 1); // the project edit stays
            // Outside a text field CTRL+Z is the project's undo.
            w.returnFocus();
            press(w, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(stack.index(), 0);
        }
        rt->shutdown();
    }

    void lapStretchToggleIsViewState() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            QAction* toggle = w.actions().action(QStringLiteral("data.lap_stretch_toggle"));
            QVERIFY(w.lapStretch().enabled());
            toggle->trigger();
            QVERIFY(!w.lapStretch().enabled());
            QVERIFY(!toggle->isChecked());
            QVERIFY(!w.session().lapStretchSettings().enabled);
            QCOMPARE(w.session().undoStack().count(), 0); // D-174: view state
            w.commands().invoke(QStringLiteral("data.lap_stretch_toggle"));
            QVERIFY(w.lapStretch().enabled());
            QVERIFY(toggle->isChecked());
        }
        rt->shutdown();
    }

    void optionsDialogFromTheToolsMenu() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            w.actions().action(QStringLiteral("tools.options"))->trigger();
            OptionsDialog& d = w.optionsDialog();
            QTRY_VERIFY(d.isVisible());
            QCOMPARE(d.windowModality(), Qt::WindowModal);
            QCOMPARE(QApplication::activeModalWidget(), &d);
            // While it is open the keymap waits (F8 would focus the Data window).
            QTest::keyClick(&d, Qt::Key_F8);
            QVERIFY(w.docks().focusedDockId() != QStringLiteral("data"));
            d.resize(640, 480);
            d.reject();
            QVERIFY(!d.isVisible());
            QVERIFY(w.saveLayout());
            const QJsonObject layout = jsonFile(w.layoutFilePath());
            QVERIFY(!layout.value(QStringLiteral("dialogs"))
                         .toObject()
                         .value(QString::fromLatin1(OptionsDialog::kGeometryKey))
                         .toObject()
                         .value(QStringLiteral("geometry"))
                         .toString()
                         .isEmpty());
            // An applied option is one undoable edit of the window's session.
            QVERIFY(w.commands().invoke(QStringLiteral("tools.options")));
            QTRY_VERIFY(d.isVisible());
            d.findChild<QCheckBox*>(QStringLiteral("general.looseChannelSearch"))->setChecked(true);
            d.accept();
            QVERIFY(!d.isVisible());
            QVERIFY(w.session().looseChannelSearch());
            QCOMPARE(w.session().undoStack().count(), 1);
            QVERIFY(w.actions().action(QStringLiteral("edit.undo"))->isEnabled());
        }
        rt->shutdown();
    }

    void schemeSwitchRepaints() {
        QTemporaryDir config;
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(showAndActivate(w));
            const std::shared_ptr<const project::Scheme> dark = w.session().shippedScheme(true);
            QTRY_VERIFY(graphOf(w) != nullptr && graphOf(w)->inputs().scheme != nullptr);
            QVERIFY(w.schemeSelector().selectScheme(QString::fromStdString(dark->name)));
            QCOMPARE(w.session().scheme(), dark);
            QTRY_VERIFY(graphOf(w)->inputs().scheme != nullptr &&
                        graphOf(w)->inputs().scheme->name == dark->name);
            QCOMPARE(QApplication::palette().color(QPalette::Window), colourOf(*dark, "windowBackground"));
            QCOMPARE(QApplication::palette().color(QPalette::Base), colourOf(*dark, "dataBackground"));
            // The other worksheet's components get the new scheme when shown.
            w.selectWorksheet(1);
            ComponentWidget* c =
                w.canvas().components().isEmpty() ? nullptr : w.canvas().components().front();
            QVERIFY(c != nullptr);
            if (auto* g = qobject_cast<TimeDistanceGraph*>(c)) {
                QTRY_VERIFY(g->inputs().scheme != nullptr && g->inputs().scheme->name == dark->name);
            }
            QCOMPARE(w.settings().value(QString::fromLatin1(SchemeSelector::kActiveKey)).toString(),
                     QString::fromStdString(dark->name));
            QCOMPARE(w.session().undoStack().count(), 0);
            w.schemeSelector().selectScheme(QString::fromStdString(w.session().shippedScheme(false)->name));
        }
        // The next window starts with the user's choice.
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QCOMPARE(w.session().scheme(), w.session().shippedScheme(false));
        }
        rt->shutdown();
    }

    void leavingADirtyProject() {
        QTemporaryDir config;
        QTemporaryDir projects;
        const std::filesystem::path folder = fsPath(projects.filePath(QStringLiteral("Exit")));
        const std::filesystem::path other = fsPath(projects.filePath(QStringLiteral("Other")));
        auto rt = runtime();
        {
            MainWindow w(*rt, pathsFor(config.path()));
            QVERIFY(w.session().newProject(folder, "Exit").ok());
            QVERIFY(w.session().newProject(other, "Other").ok());
            QVERIFY(w.openProject(folder));
            QVERIFY(showAndActivate(w));
            int asked = 0;
            MainWindow::ExitAnswer answer = MainWindow::ExitAnswer::Cancel;
            w.setExitPrompt([&] {
                ++asked;
                return answer;
            });
            // Save on exit (the default): a dirty project is saved without a question.
            editOptions(w.session(), 11.0);
            QVERIFY(w.windowTitle().startsWith(QStringLiteral("Exit*")));
            QVERIFY(w.close());
            QCOMPARE(asked, 0);
            QVERIFY(!w.session().isDirty());
            QCOMPARE(probeSecOnDisk(folder), 11.0);
            QCOMPARE(w.windowTitle(), QStringLiteral("Exit - Esses"));

            // Save on exit off: asked; Cancel keeps the window and the edit.
            setGeneral(w.session(), "saveOnExit", false);
            QVERIFY(showAndActivate(w));
            editOptions(w.session(), 12.0);
            const std::string disk = readAll(folder / "project.json");
            QVERIFY(!w.close());
            QCOMPARE(asked, 1);
            QVERIFY(w.isVisible());
            QVERIFY(w.session().isDirty());
            // Opening another project asks the same; Cancel keeps this one open.
            QVERIFY(!w.openProject(other));
            QCOMPARE(asked, 2);
            QCOMPARE(w.session().projectFile().name, std::string("Exit"));
            // Discard: nothing is written and this session's journal goes too (D-187: its own autosave is
            // not offered to itself, so the file is checked on disk).
            QVERIFY(w.session().autosave().ok());
            QVERIFY(!std::filesystem::is_empty(folder / ".journal"));
            answer = MainWindow::ExitAnswer::Discard;
            QVERIFY(w.openProject(other));
            QCOMPARE(asked, 3);
            QCOMPARE(readAll(folder / "project.json"), disk);
            QCOMPARE(w.session().projectFile().name, std::string("Other"));
            // Save: written.
            QVERIFY(w.openProject(folder));
            QVERIFY(!w.session().hasRecoverableJournal());
            setGeneral(w.session(), "saveOnExit", false);
            QVERIFY(w.session().save().ok());
            editOptions(w.session(), 13.0);
            answer = MainWindow::ExitAnswer::Save;
            QVERIFY(w.close());
            QCOMPARE(asked, 4);
            QCOMPARE(probeSecOnDisk(folder), 13.0);

            // Only view state changed: written on exit without a question.
            QVERIFY(showAndActivate(w));
            project::SelectionKeys keys;
            keys.singleColourMain = true;
            w.session().setLastSelection(keys);
            QVERIFY(!w.session().isDirty());
            QVERIFY(w.close());
            QCOMPARE(asked, 4);
            QVERIFY(jsonFile(folder / "project.json")
                        .value(QStringLiteral("lastSelection"))
                        .toObject()
                        .value(QStringLiteral("singleColourMain"))
                        .toBool());
        }
        rt->shutdown();
    }

    void autoShowMessagesFollowsTheOption() {
        auto rt = runtime();
        for (const bool on : {false, true}) {
            QTemporaryDir config;
            MainWindow w(*rt, pathsFor(config.path()));
            setGeneral(w.session(), "autoShowMessagesOnError", on);
            QCOMPARE(w.docks().state(QStringLiteral("messages")), DockState::AutoHide);
            qt::Message m;
            m.severity = qt::Message::Severity::Error;
            m.text = QStringLiteral("a test error");
            w.messages().append(std::move(m));
            QCOMPARE(w.docks().state(QStringLiteral("messages")),
                     on ? DockState::Docked : DockState::AutoHide);
        }
        rt->shutdown();
    }

    void recoveryDialogListsAndAnswers() {
        project::JournalReplay replay;
        replay.entries.push_back(*project::JournalEntry::of("project.json", "{}\n", std::nullopt));
        replay.entries.push_back(
            *project::JournalEntry::of("workbooks/General.workbook.json", "{}\n", std::nullopt));
        replay.conflicts.push_back("project.json");
        replay.log.push_back("3.json skipped: not a journal");
        for (const project::JournalRecovery which :
             {project::JournalRecovery::Recover, project::JournalRecovery::Discard,
              project::JournalRecovery::Later}) {
            RecoveryDialog d(replay, QStringLiteral("Spa"));
            QCOMPARE(d.windowModality(), Qt::WindowModal);
            QCOMPARE(d.documents(), (QStringList{QStringLiteral("project.json (changed on disk since)"),
                                                 QStringLiteral("workbooks/General.workbook.json")}));
            QVERIFY(d.findChild<QLabel*>(QStringLiteral("recoveryLog")) != nullptr);
            QVERIFY(d.findChild<QLabel*>(QStringLiteral("recoveryQuestion"))
                        ->text()
                        .contains(QStringLiteral("Recover unsaved changes?")));
            d.show();
            QVERIFY(QTest::qWaitForWindowExposed(&d));
            QSignalSpy answered(&d, &RecoveryDialog::answered);
            d.button(which).click();
            QCOMPARE(answered.count(), 1);
            QCOMPARE(answered.front().front().value<project::JournalRecovery>(), which);
            QCOMPARE(d.answer(), which);
            QVERIFY(!d.isVisible());
        }
        // ESC (or the close button) answers Later, once.
        RecoveryDialog d(replay, QStringLiteral("Spa"));
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QSignalSpy answered(&d, &RecoveryDialog::answered);
        QTest::keyClick(&d, Qt::Key_Escape);
        QCOMPARE(answered.count(), 1);
        QCOMPARE(d.answer(), project::JournalRecovery::Later);
        d.close();
        QCOMPARE(answered.count(), 1);
    }
};

QTEST_MAIN(TstShellM2)
#include "tst_shell_m2.moc"
