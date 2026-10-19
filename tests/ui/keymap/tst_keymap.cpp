// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-06 (06 sections 14.4-14.5, D-033): one Qt Test row per key of every row of data/keymaps/default.json
// with "status": "implemented", run offscreen in the real shell. Each row resets a fixture (the MainWindow
// with two synthetic circuit logs opened through the SourceLoader), arranges the command's precondition, puts
// the focus where the row's context lives (global / worksheet / graph: the time_distance component of the
// active worksheet; data_window: the Data dock's tree), sends the key with QTest (through the KeyEventRouter
// and the KeymapBinder) and checks the documented post-condition. A menu-only row (no key, 06 section 14.3)
// is run through its shared QAction, as the menu or the toolbar triggers it. Every command id with an
// implemented row has a KEYMAP_TEST entry naming it below: a row without one fails here, and
// tools/check_keymap_coverage.py checks the same at lint time (08 section 6.5). M2 adds edit.undo / edit.redo
// (the session's undo stack, with a document edit arranged first), tools.options, data.lap_stretch_toggle,
// view.messages_window and view.reset_docks, then the editors' rows of the shell's M2 wiring:
// tools.maths_editor, tools.channel_status, view.channel_status, tools.channel_editor, tools.aliases,
// tools.mappings, tools.details, tools.lap_editor, data.filter_all_sources, cursor.insert_marker (SHIFT+F7)
// and the nine channel.* rows, and the workbook rows view.workbook_n (CTRL+1..CTRL+0), view.prev_workbook and
// view.next_workbook (a second workbook added first). Rows that edit a document undo back to the state of
// reset() afterwards. M3 (keymap_m3_cases.hpp, AC-M3-01): the rows of the properties/shell work; a summary
// row focuses a scatter component, a row may replace the key by a mouse gesture (zoom.extend), and the file
// rows get their answers from MainWindow::setPrompts (a project folder of the fixture, the third log, an
// export file). M4 (D-206): the packages' rows in keymap_m4_<package>_cases.hpp. plannedAndReservedRows is
// the generic row of 06 section 14.5 for the planned (M4 until implemented, v1.0 and later) and reserved
// rows: registered, disabled, the milestone or "reserved" in the tool tip.

#include "../../../libs/qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/uuid.hpp>
#include <esses/project/keymap.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/editors_data/channel_editor.hpp>
#include <esses/widgets/editors_data/details_editor.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/editors_maths/active_channel_commands.hpp>
#include <esses/widgets/editors_maths/channel_documents_dialogs.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>
// M3
#include <esses/analysis/track_map.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/components/scatter_plot.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/properties_shell/component_clipboard.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/properties_shell/shell_dialogs.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>
// M4 (the docs package's help.contents row, keymap_m4_docs_cases.hpp)
#include <esses/widgets/shell/docs_shell.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFileInfo>
#include <QIdentityProxyModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QSet>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

// Names the command a table entry tests (tools/check_keymap_coverage.py greps for it).
#define KEYMAP_TEST(id) QStringLiteral(id)

namespace {

constexpr Micros kSec = 1'000'000;
const QString kData = QStringLiteral("data");
const QString kValues = QStringLiteral("values");
const QString kStatus = QStringLiteral("channelstatus");

struct Fixture;

struct Before {
    plot::SelectionSnapshot selection;
    plot::ViewState view;
    int sheet = 0;
    bool valuesVisible = false;
    bool messagesVisible = false;
    bool stretch = false;
    int undoIndex = 0;
    std::string activeChannel;                       // the focused graph's active channel
    std::optional<project::ComponentSpec> graphSpec; // and its spec
    // M3
    std::optional<project::ComponentSpec> focusedSpec; // the canvas's focused component's spec
    bool trackMapVisible = false;
    bool inDock = false; // the row runs with the focus in the Data window
};

struct Case {
    using Step = std::function<void(Fixture&, int arg)>;
    using Verify = std::function<void(Fixture&, const Before&, int arg)>;
    using After = std::function<void(Fixture&)>;
    Case() = default;
    // The table's {arrange, focus, verify, after} form (a mouse row sets `gesture` after construction).
    Case(Step arrangeStep, Step focusStep, Verify verifyStep, After afterStep)
        : arrange(std::move(arrangeStep)), focus(std::move(focusStep)), verify(std::move(verifyStep)),
          after(std::move(afterStep)) {}

    // Before the focus is placed (worksheet switches recreate the components).
    std::function<void(Fixture&, int arg)> arrange;
    // Replaces the focus of the row's context (e.g. ESC pressed inside a dock).
    std::function<void(Fixture&, int arg)> focus;
    std::function<void(Fixture&, const Before&, int arg)> verify;
    std::function<void(Fixture&)> after;
    // M3: replaces the key (or the menu action) by a mouse gesture (the mouse rows, e.g. zoom.extend).
    std::function<void(Fixture&, int arg)> gesture;
};

struct Fixture {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt;
    std::unique_ptr<MainWindow> w;
    std::filesystem::path ldA;
    std::filesystem::path ldB;
    model::SourceId a;
    model::SourceId b;
    std::vector<model::RangeRef> lapsA; // displayed order: out lap, 50 s, 48 s, 49 s, in lap
    std::vector<model::RangeRef> lapsB;
    int undoMark = 0;   // the undo index of reset(): rows that edit documents undo back to it
    int bookBefore = 0; // the workbook rows: the active workbook before the key
    // M3: a third log (file.import_data), a project folder (file.save_project, file.open_project), the
    // answers of the prompts and the components a row adds.
    QTemporaryDir projects;
    std::filesystem::path ldC;
    std::filesystem::path projectDir;
    std::filesystem::path nextFolder;
    std::filesystem::path nextFile;
    int folders = 0;
    int componentsBefore = 0;
    std::vector<double> expect; // what a mouse row's gesture expects (read by its verify)

