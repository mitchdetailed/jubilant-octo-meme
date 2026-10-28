// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DprWatcher, ComponentWidget::recheckDevicePixelRatio and WorksheetCanvas::refreshGeometry (M4 hidpi
// package, D-209; AC-M4-07; 06 section 17 items 3-4; 04 section 6.6 item 3), offscreen on the four screens of
// dpr_test_support.hpp (DPR 1, 1.25, 1.5, 2):
//   * a watched window moved to a screen of another DPR is checked once: changes() counts it, lastDpr()
//     follows it and dprChanged(window, from, to) names it; a move to a screen of the same DPR is no change;
//     unwatch() and a destroyed window end the watch;
//   * the gap the watcher closes: a component moved into a window of another DPR (QWidget::setParent, as a
//     dock does when it floats or docks back) gets no DPR event from Qt and keeps its old layers; refresh-
//     Components on its new window makes it rebuild at the window's DPR (once: a second refresh asks
//     nothing), and a canvas refreshed so keeps its logical placement;
//   * recheckDevicePixelRatio is false for a component already at its window's DPR and for one never laid
//     out.

#include "components/component_test_support.hpp"
#include "m4_hidpi/dpr_test_support.hpp"

#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/shell/dpr_watcher.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <QWindow>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::dprtest;

namespace {

constexpr int kTimeoutMs = 60'000;
constexpr const char* kGraphId = "7a2d1f9e-1a51-4f5a-9d43-0a7d2d5f0209";

project::WorksheetDef sheetOf(const World& w) {
    project::WorksheetDef def = w.sheet;
    def.components.push_back(
        timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}, project::Rect{0.0, 0.0, 0.6, 1.0}, 0, kGraphId));
    return def;
}

// A frameless top-level window on the screen of `dpr`, at `offset` inside it.
std::unique_ptr<QWidget> windowOn(qreal dpr, QPoint offset, QSize size) {
    auto w = std::make_unique<QWidget>();
    w->setScreen(screenForDpr(dpr));
    w->setGeometry(QRect(pointOn(dpr, offset), size));
    return w;
}

void moveTo(QWidget& top, qreal dpr, QPoint offset) {
    top.windowHandle()->setScreen(screenForDpr(dpr));
    top.move(pointOn(dpr, offset));
}

bool settledAt(TimeDistanceGraph& g, qreal dpr) {
    return settled(g) && g.layers().devicePixelRatio() == dpr && g.layoutDevicePixelRatio() == dpr;
}

} // namespace

