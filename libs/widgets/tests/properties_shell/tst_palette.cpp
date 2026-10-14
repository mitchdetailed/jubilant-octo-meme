// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M3-12 (06 section 15; test palette): the command palette in the real shell with a synthetic log open. It
// lists every CommandRegistry id ('?' also the disabled ones, a command with keymap arguments once per row)
// with the key the keymap binds, filters by fuzzy subsequence with prefix matches first and recently used
// commands next, keeps the last 20 invoked ids, and executing a command from it equals pressing its key: a
// shared harness runs each case once by key (through the KeyEventRouter) and once through the palette from
// the same starting state, and compares the resulting state (view, selection, docks, the focused component's
// spec). CTRL+SHIFT+P opens it with the focus context captured; ESC closes it; the '>' list switches
// worksheets and the '#' list sets Main (ENTER) or Overlay 0 (CTRL+ENTER).

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"
#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QKeySequence>
#include <QLineEdit>
#include <QSet>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

constexpr Micros kSec = 1'000'000;

struct State {
    plot::ViewState view;
    plot::SelectionSnapshot selection;
    bool values = false;
    project::ComponentSpec focused;
    bool operator==(const State& o) const {
        return view.zoom == o.view.zoom && view.cursorT == o.view.cursorT && view.datumT == o.view.datumT &&
               view.domain == o.view.domain && selection.main == o.selection.main &&
               selection.singleColourMain == o.selection.singleColourMain &&
               selection.overlaysHidden == o.selection.overlaysHidden && values == o.values &&
               focused == o.focused;
    }
};

struct Fixture {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt = runtime(4);
    std::unique_ptr<MainWindow> w;
    std::vector<model::RangeRef> laps;

    bool start() {
        if (!config.isValid() || !logs.isValid()) {
            return false;
        }
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            qt::test::SyntheticOptions o;
            o.path = "palette.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            ld = qt::test::writeLd(*qt::test::makeSource(o, rt->cache()), fsPath(logs.path()), "palette");
        }
        w = std::make_unique<MainWindow>(*rt, pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        if (!showAndActivate(*w)) {
            return false;
        }
        w->openLog(ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == 1; }, 30000)) {
            return false;
        }
        laps = w->selection().displayedRanges(w->selection().sources().front()->id);
        return laps.size() == 5;
    }
    ~Fixture() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }
    ComponentWidget* graph() const {
        for (ComponentWidget* c : w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String("time_distance")) {
                return c;
            }
        }
        return nullptr;
    }
    void reset() {
        w->selection().setMain(laps[2]);
        w->selection().setSingleColourMain(false);
        if (w->activeWorksheet()->domain() != plot::Domain::Time) {
            w->activeWorksheet()->setDomain(plot::Domain::Time);
        }
        w->activeWorksheet()->zoomToMainRange();
        w->activeWorksheet()->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        w->workspace().setDatum(std::nullopt);
        w->docks().setState(QStringLiteral("values"), w->docks().defaultState(QStringLiteral("values")));
        ComponentWidget* g = graph();
        w->canvas().focusComponent(g);
        g->setFocus(Qt::OtherFocusReason);
    }
    State state() {
        State s;
        s.view = w->activeWorksheet()->state();
        s.selection = w->selection().snapshot();
        s.values = w->docks().isVisible(QStringLiteral("values"));
        s.focused = graph()->spec();
        return s;
    }
};

std::pair<Qt::Key, Qt::KeyboardModifiers> parseKey(const QString& text) {
    const QKeyCombination c = QKeySequence::fromString(text, QKeySequence::PortableText)[0];
    return {c.key(), c.keyboardModifiers()};
}

} // namespace

