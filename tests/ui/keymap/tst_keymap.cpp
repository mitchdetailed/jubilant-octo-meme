// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-06 (06 sections 14.4-14.5, D-033): one Qt Test row per key of every row of data/keymaps/default.json
// with "status": "implemented", run offscreen in the real shell. Each row resets a fixture (the MainWindow
// with two synthetic circuit logs opened through the SourceLoader), arranges the command's precondition, puts
// the focus where the row's context lives (global / worksheet / graph: the time_distance component of the
// active worksheet; data_window: the Data dock's tree), sends the key with QTest (through the KeyEventRouter
// and the KeymapBinder) and checks the documented post-condition. Every command id with an implemented row
// has a KEYMAP_TEST entry naming it below: a row without one fails here, and tools/check_keymap_coverage.py
// checks the same at lint time (08 section 6.5).

#include "../../../libs/qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/keymap.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QIdentityProxyModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLineEdit>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <cmath>
#include <functional>
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

struct Fixture;

struct Before {
    plot::SelectionSnapshot selection;
    plot::ViewState view;
    int sheet = 0;
    bool valuesVisible = false;
};

struct Case {
    // Before the focus is placed (worksheet switches recreate the components).
    std::function<void(Fixture&, int arg)> arrange;
    // Replaces the focus of the row's context (e.g. ESC pressed inside a dock).
    std::function<void(Fixture&, int arg)> focus;
    std::function<void(Fixture&, const Before&, int arg)> verify;
    std::function<void(Fixture&)> after;
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

const std::map<QString, Case>& cases() {
    using F = Fixture;
    static const std::map<QString, Case> table{
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
        // ---- data ----
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
        // N / P: the next / previous Main range with the zoom and the cursor kept (06 section 5.2), and no
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
         {{}, {}, [](F& f, const Before& b, int) { QVERIFY(f.ws().state().cursorT < b.view.cursorT); }, {}}},
        {KEYMAP_TEST("cursor.right"),
         {{}, {}, [](F& f, const Before& b, int) { QVERIFY(f.ws().state().cursorT > b.view.cursorT); }, {}}},
        {KEYMAP_TEST("cursor.left_fast"),
         {{},
          {},
          [](F& f, const Before& b, int) {
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
              QVERIFY(t < b.view.zoom.b && t > b.view.zoom.b - 0.1 * kSec); // within one sample of the edge
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
    return table;
}

void zoomInsideMain(Fixture& f) {
    f.ws().zoomTo(plot::XSpan{5.0 * kSec, 20.0 * kSec});
    f.ws().setCursor(7 * kSec, qt::CursorSource::Programmatic);
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
            for (const std::string& key : row.keys) {
                const QString command = QString::fromStdString(row.command);
                const QString name = command + QLatin1Char(' ') + QString::fromStdString(key) +
                                     QLatin1Char(' ') + QString::fromStdString(row.context);
                QTest::newRow(name.toUtf8().constData()) << command << QString::fromStdString(row.context)
                                                         << QString::fromStdString(key) << arg << false;
                // A global row works from any dock (06 section 14.2): the same key with the focus in the Data
                // window's tree, unless the row places its own focus.
                const auto it = cases().find(command);
                if (row.context == "global" && it != cases().end() && !it->second.focus) {
                    const QString dockName = name + QStringLiteral(" (from the Data window)");
                    QTest::newRow(dockName.toUtf8().constData())
                        << command << QString::fromStdString(row.context) << QString::fromStdString(key)
                        << arg << true;
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
        } else {
            f.focusGraph(); // global, worksheet and graph rows: the time_distance component has focus
        }
        QVERIFY(QApplication::focusWidget() != nullptr);
        Before before;
        before.selection = f.sel().snapshot();
        before.view = f.ws().state();
        before.sheet = f.w->activeWorksheetIndex();
        before.valuesVisible = f.w->docks().isVisible(kValues);

        const auto [k, mods] = parseKey(key);
        QTest::keyClick(QApplication::focusWidget(), k, mods);
        c.verify(f, before, arg);
        if (c.after) {
            c.after(f);
        }
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