    qt::WorksheetViewModel& ws() const { return *w->activeWorksheet(); }
    qt::DataSelectionModel& sel() const { return w->selection(); }
    ComponentWidget* graph() const {
        for (ComponentWidget* c : w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String("time_distance")) {
                return c;
            }
        }
        return nullptr;
    }
    void focusGraph() const {
        ComponentWidget* g = graph();
        w->canvas().focusComponent(g);
        g->setFocus(Qt::OtherFocusReason);
    }
    void focusData(const model::RangeRef& highlight) const {
        w->docks().focus(kData);
        w->dataWindow().highlight(highlight);
    }
    bool open(const std::filesystem::path& path, std::size_t expectSources) {
        w->openLog(path);
        return QTest::qWaitFor([&] { return sel().sources().size() == expectSources; }, 30000);
    }
    model::SourceId sourceOf(const std::filesystem::path& path) const {
        for (const auto& s : sel().sources()) {
            if (s->prov.path.filename() == path.filename()) {
                return s->id;
            }
        }
        return {};
    }
    // ---- M3 ----
    std::filesystem::path freshFolder() {
        return std::filesystem::path(projects.path().toStdWString()) / ("new" + std::to_string(++folders));
    }
    std::filesystem::path exportFile() {
        return std::filesystem::path(logs.path().toStdWString()) /
               ("export" + std::to_string(++folders) + ".csv");
    }
    bool reopenLogs() {
        if (sel().source(a) == nullptr && !open(ldA, sel().sources().size() + 1)) {
            return false;
        }
        if (sel().source(b) == nullptr && !open(ldB, sel().sources().size() + 1)) {
            return false;
        }
        a = sourceOf(ldA);
        b = sourceOf(ldB);
        lapsA = sel().displayedRanges(a);
        lapsB = sel().displayedRanges(b);
        return lapsA.size() == 5 && lapsB.size() == 4;
    }
    bool openProject() { return w->session().isOpen() || w->openProject(projectDir); }
    bool closeProject() {
        if (w->session().isOpen() && !w->commands().invoke(QStringLiteral("file.close_project"))) {
            return false;
        }
        w->activateWindow();
        return !w->session().isOpen() && graph() != nullptr;
    }
    // A component of `type` on the shown worksheet (added as an undoable edit when there is none).
    ComponentWidget* extra(const char* type) {
        for (ComponentWidget* c : w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String(type)) {
                return c;
            }
        }
        project::ComponentSpec spec;
        spec.id = uuidRandom();
        spec.type = type;
        spec.known = true;
        spec.rect = project::Rect{0.8, 0.8, 1.0, 1.0};
        if (QLatin1String(type) == QLatin1String("outing")) {
            spec.rect = project::Rect{0.0, 0.55, 0.6, 1.0}; // room for its zoom rectangle's handles
            spec.body = jsonptr::toDoc(QJsonObject{
                {QStringLiteral("groups"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("name"), QStringLiteral("Throttle")},
                     {QStringLiteral("scaleMode"), QStringLiteral("channel")},
                     {QStringLiteral("traces"),
                      QJsonArray{QJsonObject{{QStringLiteral("channel"), QStringLiteral("Throttle")}}}}}}}});
        } else if (QLatin1String(type) == QLatin1String("track_position")) {
            spec.rect = project::Rect{0.6, 0.0, 1.0, 0.5};
        }
        if (QLatin1String(type) == QLatin1String("gauge_numeric")) {
            QJsonArray bands;
            for (int k = 0; k < 10; ++k) {
                bands.append(QJsonObject{
                    {QStringLiteral("min"),
                     QJsonObject{{QStringLiteral("v"), 10 * k}, {QStringLiteral("unit"), QString()}}},
                    {QStringLiteral("max"),
                     QJsonObject{{QStringLiteral("v"), 10 * k + 10}, {QStringLiteral("unit"), QString()}}},
                    {QStringLiteral("colour"), QStringLiteral("#20a040")},
                    {QStringLiteral("visible"), true}});
            }
            spec.body = jsonptr::toDoc(QJsonObject{{QStringLiteral("channel"), QStringLiteral("Throttle")},
                                                   {QStringLiteral("colourBands"), bands}});
        } else if (QLatin1String(type) == QLatin1String("scatter")) {
            spec.body = jsonptr::toDoc(QJsonObject{
                {QStringLiteral("xChannel"), QStringLiteral("Throttle")},
                {QStringLiteral("traces"),
                 QJsonArray{QJsonObject{{QStringLiteral("yChannel"), QStringLiteral("Wheel Speed FL")}}}}});
        }
        qt::ProjectSession& s = w->session();
        s.undoStack().push(new qt::AddComponentsCommand(s, s.activeWorkbook(), w->activeWorksheetIndex(),
                                                        {spec}, QStringLiteral("Add for a keymap row")));
        for (ComponentWidget* c : w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String(type)) {
                return c;
            }
        }
        return nullptr;
    }
    void focusExtra(const char* type) {
        ComponentWidget* c = extra(type);
        QVERIFY(c != nullptr);
        w->canvas().focusComponent(c);
        c->setFocus(Qt::OtherFocusReason);
    }
    // A track row: a track_position component showing a map of the Main source's venue (the track-1 example
    // map, cached by the window's TrackMapService), in "fit".
    void focusTrack() {
        const std::shared_ptr<const model::DataSource> src = sel().source(sel().snapshot().main.src);
        QVERIFY(src != nullptr);
        if (w->trackMaps().mapFor(*src) == nullptr) {
            Result<JsonDoc> doc = JsonDoc::load(qt::test::repoRoot() / "schemas" / "examples" / "track-1" /
                                                "valid" / "full.json");
            QVERIFY(doc.ok());
            Result<analysis::TrackMap> map = analysis::TrackMap::fromJson(doc.value());
            QVERIFY(map.ok());
            QVERIFY(w->trackMaps()
                        .setMap(qt::TrackMapService::venueOf(*src),
                                std::make_shared<const analysis::TrackMap>(std::move(map).value()))
                        .ok());
        }
        focusExtra("track_position");
        auto* tp = qobject_cast<TrackPosition*>(w->canvas().focusedComponent());
        QVERIFY(tp != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(tp->trackMap() != nullptr, 20000);
        tp->fit();
    }
};

Fixture* fx = nullptr;

std::pair<Qt::Key, Qt::KeyboardModifiers> parseKey(const QString& text) {
    QString t = text;
    if (t.endsWith(QLatin1String("Delete"))) {
        t.chop(3); // "Del": Qt's portable name
    }
    const QKeySequence seq = QKeySequence::fromString(t, QKeySequence::PortableText);
    const QKeyCombination c = seq[0];
    Qt::KeyboardModifiers mods = c.keyboardModifiers();
    if (c.key() == Qt::Key_Enter) {
        mods |= Qt::KeypadModifier; // "Enter" is the keypad key
    }
    return {c.key(), mods};
}

bool inCanvas() {
    QWidget* f = QApplication::focusWidget();
    return f != nullptr && (f == &fx->w->canvas() || fx->w->canvas().isAncestorOf(f));
}

bool near(double a, double b) {
    return std::abs(a - b) <= 1000.0; // 1 ms in time-domain units
}

// A zoom and a cursor inside the Main lap that differ from the reset state (N/P must keep both).
void zoomInsideMain(Fixture& f);

// M3: a summary component (the scatter of a summary row) has the canvas's focus.
bool isSummaryFocused(const Fixture& f) {
    const ComponentWidget* c = f.w->canvas().focusedComponent();
    return c != nullptr && c->commandContext() == qt::CommandContext::Summary;
}

// edit.undo / edit.redo: one document edit on the session's stack (options.reports.probeSec = kProbe).
constexpr double kProbe = 42.0;
void pushProbeEdit(Fixture& f);
double probeSec(const Fixture& f);
// options.reports.probeSec of a project.json on disk (0 when absent).
double probeSecOnDisk(const std::filesystem::path& folder) {
    Result<std::string> read = readWholeFile(folder / "project.json");
    const std::string text = read ? std::move(read).value() : std::string();
    return QJsonDocument::fromJson(QByteArray::fromStdString(text))
        .object()
        .value(QStringLiteral("options"))
        .toObject()
        .value(QStringLiteral("reports"))
        .toObject()
        .value(QStringLiteral("probeSec"))
        .toDouble();
}

