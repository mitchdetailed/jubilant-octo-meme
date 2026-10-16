// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetCanvas M3 (06 section 4.3 items 2-5, 8-11; 04 section 7.2 item 7): the shown worksheet updated in
// place (the surviving widgets kept, changed specs applied and announced, new ones created, removed ones
// destroyed); selection (CTRL+click toggles, a rubber band on empty canvas selects by intersection, select
// all, a plain click on another component clears it); moving by the caption and by SHIFT+drag with the
// selection, snapped to the grid (ALT: not snapped), inside the canvas; resizing by an edge with the minimum
// of 4 x 4 cells; the CTRL+drag splitter moving a shared edge for both components; ALT+arrow nudges; the z
// order (send to background, bring to front, renumbered 0..n-1); each a single layoutEdited with the
// component's id as merge key; and the layout lock refusing all of them. Offscreen, over the qtmodel services
// of cmptest::World.

#include "components/component_test_support.hpp"

#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

bool onGrid(double v) {
    return std::abs(v * 100.0 - std::round(v * 100.0)) < 1e-9;
}

struct Canvas {
    World world;
    QWidget host;
    WorksheetCanvas* canvas = nullptr;
    project::WorksheetDef def;

    Canvas() {
        host.resize(1000, 600);
        canvas = new WorksheetCanvas(&host);
        canvas->setGeometry(0, 0, 1000, 600);
        def = world.sheet;
        def.components.push_back(valuesListSpec({"Throttle"}, project::Rect{0.0, 0.0, 0.5, 0.5}, 0));
        def.components.push_back(valuesListSpec({"Throttle"}, project::Rect{0.5, 0.0, 1.0, 0.5}, 1));
        def.components.push_back(valuesListSpec({"Throttle"}, project::Rect{0.2, 0.6, 0.4, 0.9}, 2));
        canvas->setWorksheet(def, *world.context);
        host.show();
    }
    ComponentWidget* at(int i) const {
        return canvas->componentById(def.components[static_cast<std::size_t>(i)].id);
    }
    QPoint centreOf(int i) const { return at(i)->geometry().center(); }
};

} // namespace