class TstPalette : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
        fx_ = std::make_unique<Fixture>();
        QVERIFY(fx_->start());
    }
    void cleanupTestCase() { fx_.reset(); }

    // Every CommandRegistry id is listed ('?' query), with its keymap key.
    void listsEveryCommandWithItsKey() {
        Fixture& f = *fx_;
        CommandPalette& p = f.w->commandPalette();
        p.popup(qt::CommandContext::Global, QStringLiteral("?"));
        const QStringList ids = p.visibleIds();
        QSet<QString> listed(ids.begin(), ids.end());
        for (const qt::CommandSpec* spec : f.w->commands().all()) {
            QVERIFY2(listed.contains(spec->id), qPrintable(spec->id + QStringLiteral(" not listed")));
            const QStringList keys = f.w->keymap().keysFor(spec->id);
            QCOMPARE(p.keyText(spec->id), keys.isEmpty() ? QString() : keys.front());
        }
        // A command with keymap arguments is listed once per row (view.worksheet_n: keys 1..9, 0).
        QCOMPARE(ids.count(QStringLiteral("view.worksheet_n")), qsizetype{10});
        // Disabled commands only with '?'.
        QVERIFY(!f.w->commands().isEnabled(QStringLiteral("file.print")));
        p.setQuery(QString());
        QVERIFY(!p.visibleIds().contains(QStringLiteral("file.print")));
        QVERIFY(p.visibleIds().contains(QStringLiteral("zoom.in")));
        // Opened without a query: sorted by title.
        p.hide();
    }

    void fuzzyFilterAndRecent() {
        Fixture& f = *fx_;
        f.reset();
        CommandPalette& p = f.w->commandPalette();
        p.popup(qt::CommandContext::Graph, QStringLiteral("zoom in"));
        QVERIFY(!p.visibleIds().isEmpty());
        QCOMPARE(p.visibleIds().front(), QStringLiteral("zoom.in")); // a prefix match comes first
        p.setQuery(QStringLiteral("zmin"));                          // a subsequence of "Zoom In"
        QVERIFY(p.visibleIds().contains(QStringLiteral("zoom.in")));
        p.setQuery(QStringLiteral("qqqqqq"));
        QVERIFY(p.visibleIds().isEmpty());
        QVERIFY(!p.invokeCurrent());
        // Recently used first among the matches that are not prefix matches.
        p.setQuery(QStringLiteral("datum"));
        const QStringList before = p.visibleIds();
        QVERIFY(before.contains(QStringLiteral("datum.swap")));
        p.setCurrentRow(static_cast<int>(before.indexOf(QStringLiteral("datum.swap"))));
        f.w->activeWorksheet()->placeDatum();
        QVERIFY(p.invokeCurrent());
        QCOMPARE(p.recentIds().front(), QStringLiteral("datum.swap"));
        p.popup(qt::CommandContext::Graph, QStringLiteral("datum"));
        QCOMPARE(p.visibleIds().front(), QStringLiteral("datum.swap"));
        p.hide();
        f.reset();
    }

    // The shared harness: the key and the palette give the same state from the same start.
    void paletteEqualsTheKey_data() {
        QTest::addColumn<QString>("command");
        QTest::newRow("zoom.in") << QStringLiteral("zoom.in");
        QTest::newRow("data.toggle_domain") << QStringLiteral("data.toggle_domain");
        QTest::newRow("datum.place") << QStringLiteral("datum.place");
        QTest::newRow("cursor.right") << QStringLiteral("cursor.right");
        QTest::newRow("pan.forward") << QStringLiteral("pan.forward");
        QTest::newRow("data.single_colour_main") << QStringLiteral("data.single_colour_main");
        QTest::newRow("view.values_window") << QStringLiteral("view.values_window");
        QTest::newRow("component.legend") << QStringLiteral("component.legend");
        QTest::newRow("data.next_range") << QStringLiteral("data.next_range");
    }
    void paletteEqualsTheKey() {
        QFETCH(QString, command);
        Fixture& f = *fx_;
        const QStringList keys = f.w->keymap().keysFor(command);
        QVERIFY(!keys.isEmpty());
        const int undo = f.w->session().undoStack().index();

        f.reset();
        const auto [k, mods] = parseKey(keys.front());
        QTest::keyClick(QApplication::focusWidget(), k, mods);
        const State byKey = f.state();
        while (f.w->session().undoStack().index() > undo) {
            f.w->session().undoStack().undo();
        }

        f.reset();
        const State start = f.state();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        CommandPalette& p = f.w->commandPalette();
        QVERIFY(p.isVisible());
        QCOMPARE(p.capturedContext(), qt::CommandContext::Graph);
        p.setQuery(f.w->commands().find(command)->title);
        const qsizetype row = p.visibleIds().indexOf(command);
        QVERIFY2(row >= 0, qPrintable(command + QStringLiteral(" not found by its title")));
        p.setCurrentRow(static_cast<int>(row));
        QVERIFY(p.invokeCurrent());
        QVERIFY(!p.isVisible());
        const State byPalette = f.state();
        QVERIFY2(!(byPalette == start), qPrintable(command + QStringLiteral(" changed nothing")));
        QVERIFY2(byPalette == byKey,
                 qPrintable(command + QStringLiteral(": the palette and the key differ")));
        while (f.w->session().undoStack().index() > undo) {
            f.w->session().undoStack().undo();
        }
    }

    void prefixesAndEscape() {
        Fixture& f = *fx_;
        f.reset();
        CommandPalette& p = f.w->commandPalette();
        // '>' lists the worksheets; ENTER switches.
        p.popup(qt::CommandContext::Graph, QStringLiteral(">"));
        QCOMPARE(p.visibleIds().size(), qsizetype{f.w->worksheetCount()});
        p.setCurrentRow(1);
        QVERIFY(p.invokeCurrent());
        QCOMPARE(f.w->activeWorksheetIndex(), 1);
        f.w->selectWorksheet(0);
        f.reset();
        // '#' lists the Main source's ranges: ENTER makes one Main, CTRL+ENTER Overlay 0.
        p.popup(qt::CommandContext::Graph, QStringLiteral("#"));
        QCOMPARE(p.visibleIds().size(), qsizetype{5});
        p.setCurrentRow(3);
        QVERIFY(p.invokeCurrent());
        QVERIFY(f.w->selection().snapshot().main == f.laps[3]);
        p.popup(qt::CommandContext::Graph, QStringLiteral("#"));
        p.setCurrentRow(1);
        QVERIFY(p.invokeCurrent(Qt::ControlModifier));
        QVERIFY(f.w->selection().snapshot().overlay0 && *f.w->selection().snapshot().overlay0 == f.laps[1]);
        f.w->selection().clearOverlays();
        // '@' lists channels.
        p.popup(qt::CommandContext::Graph, QStringLiteral("@Throttle"));
        QVERIFY(!p.visibleIds().isEmpty());
        QVERIFY(p.visibleIds().front().startsWith(QStringLiteral("channel:")));
        // ESC closes and gives the focus back.
        QWidget* edit = QApplication::focusWidget();
        QVERIFY(qobject_cast<QLineEdit*>(edit) != nullptr);
        QTest::keyClick(edit, Qt::Key_Escape);
        QVERIFY(!p.isVisible());
        f.reset();
    }

private:
    std::unique_ptr<Fixture> fx_;
};

QTEST_MAIN(TstPalette)
#include "tst_palette.moc"