// The M2 editors' rows: back to the reset state.
void undoToMark(Fixture& f) {
    QUndoStack& stack = f.w->session().undoStack();
    while (stack.index() > f.undoMark && stack.canUndo()) {
        stack.undo();
    }
}
// The active channel's value at the cursor is available: a logged channel always, a maths channel once its
// head is evaluated (the earlier rows re-version it: a transient filter, local maths undone).
void waitActiveChannelReady(Fixture& f) {
    const std::optional<QString> name = f.w->activeChannelCommands().activeChannel();
    QVERIFY(name.has_value());
    const std::string n = name->toStdString();
    if (!f.w->maths().isMathsChannel(f.a, "Normal", n)) {
        return;
    }
    const maths::ChannelKey key = maths::ChannelKey::make(f.a, "Normal", n);
    QTRY_VERIFY_WITH_TIMEOUT(f.w->maths().status(key).state == maths::MathState::Ok ||
                                 f.w->maths().status(key).state == maths::MathState::Warning,
                             30000);
}
void closeEditor(Fixture& f, QDialog& dialog) {
    dialog.close();
    f.w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(f.w.get()));
}
// The workbook rows: a second workbook (an undoable edit) and the active one chosen before the key.
void addSecondWorkbook(Fixture& f, int active) {
    qt::ProjectSession& s = f.w->session();
    if (s.workbooks().size() < 2) {
        project::WorkbookFile book = s.workbooks().front();
        book.id = {};
        book.name = "Second";
        s.undoStack().push(new qt::AddWorkbookCommand(s, book));
    }
    QCOMPARE(s.workbooks().size(), std::size_t{2});
    f.w->selectWorkbook(active);
    QCOMPARE(s.activeWorkbook(), active);
    f.bookBefore = active;
}
void backToFirstWorkbook(Fixture& f) {
    f.w->selectWorkbook(0);
    undoToMark(f);
    QCOMPARE(f.w->session().workbooks().size(), std::size_t{1});
    QCOMPARE(f.w->activeWorksheetIndex(), 0);
}

#include "keymap_m3_cases.hpp" // inside this anonymous namespace: addM3Cases(table)
// M4 (D-206, docs/milestones/M4.md section 3): one file per package that implements a keymap row.
#include "keymap_m4_docs_cases.hpp"   // addM4DocsCases(table): help.contents (wave 2)
#include "keymap_m4_reload_cases.hpp" // addM4ReloadCases(table): file.reload_source