class TstWorksheetCanvasM3 : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void updatesInPlace() {
        Canvas c;
        QVERIFY(QTest::qWaitForWindowExposed(&c.host));
        ComponentWidget* first = c.at(0);
        ComponentWidget* second = c.at(1);
        const QPointer<ComponentWidget> third = c.at(2);
        QSignalSpy applied(c.canvas, &WorksheetCanvas::componentSpecApplied);
        project::WorksheetDef next = c.def;
        next.components[0].caption.text = "Renamed";
        next.components.erase(next.components.begin() + 2);
        next.components.push_back(valuesListSpec({"Throttle"}, project::Rect{0.0, 0.5, 0.3, 1.0}, 5));
        c.canvas->setWorksheet(next, *c.world.context);
        QCOMPARE(c.canvas->componentById(next.components[0].id), first);  // kept and updated
        QCOMPARE(c.canvas->componentById(next.components[1].id), second); // kept, unchanged
        QCOMPARE(first->spec().caption.text, std::string("Renamed"));
        QVERIFY(third.isNull());                                            // removed: destroyed
        QVERIFY(c.canvas->componentById(next.components[2].id) != nullptr); // added
        QCOMPARE(applied.count(), 1);
        QCOMPARE(applied.front().front().value<ComponentWidget*>(), first);
        QVERIFY(c.canvas->worksheet() == next);
    }

    void selection() {
        Canvas c;
        QVERIFY(QTest::qWaitForWindowExposed(&c.host));
        QSignalSpy changed(c.canvas, &WorksheetCanvas::selectionChanged);
        c.canvas->focusComponent(c.at(0));
        QVERIFY(c.canvas->selection().isEmpty());
        QCOMPARE(c.canvas->selectedOrFocused(), QList<ComponentWidget*>{c.at(0)});
        // CTRL+click on another component: both are selected; again: it leaves.
        QTest::mouseClick(c.at(1), Qt::LeftButton, Qt::ControlModifier, QPoint(100, 100));
        QCOMPARE(c.canvas->selection().size(), qsizetype{2});
        QVERIFY(c.canvas->isSelected(c.at(0)) && c.canvas->isSelected(c.at(1)));
        QTest::mouseClick(c.at(1), Qt::LeftButton, Qt::ControlModifier, QPoint(100, 100));
        QCOMPARE(c.canvas->selection(), QList<ComponentWidget*>{c.at(0)});
        // A plain click on another component: the selection is that one alone (the focused one).
        QTest::mouseClick(c.at(2), Qt::LeftButton, Qt::NoModifier, QPoint(50, 50));
        QVERIFY(c.canvas->selection().isEmpty());
        QCOMPARE(c.canvas->focusedComponent(), c.at(2));
        // A rubber band from an empty spot (700, 560) to (300, 200) intersects all three components.
        QTest::mousePress(c.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(700, 560)); // empty area
        QTest::mouseMove(c.canvas, QPoint(300, 200));
        QTest::mouseRelease(c.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
        // The band [300, 700] x [200, 560] intersects all three components.
        QCOMPARE(c.canvas->selection().size(), qsizetype{3});
        c.canvas->clearSelection();
        c.canvas->selectAll();
        QCOMPARE(c.canvas->selection().size(), qsizetype{3});
        QVERIFY(changed.count() >= 4);
    }

    void moveAndResize() {
        Canvas c;
        QVERIFY(QTest::qWaitForWindowExposed(&c.host));
        QSignalSpy edits(c.canvas, &WorksheetCanvas::layoutEdited);
        ComponentWidget* comp = c.at(2); // [0.2, 0.6, 0.4, 0.9] = x 200..400, y 360..540
        // The pointer moves in canvas coordinates (the component itself moves during the drag).
        const auto drag = [&](ComponentWidget* w, QPoint local, QPoint travel, Qt::KeyboardModifiers mods) {
            const QPoint from = w->mapTo(c.canvas, local);
            QTest::mousePress(w, Qt::LeftButton, mods, local);
            QTest::mouseMove(c.canvas, from + travel / 3);
            QTest::mouseMove(c.canvas, from + travel);
            QTest::mouseRelease(c.canvas, Qt::LeftButton, mods, from + travel);
        };
        // Drag the caption bar (below the 6 px top edge zone) by (+33, -25) px: 3.3 % and -4.17 % -> snapped
        // to 3 and -4 cells.
        const QPoint caption(60, 10);
        drag(comp, caption, QPoint(33, -25), Qt::NoModifier);
        QCOMPARE(edits.count(), 1);
        QCOMPARE(edits.front().at(1).toString(), QString::fromStdString(comp->spec().id.str()));
        project::Rect r = comp->spec().rect;
        QVERIFY(onGrid(r.l) && onGrid(r.t) && onGrid(r.r) && onGrid(r.b));
        QCOMPARE(std::lround(r.l * 100), 23L);
        QCOMPARE(std::lround(r.t * 100), 56L);
        QCOMPARE(std::lround((r.r - r.l) * 100), 20L); // size kept
        QVERIFY(c.canvas->worksheet().components[2].rect == r);
        // ALT held during the drag: not snapped (+7 px = 0.7 %).
        {
            const QPoint from = comp->mapTo(c.canvas, caption);
            QTest::mousePress(comp, Qt::LeftButton, Qt::NoModifier, caption);
            QTest::mouseMove(c.canvas, from + QPoint(5, 0));
            const QPoint to = from + QPoint(7, 0);
            QMouseEvent alt(QEvent::MouseMove, QPointF(to), c.canvas->mapToGlobal(QPointF(to)), Qt::NoButton,
                            Qt::LeftButton, Qt::AltModifier);
            QApplication::sendEvent(c.canvas, &alt);
            QTest::mouseRelease(c.canvas, Qt::LeftButton, Qt::AltModifier, to);
        }
        r = comp->spec().rect;
        QVERIFY2(std::abs(r.l - 0.237) < 1e-9, qPrintable(QString::number(r.l)));
        // Resize by the right edge (+120 px -> 0.557, snapped to 0.56), then far to the left: 4 cells wide.
        drag(comp, QPoint(comp->width() - 2, comp->height() / 2), QPoint(120, 0), Qt::NoModifier);
        r = comp->spec().rect;
        QCOMPARE(std::lround(r.r * 100), 56L);
        drag(comp, QPoint(comp->width() - 2, comp->height() / 2), QPoint(-900, 0), Qt::NoModifier);
        r = comp->spec().rect;
        QVERIFY(std::abs((r.r - r.l) - 0.04) < 1e-9);
        // SHIFT+drag in the body moves the selection: components 0 and 1 together.
        c.canvas->setSelection({c.at(0), c.at(1)});
        const project::Rect a0 = c.at(0)->spec().rect;
        const project::Rect b0 = c.at(1)->spec().rect;
        const int before = static_cast<int>(edits.count());
        drag(c.at(0), QPoint(200, 150), QPoint(0, 60), Qt::ShiftModifier);
        QCOMPARE(edits.count(), before + 1);
        QCOMPARE(edits.back().at(1).toString(), QString()); // several components: no merge key
        QCOMPARE(std::lround((c.at(0)->spec().rect.t - a0.t) * 100), 10L);
        QCOMPARE(std::lround((c.at(1)->spec().rect.t - b0.t) * 100), 10L);
    }

    void splitterNudgeAndZOrder() {
        Canvas c;
        QVERIFY(QTest::qWaitForWindowExposed(&c.host));
        // Components 0 and 1 share the edge x = 0.5: CTRL+drag on 0's right edge moves it for both.
        ComponentWidget* left = c.at(0);
        const QPoint edge(left->width() - 2, 100);
        const QPoint from = left->mapTo(c.canvas, edge);
        QTest::mousePress(left, Qt::LeftButton, Qt::ControlModifier, edge);
        QTest::mouseMove(c.canvas, from + QPoint(-50, 0));
        QTest::mouseMove(c.canvas, from + QPoint(-100, 0));
        QTest::mouseRelease(c.canvas, Qt::LeftButton, Qt::ControlModifier, from + QPoint(-100, 0));
        QCOMPARE(std::lround(c.at(0)->spec().rect.r * 100), 40L);
        QCOMPARE(std::lround(c.at(1)->spec().rect.l * 100), 40L);
        QCOMPARE(std::lround(c.at(1)->spec().rect.r * 100), 100L);
        QVERIFY(c.canvas->moveSharedEdge(true, 0.4, 0.45));
        QCOMPARE(std::lround(c.at(0)->spec().rect.r * 100), 45L);
        // ALT held: the edge lands where the pointer left it, off the 1 % grid (06 section 4.3 item 2).
        {
            ComponentWidget* l = c.at(0);
            const QPoint e(l->width() - 2, 100);
            const QPoint f = l->mapTo(c.canvas, e);
            QTest::mousePress(l, Qt::LeftButton, Qt::ControlModifier, e);
            const QPoint to = f + QPoint(-23, 0); // 0.45 - 0.023 = 0.427 of 1000 px
            QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(c.canvas->mapToGlobal(to)), Qt::NoButton,
                             Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier);
            QApplication::sendEvent(c.canvas, &move);
            QTest::mouseRelease(c.canvas, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier, to);
            QVERIFY(!onGrid(c.at(0)->spec().rect.r));
            QVERIFY(std::abs(c.at(0)->spec().rect.r - 0.427) < 0.002);
            QCOMPARE(c.at(1)->spec().rect.l, c.at(0)->spec().rect.r);
            QVERIFY(c.canvas->moveSharedEdge(true, c.at(0)->spec().rect.r, 0.45));
        }
        // Nudge: one cell, the size kept; ALT+arrow through the canvas.
        c.canvas->focusComponent(c.at(2));
        const project::Rect n0 = c.at(2)->spec().rect;
        QVERIFY(c.canvas->nudgeSelection(1, 0));
        QCOMPARE(std::lround((c.at(2)->spec().rect.l - n0.l) * 100), 1L);
        QTest::keyClick(c.at(2), Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(std::lround((c.at(2)->spec().rect.t - n0.t) * 100), 1L);
        // Z order: background gives z 0 and renumbers the others; front gives the top.
        QSignalSpy edits(c.canvas, &WorksheetCanvas::layoutEdited);
        QVERIFY(c.canvas->sendToBackground(c.at(2)));
        QCOMPARE(c.at(2)->spec().z, 0);
        QCOMPARE(c.at(0)->spec().z, 1);
        QCOMPARE(c.at(1)->spec().z, 2);
        QCOMPARE(c.canvas->components().front(), c.at(2)); // paint order follows
        QVERIFY(c.canvas->bringToFront(c.at(2)));
        QCOMPARE(c.at(2)->spec().z, 2);
        QCOMPARE(c.canvas->components().back(), c.at(2));
        QVERIFY(!c.canvas->bringToFront(c.at(2))); // already on top: no edit
        QCOMPARE(edits.count(), 2);
    }

    void layoutLock() {
        Canvas c;
        QVERIFY(QTest::qWaitForWindowExposed(&c.host));
        QSignalSpy edits(c.canvas, &WorksheetCanvas::layoutEdited);
        c.canvas->setLayoutLocked(true);
        const project::Rect r0 = c.at(2)->spec().rect;
        QTest::mousePress(c.at(2), Qt::LeftButton, Qt::NoModifier, QPoint(60, 10));
        QTest::mouseMove(c.at(2), QPoint(160, 50));
        QTest::mouseRelease(c.at(2), Qt::LeftButton, Qt::NoModifier, QPoint(160, 50));
        QVERIFY(c.at(2)->spec().rect == r0);
        c.canvas->focusComponent(c.at(2));
        QVERIFY(!c.canvas->nudgeSelection(1, 1));
        QVERIFY(!c.canvas->sendToBackground(c.at(2)));
        QVERIFY(!c.canvas->moveSharedEdge(true, 0.5, 0.3));
        QVERIFY(
            !c.canvas->setRects({{c.at(2)->spec().id, project::Rect{0, 0, 0.5, 0.5}}}, QStringLiteral("x")));
        QCOMPARE(edits.count(), 0);
        c.canvas->setLayoutLocked(false);
        QVERIFY(c.canvas->nudgeSelection(1, 1));
        QCOMPARE(edits.count(), 1);
    }

    void snapped() {
        const project::Rect s =
            WorksheetCanvas::snapped(project::Rect{0.123, 0.456, 0.789, 0.501}, QSize(100, 100), false);
        QCOMPARE(std::lround(s.l * 100), 12L);
        QCOMPARE(std::lround(s.r * 100), 79L);
        QCOMPARE(std::lround(s.t * 100), 46L);
        QCOMPARE(std::lround(s.b * 100), 50L); // 46..50 is the minimum of 4 cells
        const project::Rect k =
            WorksheetCanvas::snapped(project::Rect{0.97, 0.0, 1.17, 0.2}, QSize(100, 100), true);
        QCOMPARE(std::lround(k.r * 100), 100L); // kept inside, size kept
        QCOMPARE(std::lround((k.r - k.l) * 100), 20L);
    }
};

QTEST_MAIN(TstWorksheetCanvasM3)
#include "tst_worksheet_canvas_m3.moc"