class TstDprWatcher : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        prepareFontEnvironment();
        useSweepScreens("tst_dpr_watcher", std::filesystem::path(ESSES_TEST_BINARY_DIR));
    }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
        for (const ScreenSpec& s : kScreens) {
            QVERIFY2(screenNamed(QString::fromLatin1(s.name)) != nullptr, s.name);
        }
    }

    // A watched window across screens: one change per new DPR, none for the same DPR, none after unwatch.
    void watchedWindowAcrossScreens() {
        DprWatcher watcher;
        QSignalSpy spy(&watcher, &DprWatcher::dprChanged);
        std::unique_ptr<QWidget> top = windowOn(1.0, QPoint(60, 60), QSize(400, 300));
        auto* child = new QWidget(top.get());
        watcher.watch(child); // a child names its window
        watcher.watch(top.get());
        QCOMPARE(watcher.watched(), QList<QWidget*>{top.get()});
        top->show();
        QVERIFY(QTest::qWaitForWindowExposed(top.get()));
        QCOMPARE(top->devicePixelRatioF(), 1.0);
        QTRY_COMPARE(watcher.lastDpr(top.get()), 1.0);
        QCOMPARE(watcher.lastDpr(child), 1.0);
        drainEvents();
        QCOMPARE(watcher.changes(), 0);

        moveTo(*top, 2.0, QPoint(60, 60));
        QTRY_COMPARE_WITH_TIMEOUT(watcher.changes(), 1, kTimeoutMs);
        QCOMPARE(watcher.lastDpr(top.get()), 2.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QWidget*>(), top.get());
        QCOMPARE(spy.at(0).at(1).toReal(), 1.0);
        QCOMPARE(spy.at(0).at(2).toReal(), 2.0);

        moveTo(*top, 1.25, QPoint(60, 60));
        QTRY_COMPARE_WITH_TIMEOUT(watcher.changes(), 2, kTimeoutMs);
        QCOMPARE(watcher.lastDpr(top.get()), 1.25);
        QCOMPARE(spy.at(1).at(2).toReal(), 1.25);

        // Within the same screen: no change.
        top->move(pointOn(1.25, QPoint(200, 120)));
        drainEvents();
        QCOMPARE(watcher.changes(), 2);

        watcher.unwatch(top.get());
        QVERIFY(watcher.watched().isEmpty());
        QCOMPARE(watcher.lastDpr(top.get()), 0.0);
        moveTo(*top, 1.5, QPoint(60, 60));
        QTRY_COMPARE(top->devicePixelRatioF(), 1.5);
        drainEvents();
        QCOMPARE(watcher.changes(), 2);
        QCOMPARE(spy.count(), 2);
    }

    // A destroyed window is forgotten.
    void destroyedWindowIsForgotten() {
        DprWatcher watcher;
        auto top = windowOn(1.0, QPoint(80, 80), QSize(200, 150));
        watcher.watch(top.get());
        QCOMPARE(watcher.watched().size(), 1);
        top.reset();
        QVERIFY(watcher.watched().isEmpty());
        QCOMPARE(watcher.refreshAll(), 0);
    }

    // The reparent gap: a graph moved from a DPR 2 window into a DPR 1 one gets no DPR event from Qt; the
    // watcher's refresh makes it rebuild at 1, once, and the canvas keeps its logical placement.
    void reparentedComponentRebuildsOnRefresh() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        std::unique_ptr<QWidget> hi = windowOn(2.0, QPoint(40, 40), QSize(700, 400));
        std::unique_ptr<QWidget> lo = windowOn(1.0, QPoint(40, 40), QSize(700, 400));
        auto* layout = new QVBoxLayout(lo.get());
        layout->setContentsMargins(0, 0, 0, 0);
        auto* canvas = new WorksheetCanvas(hi.get());
        canvas->setGeometry(0, 0, 700, 400);
        canvas->setWorksheet(sheetOf(w), *w.context);
        hi->show();
        lo->show();
        QVERIFY(QTest::qWaitForWindowExposed(hi.get()));
        QVERIFY(QTest::qWaitForWindowExposed(lo.get()));
        QPointer<TimeDistanceGraph> g =
            qobject_cast<TimeDistanceGraph*>(canvas->componentById(Uuid::parse(kGraphId)));
        QVERIFY(g != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(settledAt(*g, 2.0), kTimeoutMs);
        QCOMPARE(g->layers().staticImage().size(), QSize(840, 800));
        QCOMPARE(DprWatcher::refreshComponents(*hi), 0); // already at its window's DPR
        const QRect placed = g->geometry();

        layout->addWidget(canvas); // into the DPR 1 window, as a dock does
        QVERIFY(canvas->window() == lo.get());
        QCOMPARE(g->devicePixelRatioF(), 1.0);
        drainEvents();
        QTRY_VERIFY(poolSettled(w.rt.pool()));
        drainEvents();
        QCOMPARE(g->layoutDevicePixelRatio(), 2.0); // Qt sent it nothing: still laid out at 2
        QCOMPARE(g->layers().devicePixelRatio(), 2.0);

        const int passes = canvas->layoutPasses();
        QCOMPARE(DprWatcher::refreshComponents(*lo), 1);
        QTRY_VERIFY_WITH_TIMEOUT(settledAt(*g, 1.0), kTimeoutMs);
        QCOMPARE(g->layers().staticImage().size(), QSize(420, 400));
        QCOMPARE(g->layers().staticImage().devicePixelRatio(), 1.0);
        QCOMPARE(g->layout().dpr, 1.0F);
        // The canvas lays out again (coalesced: when the reparent's own relayout ran a moment ago, the pass
        // comes with the coalescing timer).
        QTRY_VERIFY_WITH_TIMEOUT(canvas->layoutPasses() > passes, kTimeoutMs);
        QCOMPARE(g->geometry(), placed); // in the same logical place
        QCOMPARE(DprWatcher::refreshComponents(*lo), 0);
        QVERIFY(!g->recheckDevicePixelRatio());
    }

    // A watched window's components and canvases are refreshed by its own DPR change too (the watcher's path
    // agrees with Qt's events: one rebuild, never two).
    void watchedWindowRefreshesItsComponents() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        std::unique_ptr<QWidget> top = windowOn(1.0, QPoint(40, 40), QSize(700, 400));
        auto* canvas = new WorksheetCanvas(top.get());
        canvas->setGeometry(0, 0, 700, 400);
        canvas->setWorksheet(sheetOf(w), *w.context);
        DprWatcher watcher;
        watcher.watch(top.get());
        top->show();
        QVERIFY(QTest::qWaitForWindowExposed(top.get()));
        auto* g = qobject_cast<TimeDistanceGraph*>(canvas->componentById(Uuid::parse(kGraphId)));
        QVERIFY(g != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(settledAt(*g, 1.0), kTimeoutMs);
        const int sizes = g->sizeChangesDelivered();
        const QRect placed = g->geometry();
        moveTo(*top, 1.5, QPoint(40, 40));
        QTRY_COMPARE_WITH_TIMEOUT(watcher.changes(), 1, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(settledAt(*g, 1.5), kTimeoutMs);
        QCOMPARE(g->layers().staticImage().size(), QSize(630, 600));
        QCOMPARE(g->sizeChangesDelivered(), sizes + 1); // one Size|Scheme rebuild for the move
        QCOMPARE(g->geometry(), placed);
        QCOMPARE(watcher.refreshAll(), 0);
    }

    // Never laid out: nothing to compare.
    void neverLaidOutIsNoRecheck() {
        World w;
        auto* c = ComponentWidget::create(
            timeDistanceSpec({{"Throttle"}}, project::Rect{0.0, 0.0, 1.0, 1.0}, 0, kGraphId), *w.context);
        std::unique_ptr<ComponentWidget> owned(c);
        QCOMPARE(c->layoutDevicePixelRatio(), 0.0);
        QVERIFY(!c->recheckDevicePixelRatio());
    }
};

QTEST_MAIN(TstDprWatcher)
#include "tst_dpr_watcher.moc"