const std::map<QString, Case>& cases() {
    static const std::map<QString, Case> table = [] {
        using F = Fixture;
        std::map<QString, Case> m3;
        addM3Cases(m3);
        std::map<QString, Case> m4;
        addM4ReloadCases(m4);
        addM4DocsCases(m4);
        std::map<QString, Case> rows{
            // ---- views and windows ----
            {KEYMAP_TEST("view.data_window"),
             {{}, {}, [](F& f, const Before&, int) { QCOMPARE(f.w->docks().focusedDockId(), kData); }, {}}},
            {KEYMAP_TEST("view.channels_window"),
             {{},
              {},
              [](F& f, const Before&, int) {
                  QCOMPARE(f.w->docks().focusedDockId(), QStringLiteral("channels"));
                  QVERIFY(QApplication::focusWidget() == &f.w->channelsWindow().search());
              },
              {}}},
            {KEYMAP_TEST("view.values_window"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->docks().isVisible(kValues), !b.valuesVisible);
                  QVERIFY(inCanvas()); // focus unchanged
              },
              {}}},
            {KEYMAP_TEST("view.return_focus"),
             {{},
              [](F& f, int) { f.focusData(f.lapsA[1]); },
              [](F&, const Before&, int) { QVERIFY(inCanvas()); },
              {}}},
            {KEYMAP_TEST("view.worksheet_n"),
             {[](F& f, int n) { f.w->selectWorksheet(n == 1 ? 1 : 0); },
              {},
              [](F& f, const Before& b, int n) {
                  QCOMPARE(f.w->activeWorksheetIndex(), n <= f.w->worksheetCount() ? n - 1 : b.sheet);
              },
              {}}},
            {KEYMAP_TEST("view.prev_worksheet"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  const int n = f.w->worksheetCount();
                  QCOMPARE(f.w->activeWorksheetIndex(), (b.sheet + n - 1) % n);
              },
              {}}},
            {KEYMAP_TEST("view.next_worksheet"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->activeWorksheetIndex(), (b.sheet + 1) % f.w->worksheetCount());
              },
              {}}},
            {KEYMAP_TEST("view.workbook_n"),
             {[](F& f, int n) { addSecondWorkbook(f, n == 1 ? 1 : 0); },
              {},
              [](F& f, const Before&, int n) {
                  const int count = static_cast<int>(f.w->session().workbooks().size());
                  QCOMPARE(f.w->session().activeWorkbook(), n <= count ? n - 1 : f.bookBefore);
                  QCOMPARE(f.w->workbookSelector().currentIndex(), f.w->session().activeWorkbook());
              },
              [](F& f) { backToFirstWorkbook(f); }}},
            {KEYMAP_TEST("view.prev_workbook"),
             {[](F& f, int) { addSecondWorkbook(f, 0); },
              {},
              [](F& f, const Before&, int) { QCOMPARE(f.w->session().activeWorkbook(), 1); }, // wraps
              [](F& f) { backToFirstWorkbook(f); }}},
            {KEYMAP_TEST("view.next_workbook"),
             {[](F& f, int) { addSecondWorkbook(f, 1); },
              {},
              [](F& f, const Before&, int) { QCOMPARE(f.w->session().activeWorkbook(), 0); }, // wraps
              [](F& f) { backToFirstWorkbook(f); }}},
            {KEYMAP_TEST("view.messages_window"), // menu only
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->docks().isVisible(QStringLiteral("messages")), !b.messagesVisible);
              },
              {}}},
            {KEYMAP_TEST("view.reset_docks"), // menu only
             {[](F& f, int) {
                  f.w->docks().setState(kValues, DockState::Closed);
                  f.w->docks().setState(kData, DockState::Floating);
              },
              {},
              [](F& f, const Before&, int) {
                  QCOMPARE(f.w->docks().state(kValues), f.w->docks().defaultState(kValues));
                  QCOMPARE(f.w->docks().state(kData), f.w->docks().defaultState(kData));
              },
              [](F& f) {
                  f.w->activateWindow();
                  QVERIFY(QTest::qWaitForWindowActive(f.w.get()));
              }}},
            // ---- edit (M2: the session's undo stack, D-032) ----
            {KEYMAP_TEST("edit.undo"),
             {[](F& f, int) { pushProbeEdit(f); },
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex - 1);
                  QVERIFY(probeSec(f) != kProbe);
                  QVERIFY(f.w->session().undoStack().canRedo());
              },
              [](F& f) { f.w->session().undoStack().clear(); }}},
            {KEYMAP_TEST("edit.redo"),
             {[](F& f, int) {
                  pushProbeEdit(f);
                  f.w->session().undoStack().undo();
              },
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
                  QCOMPARE(probeSec(f), kProbe);
              },
              [](F& f) {
                  f.w->session().undoStack().undo(); // back to the original options
                  f.w->session().undoStack().clear();
              }}},
            // ---- tools ----
            {KEYMAP_TEST("tools.options"), // menu only
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->optionsDialog().isVisible());
                  QCOMPARE(QApplication::activeModalWidget(), &f.w->optionsDialog());
              },
              [](F& f) {
                  f.w->optionsDialog().reject();
                  f.w->activateWindow();
                  QVERIFY(QTest::qWaitForWindowActive(f.w.get()));
              }}},
            // ---- M2 editors (the shell's wiring, 06 sections 10, 13 and 14.4) ----
            {KEYMAP_TEST("tools.maths_editor"),
             {{},
              {},
              [](F& f, const Before&, int) { QVERIFY(f.w->mathsEditor().isVisible()); },
              [](F& f) { closeEditor(f, f.w->mathsEditor()); }}},
            {KEYMAP_TEST("tools.channel_status"),
             {{},
              {},
              [](F& f, const Before&, int) {
                  QCOMPARE(f.w->docks().focusedDockId(), kStatus);
                  // The built-in outputs of both open logs at least (corrected speed and distance).
                  QVERIFY(f.w->channelStatusWindow().table().model()->rowCount() >= 4);
              },
              {}}},
            {KEYMAP_TEST("view.channel_status"), // menu only: an alias of tools.channel_status
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->docks().isVisible(kStatus));
                  QCOMPARE(f.w->docks().focusedDockId(), kStatus);
              },
              {}}},
            {KEYMAP_TEST("tools.channel_editor"),
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->channelEditor().isVisible());
                  QVERIFY(f.w->channelEditor().channelModel().rowCount() > 0); // the channels of both logs
              },
              [](F& f) { closeEditor(f, f.w->channelEditor()); }}},
            {KEYMAP_TEST("tools.aliases"), // menu only
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->channelEditor().aliasesDialog() != nullptr);
                  QVERIFY(f.w->channelEditor().aliasesDialog()->isVisible());
              },
              [](F& f) { closeEditor(f, *f.w->channelEditor().aliasesDialog()); }}},
            {KEYMAP_TEST("tools.mappings"), // menu only
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->channelEditor().mappingsDialog() != nullptr);
                  QVERIFY(f.w->channelEditor().mappingsDialog()->isVisible());
              },
              [](F& f) { closeEditor(f, *f.w->channelEditor().mappingsDialog()); }}},
            {KEYMAP_TEST("tools.details"),
             {{},
              {},
              [](F& f, const Before&, int) { QVERIFY(f.w->detailsEditor().isVisible()); },
              [](F& f) { closeEditor(f, f.w->detailsEditor()); }}},
            {KEYMAP_TEST("tools.lap_editor"), // menu only
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.w->lapEditor().isVisible());
                  QVERIFY(f.w->lapEditor().source() == f.a);        // follows the Main source
                  QVERIFY(f.w->lapEditor().rowOf(f.lapsA[2]) >= 0); // its laps are listed
              },
              [](F& f) { closeEditor(f, f.w->lapEditor()); }}},
            {KEYMAP_TEST("data.filter_all_sources"), // menu only
             {[](F& f, int) {
                  qt::FilterSpec fastest;
                  fastest.kind = qt::FilterSpec::Kind::Fastest;
                  f.sel().setFilter(f.a, fastest);
              },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.sel().filter(f.b).kind == qt::FilterSpec::Kind::Fastest);
              },
              [](F& f) {
                  f.sel().setFilter(f.a, qt::FilterSpec{});
                  f.sel().setFilter(f.b, qt::FilterSpec{});
              }}},
            // SHIFT+F7: a user beacon at the main cursor; the laps rebuild at once (AC-M2-05); one undo step.
            {KEYMAP_TEST("cursor.insert_marker"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
                  QCOMPARE(f.sel().displayedRanges(f.a).size(), f.lapsA.size() + 1);
              },
              [](F& f) {
                  undoToMark(f);
                  QCOMPARE(f.sel().displayedRanges(f.a).size(), f.lapsA.size());
                  f.lapsA = f.sel().displayedRanges(f.a);
              }}},
            // ---- active channel (ActiveChannelCommands over the focused graph) ----
            {KEYMAP_TEST("channel.next_active"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(!f.graph()->spec().activeChannel.empty());
                  QVERIFY(f.graph()->spec().activeChannel != b.activeChannel);
              },
              [](F& f) { undoToMark(f); }}},
            {KEYMAP_TEST("channel.prev_active"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(!f.graph()->spec().activeChannel.empty());
                  QVERIFY(f.graph()->spec().activeChannel != b.activeChannel);
              },
              [](F& f) { undoToMark(f); }}},
            {KEYMAP_TEST("channel.hide"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1); // a workbook edit
                  QVERIFY(b.graphSpec.has_value() && !(f.graph()->spec() == *b.graphSpec));
              },
              [](F& f) { undoToMark(f); }}},
            {KEYMAP_TEST("channel.remove"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
                  QVERIFY(b.graphSpec.has_value() && !(f.graph()->spec() == *b.graphSpec));
              },
              [](F& f) { undoToMark(f); }}},
            // A logged channel has no maths item: a status-bar hint, nothing opens (06 section 14.4).
            {KEYMAP_TEST("channel.edit_maths"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(!f.w->activeChannelCommands().lastHint().isEmpty());
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex);
              },
              {}}},
            {KEYMAP_TEST("channel.status"),
             {{},
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(!f.w->activeChannelCommands().lastHint().isEmpty() ||
                          f.w->docks().focusedDockId() == kStatus);
              },
              {}}},
            // CTRL+SHIFT+F: a transient filter of the Main source, never a document edit.
            {KEYMAP_TEST("channel.filter"),
             {[](F& f, int) {
                  f.w->activeChannelCommands().setFilterPrompt(
                      [](const QString&) -> std::optional<ActiveChannelCommands::FilterChoice> {
                          ActiveChannelCommands::FilterChoice c;
                          c.points = 5;
                          return c;
                      });
              },
              {},
              [](F& f, const Before& b, int) {
                  const std::optional<maths::MathModule> t =
                      f.w->activeChannelCommands().transientModule(f.a);
                  QVERIFY(t.has_value());
                  QCOMPARE(t->items.size(), std::size_t{1});
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex);
              },
              {}}},
            // CTRL+SHIFT+O / CTRL+SHIFT+Z: a scale_offset item in the Main source's local maths, one undo
            // step.
            {KEYMAP_TEST("channel.scale_offset"),
             {[](F& f, int) {
                  f.w->activeChannelCommands().setScaleOffsetPrompt(
                      [](const QString&,
                         const QString&) -> std::optional<ActiveChannelCommands::ScaleOffsetChoice> {
                          ActiveChannelCommands::ScaleOffsetChoice c;
                          c.scale = 2.0;
                          return c;
                      });
              },
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
              },
              [](F& f) { undoToMark(f); }}},
            {KEYMAP_TEST("channel.zero_at_cursor"),
             {[](F& f, int) { waitActiveChannelReady(f); },
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY2(f.w->session().undoStack().index() == b.undoIndex + 1,
                           qPrintable(f.w->activeChannelCommands().lastHint()));
              },
              [](F& f) { undoToMark(f); }}},
            // ---- data ----
            {KEYMAP_TEST("data.lap_stretch_toggle"), // menu only (and the toolbar)
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.w->lapStretch().enabled(), !b.stretch);
                  QCOMPARE(f.w->session().lapStretchSettings().enabled, !b.stretch);
                  QCOMPARE(f.w->session().undoStack().index(), b.undoIndex); // view state (D-174)
              },
              [](F& f) { f.w->lapStretch().toggle(); }}},
            {KEYMAP_TEST("data.toggle_domain"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.ws().domain(),
                           b.view.domain == plot::Domain::Time ? plot::Domain::Distance : plot::Domain::Time);
              },
              {}}},
            {KEYMAP_TEST("data.overlays_toggle"),
             {[](F& f, int) { f.sel().toggleOverlay(f.lapsA[3]); },
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.sel().snapshot().overlaysHidden, !b.selection.overlaysHidden);
                  QCOMPARE(f.sel().snapshot().overlays.size(), std::size_t{1}); // the set is kept
              },
              {}}},
            {KEYMAP_TEST("data.overlays_clear"),
             {[](F& f, int) {
                  f.sel().toggleOverlay(f.lapsA[3]);
                  f.sel().toggleOverlay0(f.lapsA[1]);
                  f.sel().toggleOverlaysHidden();
              },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.sel().snapshot().overlays.empty());
                  QVERIFY(!f.sel().snapshot().overlay0.has_value());
                  QVERIFY(!f.sel().snapshot().overlaysHidden);
              },
              {}}},
            {KEYMAP_TEST("data.single_colour_main"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QCOMPARE(f.sel().snapshot().singleColourMain, !b.selection.singleColourMain);
              },
              {}}},
            // N / P: the next / previous Main range with the zoom and the cursor kept (06 section 5.2), and
            // no
            // wrap at the ends of the order (the after step presses the key again at the last / first lap).
            {KEYMAP_TEST("data.next_range"),
             {[](F& f, int) { zoomInsideMain(f); },
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(f.sel().snapshot().main == f.lapsA[3]);
                  QCOMPARE(f.ws().state().zoom, b.view.zoom);
                  QCOMPARE(f.ws().state().cursorT, b.view.cursorT);
              },
              [](F& f) {
                  f.sel().setMain(f.lapsA.back());
                  QTest::keyClick(QApplication::focusWidget(), Qt::Key_N);
                  QVERIFY(f.sel().snapshot().main == f.lapsA.back());
              }}},
            {KEYMAP_TEST("data.prev_range"),
             {[](F& f, int) { zoomInsideMain(f); },
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(f.sel().snapshot().main == f.lapsA[1]);
                  QCOMPARE(f.ws().state().zoom, b.view.zoom);
                  QCOMPARE(f.ws().state().cursorT, b.view.cursorT);
              },
              [](F& f) {
                  f.sel().setMain(f.lapsA.front());
                  QTest::keyClick(QApplication::focusWidget(), Qt::Key_P);
                  QVERIFY(f.sel().snapshot().main == f.lapsA.front());
              }}},
            {KEYMAP_TEST("data.select_fastest"),
             {[](F& f, int) { f.sel().setMain(f.lapsA[1]); },
              {},
              [](F& f, const Before&, int) { QVERIFY(f.sel().snapshot().main == f.lapsA[2]); }, // 48 s
              {}}},
            {KEYMAP_TEST("data.select_all_fast"),
             {[](F& f, int) {
                  f.sel().setMain(f.lapsA[1]);
                  f.sel().setFastLapTolerancePct(2.5); // 49 s is within 2.5 % of 48 s, 50 s is not
              },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.sel().snapshot().main == f.lapsA[2]);
                  QCOMPARE(f.sel().snapshot().overlays, std::vector<model::RangeRef>{f.lapsA[3]});
              },
              [](F& f) { f.sel().setFastLapTolerancePct(1.0); }}},
            {KEYMAP_TEST("data.swap_main_overlay0"),
             {[](F& f, int) { f.sel().toggleOverlay0(f.lapsA[1]); },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(f.sel().snapshot().main == f.lapsA[1]);
                  QVERIFY(f.sel().snapshot().overlay0 && *f.sel().snapshot().overlay0 == f.lapsA[2]);
              },
              {}}},
            {KEYMAP_TEST("data.select_main"),
             {{},
              [](F& f, int) { f.focusData(f.lapsA[3]); },
              [](F& f, const Before&, int) { QVERIFY(f.sel().snapshot().main == f.lapsA[3]); },
              {}}},
            {KEYMAP_TEST("data.toggle_overlay0"),
             {{},
              [](F& f, int) { f.focusData(f.lapsB[1]); },
              [](F& f, const Before&, int) {
                  QVERIFY(f.sel().snapshot().overlay0 && *f.sel().snapshot().overlay0 == f.lapsB[1]);
              },
              {}}},
            {KEYMAP_TEST("data.toggle_overlay"),
             {{},
              [](F& f, int) { f.focusData(f.lapsA[3]); },
              [](F& f, const Before&, int) { QCOMPARE(f.sel().overlayNumber(f.lapsA[3]), 1); },
              {}}},
            {KEYMAP_TEST("data.close_file"),
             {{},
              [](F& f, int) {
                  f.focusData(f.lapsB[1]);
                  QTreeView& tree = f.w->dataWindow().tree();
                  tree.setCurrentIndex(tree.currentIndex().parent()); // the source row of b
              },
              [](F& f, const Before&, int) {
                  QCOMPARE(f.sel().sources().size(), std::size_t{1});
                  QVERIFY(f.sel().source(f.b) == nullptr);
              },
              [](F& f) { // reopen b for the following rows
                  QVERIFY(f.open(f.ldB, 2));
                  f.b = f.sourceOf(f.ldB);
                  f.lapsB = f.sel().displayedRanges(f.b);
              }}},
            // ---- zoom ----
            {KEYMAP_TEST("zoom.in"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.length(), b.view.zoom.length() / 2.0));
              },
              {}}},
            {KEYMAP_TEST("zoom.out"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.length(), b.view.zoom.length() * 2.0));
              },
              {}}},
            {KEYMAP_TEST("zoom.full"),
             {{},
              {},
              [](F& f, const Before&, int) {
                  // The whole outing relative to Main's start: lap 2 of a starts 90 s into its 217 s outing.
                  QVERIFY(near(f.ws().state().zoom.a, -90.0 * kSec));
                  QVERIFY(near(f.ws().state().zoom.b, 127.0 * kSec));
              },
              {}}},
            {KEYMAP_TEST("zoom.default_range"),
             {[](F& f, int) { f.ws().zoomFull(); },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(near(f.ws().state().zoom.a, 0.0));
                  QVERIFY(near(f.ws().state().zoom.b, 48.0 * kSec));
              },
              {}}},
            {KEYMAP_TEST("zoom.datum_main"),
             {[](F& f, int) {
                  f.ws().setCursor(5 * kSec, qt::CursorSource::Programmatic);
                  f.ws().placeDatum();
                  f.ws().setCursor(20 * kSec, qt::CursorSource::Programmatic);
              },
              {},
              [](F& f, const Before&, int) {
                  QVERIFY(near(f.ws().state().zoom.a, 5.0 * kSec));
                  QVERIFY(near(f.ws().state().zoom.b, 20.0 * kSec));
              },
              {}}},
            // ---- pan ----
            {KEYMAP_TEST("pan.left"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.a, b.view.zoom.a - 0.1 * b.view.zoom.length()));
              },
              {}}},
            {KEYMAP_TEST("pan.right"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.a, b.view.zoom.a + 0.1 * b.view.zoom.length()));
              },
              {}}},
            {KEYMAP_TEST("pan.back"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.a, b.view.zoom.a - 0.9 * b.view.zoom.length()));
              },
              {}}},
            {KEYMAP_TEST("pan.forward"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(f.ws().state().zoom.a, b.view.zoom.a + 0.9 * b.view.zoom.length()));
              },
              {}}},
            {KEYMAP_TEST("pan.centre_cursor"),
             {[](F& f, int) { f.ws().setCursor(30 * kSec, qt::CursorSource::Programmatic); },
              {},
              [](F& f, const Before&, int) {
                  const plot::XSpan z = f.ws().state().zoom;
                  QVERIFY(near((z.a + z.b) / 2.0, 30.0 * kSec));
              },
              {}}},
            // ---- cursor ----
            {KEYMAP_TEST("cursor.left"),
             {{},
              {},
              [](F& f, const Before& b, int) { QVERIFY(f.ws().state().cursorT < b.view.cursorT); },
              {}}},
            {KEYMAP_TEST("cursor.right"),
             {{},
              {},
              [](F& f, const Before& b, int) { QVERIFY(f.ws().state().cursorT > b.view.cursorT); },
              {}}},
            {KEYMAP_TEST("cursor.left_fast"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  if (isSummaryFocused(f)) { // the scatter steps by Main points (06 section 14.4)
                      QVERIFY(f.ws().state().cursorT < b.view.cursorT);
                      return;
                  }
                  const Micros fast = b.view.cursorT - f.ws().state().cursorT;
                  f.ws().stepCursor(1); // back by one plain step: the fast step is ten of them
                  const Micros one = f.ws().state().cursorT - (b.view.cursorT - fast);
                  QVERIFY(one > 0);
                  QCOMPARE(fast, 10 * one);
              },
              {}}},
            {KEYMAP_TEST("cursor.right_fast"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  if (isSummaryFocused(f)) {
                      QVERIFY(f.ws().state().cursorT > b.view.cursorT);
                      return;
                  }
                  const Micros fast = f.ws().state().cursorT - b.view.cursorT;
                  f.ws().stepCursor(-1);
                  const Micros one = (b.view.cursorT + fast) - f.ws().state().cursorT;
                  QVERIFY(one > 0);
                  QCOMPARE(fast, 10 * one);
              },
              {}}},
            {KEYMAP_TEST("cursor.home"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(near(static_cast<double>(f.ws().state().cursorT), b.view.zoom.a));
              },
              {}}},
            {KEYMAP_TEST("cursor.end"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  const auto t = static_cast<double>(f.ws().state().cursorT);
                  QVERIFY(t > static_cast<double>(b.view.cursorT));
                  QVERIFY(t < b.view.zoom.b &&
                          t > b.view.zoom.b - 0.1 * kSec); // within one sample of the edge
              },
              {}}},
            // ---- datum ----
            {KEYMAP_TEST("datum.toggle"),
             {[](F& f, int) { f.ws().placeDatum(); },
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(b.view.datumT.has_value());
                  QVERIFY(!f.ws().state().datumT.has_value()); // hidden, value kept
                  QVERIFY(f.w->workspace().storedDatumT().has_value());
              },
              {}}},
            {KEYMAP_TEST("datum.place"),
             {{},
              {},
              [](F& f, const Before& b, int) {
                  QVERIFY(f.ws().state().datumT.has_value());
                  QCOMPARE(*f.ws().state().datumT, b.view.cursorT);
              },
              {}}},
            {KEYMAP_TEST("datum.swap"),
             {[](F& f, int) {
                  f.ws().setCursor(20 * kSec, qt::CursorSource::Programmatic);
                  f.ws().placeDatum();
                  f.ws().setCursor(10 * kSec, qt::CursorSource::Programmatic);
              },
              {},
              [](F& f, const Before&, int) {
                  QCOMPARE(f.ws().state().cursorT, 20 * kSec);
                  QVERIFY(f.ws().state().datumT && *f.ws().state().datumT == 10 * kSec);
              },
              {}}},
        };
        rows.merge(m3);
        rows.merge(m4);
        return rows;
    }();
    return table;
}

void zoomInsideMain(Fixture& f) {
    f.ws().zoomTo(plot::XSpan{5.0 * kSec, 20.0 * kSec});
    f.ws().setCursor(7 * kSec, qt::CursorSource::Programmatic);
}

double probeSec(const Fixture& f) {
    const QJsonObject o =
        QJsonDocument::fromJson(QByteArray::fromStdString(f.w->session().projectFile().options.compact()))
            .object();
    return o.value(QStringLiteral("reports")).toObject().value(QStringLiteral("probeSec")).toDouble();
}

void pushProbeEdit(Fixture& f) {
    qt::ProjectSession& s = f.w->session();
    project::ProjectFile next = s.projectFile();
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
    QJsonObject reports = o.value(QStringLiteral("reports")).toObject();
    reports.insert(QStringLiteral("probeSec"), kProbe);
    o.insert(QStringLiteral("reports"), reports);
    next.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
    s.undoStack().push(new qt::SetProjectOptionsCommand(s, std::move(next), QStringLiteral("Probe")));
}

// The common state every row starts from: worksheet 1 in time, Main = lap 2 of a (48 s, 90 s into the
// outing), no overlays, zoom = the Main range, cursor at 10 s, no datum, the docks at their defaults.
void reset(Fixture& f) {
    if (f.w->activeWorksheetIndex() != 0) {
        f.w->selectWorksheet(0);
    }
    f.sel().clearOverlays();
    f.sel().setSingleColourMain(false);
    f.sel().setMain(f.lapsA[2]);
    if (f.ws().domain() != plot::Domain::Time) {
        f.ws().setDomain(plot::Domain::Time);
    }
    f.ws().setSize(1000);
    f.ws().zoomToMainRange();
    f.ws().setCursor(10 * kSec, qt::CursorSource::Programmatic);
    f.w->workspace().setDatum(std::nullopt);
    for (const QString& id : f.w->docks().dockIds()) {
        if (f.w->docks().state(id) != f.w->docks().defaultState(id)) {
            f.w->docks().setState(id, f.w->docks().defaultState(id));
        }
    }
    f.undoMark = f.w->session().undoStack().index();
}

} // namespace

class TstKeymap : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        registerBuiltinComponents();
        fx = new Fixture;
        Fixture& f = *fx;
        QVERIFY(f.config.isValid() && f.logs.isValid());
        qt::AppRuntimeOptions o;
        o.workers = 4;
        o.cacheBudget = std::size_t{256} << 20;
        o.markGuiThreadNoBlock = true; // as esses: every key below runs with the AC-M1-12 check armed
        f.rt = std::make_unique<qt::AppRuntime>(o);
        PathOverrides po;
        po.configDir = std::filesystem::path(f.config.path().toStdWString());
        po.dataDir = qt::test::repoRoot() / "data";
        auto paths = Paths::resolve(po);
        QVERIFY(paths.ok());
        qt::test::SyntheticOptions oa;
        oa.path = "keymap-a.ld";
        oa.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
        qt::test::SyntheticOptions ob;
        ob.path = "keymap-b.ld";
        ob.lapSeconds = {30.0, 45.0, 44.0, 20.0};
        const std::filesystem::path dir(f.logs.path().toStdWString());
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the logs reads every chunk
            f.ldA = qt::test::writeLd(*qt::test::makeSource(oa, f.rt->cache()), dir, "keymap-a");
            f.ldB = qt::test::writeLd(*qt::test::makeSource(ob, f.rt->cache()), dir, "keymap-b");
        }

        f.w = std::make_unique<MainWindow>(*f.rt, *paths);
        f.w->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.w.get()));
        f.w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(f.w.get()));
        QVERIFY(f.open(f.ldA, 1));
        QVERIFY(f.open(f.ldB, 2));
        f.a = f.sourceOf(f.ldA);
        f.b = f.sourceOf(f.ldB);
        f.lapsA = f.sel().displayedRanges(f.a);
        f.lapsB = f.sel().displayedRanges(f.b);
        QCOMPARE(f.lapsA.size(), std::size_t{5});
        QCOMPARE(f.lapsB.size(), std::size_t{4});
        // Corr Dist and the distance indexes (F9 converts through them).
        QTRY_COMPARE_WITH_TIMEOUT(f.w->distance().state(f.a), qt::DistanceState::Ready, 30000);
        QVERIFY(f.graph() != nullptr);

        // M3: a third log for file.import_data, a project for the project rows, the prompts' answers.
        qt::test::SyntheticOptions oc;
        oc.path = "keymap-c.ld";
        oc.lapSeconds = {30.0, 40.0, 20.0};
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            f.ldC = qt::test::writeLd(*qt::test::makeSource(oc, f.rt->cache()), dir, "keymap-c");
        }
        QVERIFY(f.projects.isValid());
        f.projectDir = std::filesystem::path(f.projects.path().toStdWString()) / "keymap-project";
        {
            qt::ProjectSession maker(*paths, nullptr);
            QVERIFY(maker.loadDefaults().ok());
            QVERIFY(maker.newProject(f.projectDir, "Keymap project").ok());
            maker.closeProject();
        }
        MainWindow::Prompts prompts;
        prompts.folder = [](const QString&) { return QString::fromStdWString(fx->nextFolder.wstring()); };
        prompts.file = [](const QString&, const QString&, bool) {
            return QString::fromStdWString(fx->nextFile.wstring());
        };
        prompts.text = [](const QString& title, const QString&, const QString&) {
            return std::optional<QString>(title.contains(QStringLiteral("Annotation"))
                                              ? QStringLiteral("Keymap note")
                                              : QStringLiteral("Keymap"));
        };
        prompts.choose = [](const QString&, const QStringList&) { return std::optional<int>(0); };
        f.w->setPrompts(prompts);
    }

    void cleanupTestCase() {
        if (fx != nullptr) {
            fx->w.reset();
            fx->rt->shutdown();
            delete fx;
            fx = nullptr;
        }
    }

    void implementedRows_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("context");
        QTest::addColumn<QString>("key");
        QTest::addColumn<int>("arg");
        QTest::addColumn<bool>("fromDock");
        QTest::addColumn<bool>("viaPalette");
        // AC-M3-12 (08 section 6.5: the palette reuses this harness by command id): every command of an
        // implemented row also runs from the command palette, opened in the row's context, against the same
        // verify step as its key. Not the mouse rows (the palette has no pointer), not the palette's own row.
        QSet<QString> paletteDone;
        for (const project::KeyBinding& row : fx->w->session().keymap().bindings) {
            const QString command = QString::fromStdString(row.command);
            const auto it = cases().find(command);
            if (row.status != project::KeymapStatus::Implemented || paletteDone.contains(command) ||
                it == cases().end() || it->second.gesture ||
                command == QLatin1String("view.command_palette")) {
                continue;
            }
            int arg = 0;
            if (row.args.isArray()) {
                const QJsonArray args =
                    QJsonDocument::fromJson(QByteArray::fromStdString(row.args.compact())).array();
                arg = args.isEmpty() ? 0 : args.first().toInt();
            }
            if (arg != 0) {
                continue; // the palette runs a command without arguments: the row of argument 0
            }
            paletteDone.insert(command);
            const QString name =
                command + QStringLiteral(" (palette) ") + QString::fromStdString(row.context);
            QTest::newRow(name.toUtf8().constData())
                << command << QString::fromStdString(row.context) << QString() << 0 << false << true;
        }
        for (const project::KeyBinding& row : fx->w->session().keymap().bindings) {
            if (row.status != project::KeymapStatus::Implemented) {
                continue;
            }
            int arg = 0;
            if (row.args.isArray()) {
                const QJsonArray args =
                    QJsonDocument::fromJson(QByteArray::fromStdString(row.args.compact())).array();
                arg = args.isEmpty() ? 0 : args.first().toInt();
            }
            if (row.keys.empty()) { // a menu-only row: its QAction, as the menu or the toolbar triggers it
                const QString command = QString::fromStdString(row.command);
                const QString name =
                    command + QStringLiteral(" (menu) ") + QString::fromStdString(row.context);
                QTest::newRow(name.toUtf8().constData())
                    << command << QString::fromStdString(row.context) << QString() << arg << false << false;
            }
            for (const std::string& key : row.keys) {
                const QString command = QString::fromStdString(row.command);
                const QString name = command + QLatin1Char(' ') + QString::fromStdString(key) +
                                     QLatin1Char(' ') + QString::fromStdString(row.context);
                QTest::newRow(name.toUtf8().constData())
                    << command << QString::fromStdString(row.context) << QString::fromStdString(key) << arg
                    << false << false;
                // A global row works from any dock (06 section 14.2): the same key with the focus in the Data
                // window's tree, unless the row places its own focus.
                const auto it = cases().find(command);
                if (row.context == "global" && it != cases().end() && !it->second.focus) {
                    const QString dockName = name + QStringLiteral(" (from the Data window)");
                    QTest::newRow(dockName.toUtf8().constData())
                        << command << QString::fromStdString(row.context) << QString::fromStdString(key)
                        << arg << true << false;
                }
            }
        }
    }

    void implementedRows() {
        QFETCH(QString, command);
        QFETCH(QString, context);
        QFETCH(QString, key);
        QFETCH(int, arg);
        QFETCH(bool, fromDock);
        QFETCH(bool, viaPalette);
        const auto it = cases().find(command);
        QVERIFY2(it != cases().end(), qPrintable(QStringLiteral("no KEYMAP_TEST for ") + command));
        const Case& c = it->second;
        Fixture& f = *fx;
        reset(f);
        if (c.arrange) {
            c.arrange(f, arg);
        }
        if (c.focus) {
            c.focus(f, arg);
        } else if (context == QLatin1String("data_window") || fromDock) {
            f.focusData(f.lapsA[1]);
        } else if (context == QLatin1String("summary")) {
            f.focusExtra("scatter"); // M3: a summary row needs a summary component
        } else if (context == QLatin1String("track")) {
            f.focusTrack(); // M3: a track row needs a track_position component with a map
        } else {
            f.focusGraph(); // global, worksheet and graph rows: the time_distance component has focus
        }
        if (QTest::currentTestFailed()) {
            return; // the focus helper failed
        }
        QVERIFY(QApplication::focusWidget() != nullptr);
        Before before;
        before.selection = f.sel().snapshot();
        before.view = f.ws().state();
        before.sheet = f.w->activeWorksheetIndex();
        before.valuesVisible = f.w->docks().isVisible(kValues);
        before.messagesVisible = f.w->docks().isVisible(QStringLiteral("messages"));
        before.stretch = f.w->lapStretch().enabled();
        before.undoIndex = f.w->session().undoStack().index();
        if (const ComponentWidget* g = f.graph()) {
            before.activeChannel = g->spec().activeChannel;
            before.graphSpec = g->spec();
        }
        if (const ComponentWidget* focused = f.w->canvas().focusedComponent()) {
            before.focusedSpec = focused->spec();
        }
        before.trackMapVisible = f.w->docks().isVisible(QStringLiteral("trackmap"));
        before.inDock = context == QLatin1String("data_window") || fromDock;

        if (viaPalette) {
            // AC-M3-12 with the user's key events: CTRL+SHIFT+P at the row's focus, the command id typed into
            // the query, DOWN to the command's first row, ENTER.
            CommandPalette& palette = f.w->commandPalette();
            auto* query = palette.findChild<QLineEdit*>(QStringLiteral("paletteQuery"));
            QVERIFY(query != nullptr);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(palette.isVisible());
            QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(query));
            QTest::keyClicks(query, command);
            QCOMPARE(palette.query(), command);
            const qsizetype at = palette.visibleIds().indexOf(command);
            QVERIFY2(at >= 0, qPrintable(command + QStringLiteral(" is not listed by the palette")));
            for (qsizetype down = 0; down < at; ++down) {
                QTest::keyClick(query, Qt::Key_Down);
            }
            QCOMPARE(palette.currentRow(), static_cast<int>(at));
            QSignalSpy ran(&palette, &CommandPalette::invoked);
            QTest::keyClick(query, Qt::Key_Return);
            QVERIFY2(ran.count() == 1 && ran.at(0).at(0).toString() == command,
                     qPrintable(command + QStringLiteral(" did not run from the palette")));
        } else if (c.gesture) {
            c.gesture(f, arg); // a mouse row
        } else if (key.isEmpty()) {
            QAction* action = f.w->actions().action(command);
            QVERIFY(action != nullptr && action->isEnabled());
            action->trigger();
        } else {
            const auto [k, mods] = parseKey(key);
            QTest::keyClick(QApplication::focusWidget(), k, mods);
        }
        c.verify(f, before, arg);
        if (c.after) {
            c.after(f);
        }
    }

    // The generic row of 06 section 14.5 (AC-M3-01): every planned row (after M3) and every reserved row
    // whose command has no implemented row is registered, disabled, greyed with its milestone ("planned for
    // v1.0") or "reserved" in the tool tip, and invoking it does nothing.
    void plannedAndReservedRows() {
        Fixture& f = *fx;
        reset(f);
        QSet<QString> implemented;
        for (const project::KeyBinding& row : f.w->session().keymap().bindings) {
            if (row.status == project::KeymapStatus::Implemented) {
                implemented.insert(QString::fromStdString(row.command));
            }
        }
        int checked = 0;
        for (const project::KeyBinding& row : f.w->session().keymap().bindings) {
            const QString id = QString::fromStdString(row.command);
            if (row.status == project::KeymapStatus::Implemented || implemented.contains(id)) {
                continue;
            }
            QVERIFY2(row.status == project::KeymapStatus::Reserved ||
                         (row.milestone != "M1" && row.milestone != "M2" && row.milestone != "M3"),
                     qPrintable(id + QStringLiteral(" is planned for a closed milestone")));
            QVERIFY2(f.w->commands().find(id) != nullptr,
                     qPrintable(id + QStringLiteral(" is not registered")));
            QVERIFY2(!f.w->commands().isEnabled(id), qPrintable(id + QStringLiteral(" is enabled")));
            QAction* action = f.w->actions().action(id);
            QVERIFY2(action != nullptr && !action->isEnabled(), qPrintable(id));
            const QString note =
                row.status == project::KeymapStatus::Reserved
                    ? QStringLiteral("reserved")
                    : QStringLiteral("planned for %1").arg(QString::fromStdString(row.milestone));
            QVERIFY2(action->toolTip().contains(note),
                     qPrintable(id + QStringLiteral(": tool tip '") + action->toolTip() + QLatin1Char('\'')));
            const int undo = f.w->session().undoStack().index();
            QVERIFY2(!f.w->commands().invoke(id), qPrintable(id + QStringLiteral(" ran")));
            QCOMPARE(f.w->session().undoStack().index(), undo);
            ++checked;
        }
        QVERIFY(checked > 0);
        qInfo("%d planned (after M3) or reserved commands checked", checked);
    }

    // Every implemented command has a table entry and no entry names another command (the runtime half of
    // tools/check_keymap_coverage.py).
    void tableMatchesTheKeymap() {
        QSet<QString> implemented;
        for (const project::KeyBinding& row : fx->w->session().keymap().bindings) {
            if (row.status == project::KeymapStatus::Implemented) {
                implemented.insert(QString::fromStdString(row.command));
            }
        }
        for (const auto& [command, c] : cases()) {
            QVERIFY2(implemented.contains(command),
                     qPrintable(command + QStringLiteral(" has no implemented row")));
        }
        QCOMPARE(static_cast<qsizetype>(cases().size()), implemented.size());
    }
};

QTEST_MAIN(TstKeymap)
#include "tst_keymap.moc"
