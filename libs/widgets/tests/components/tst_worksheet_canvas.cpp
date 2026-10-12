// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetCanvas (06 section 4.3, 04 section 7.2 items 1 and 7), offscreen: free fractional placement with
// the OffscreenRenderer rounding, paint order by z, unknown types as placeholders; focus (exactly one
// focused component with the Chrome border, click, TAB/SHIFT+TAB in z then reading order, remembered per
// worksheet, returnFocus); maximise/restore by caption double-click; the M1 edit path (specEditRequested ->
// applySpec + componentEdited); 30 Hz relayout coalescing.

#include "components/component_test_support.hpp"

#include <esses/widgets/components/placeholder_component.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/values_list.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr const char* kGraphId = "6f1c1f9e-1a51-4f5a-9d43-0a7d2d5f0001";
constexpr const char* kValuesId = "6f1c1f9e-1a51-4f5a-9d43-0a7d2d5f0002";
constexpr const char* kGaugeId = "6f1c1f9e-1a51-4f5a-9d43-0a7d2d5f0003";
constexpr const char* kSecondGraphId = "6f1c1f9e-1a51-4f5a-9d43-0a7d2d5f0004";

// graph [0,0,0.75,1] z 1; values [0.75,0,1,1] z 0; an unregistered type [0.5,0.5,1,1] z 2 (overlapping both).
project::WorksheetDef sheetOf(const World& w) {
    project::WorksheetDef def = w.sheet;
    def.components.push_back(timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}},
                                              project::Rect{0.0, 0.0, 0.75, 1.0}, 1, kGraphId));
    def.components.push_back(
        valuesListSpec({"Throttle", "Wheel Speed FL"}, project::Rect{0.75, 0.0, 1.0, 1.0}, 0, kValuesId));
    project::ComponentSpec gauge;
    gauge.id = Uuid::parse(kGaugeId);
    gauge.type = "gauge_numeric"; // an MVP type this build does not register yet
    gauge.rect = project::Rect{0.5, 0.5, 1.0, 1.0};
    gauge.z = 2;
    def.components.push_back(gauge);
    return def;
}

struct Rig {
    explicit Rig(World& w, const project::WorksheetDef& def, QSize size = {1001, 601}) {
        host = std::make_unique<QWidget>();
        host->resize(size);
        canvas = new WorksheetCanvas(host.get());
        canvas->setGeometry(QRect(QPoint(0, 0), size));
        canvas->setWorksheet(def, *w.context);
        host->show();
        host->activateWindow();
    }
    std::unique_ptr<QWidget> host;
    WorksheetCanvas* canvas = nullptr;
};

void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

int focusedCount(const WorksheetCanvas& c) {
    int n = 0;
    for (const ComponentWidget* w : c.components()) {
        n += w->isFocusedComponent() ? 1 : 0;
    }
    return n;
}

} // namespace

class TstWorksheetCanvas : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void placementAndPaintOrder() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        const QList<ComponentWidget*> all = c.components();
        QCOMPARE(all.size(), 3);
        // Paint order: ascending z (values z 0, graph z 1, the placeholder z 2).
        QCOMPARE(QString::fromLatin1(all[0]->type()), QStringLiteral("values_list"));
        QCOMPARE(QString::fromLatin1(all[1]->type()), QStringLiteral("time_distance"));
        QVERIFY(qobject_cast<PlaceholderComponent*>(all[2]) != nullptr);
        QCOMPARE(QString::fromLatin1(all[2]->type()), QStringLiteral("gauge_numeric"));
        // Fractional rectangles rounded like OffscreenRenderer: x0 = lround(l * w), ...
        const QRect content = c.contentsRect();
        QCOMPARE(content.size(), QSize(1001, 601));
        QCOMPARE(all[1]->geometry(), QRect(0, 0, 751, 601)); // lround(0.75 * 1001) = 751
        QCOMPARE(all[0]->geometry(), QRect(751, 0, 250, 601));
        QCOMPARE(all[2]->geometry(), QRect(501, 301, 500, 300)); // lround(500.5) = 501, lround(300.5) = 301
        for (ComponentWidget* cw : all) {
            QCOMPARE(cw->geometry(), WorksheetCanvas::placement(cw->spec().rect, content));
        }
        // The highest z paints on top where they overlap.
        QCOMPARE(c.childAt(QPoint(600, 400)), static_cast<QWidget*>(all[2]));
        QCOMPARE(c.childAt(QPoint(100, 100)), static_cast<QWidget*>(all[1]));
        QCOMPARE(c.componentById(Uuid::parse(kGraphId)), all[1]);
    }

    void focusRouting() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        QVERIFY(QTest::qWaitForWindowActive(rig.host.get()));
        ComponentWidget* values = c.componentById(Uuid::parse(kValuesId));
        ComponentWidget* graph = c.componentById(Uuid::parse(kGraphId));
        ComponentWidget* gauge = c.componentById(Uuid::parse(kGaugeId));
        // TAB order: z, then reading order.
        QCOMPARE(c.focusOrder(), (QList<ComponentWidget*>{values, graph, gauge}));
        // The first in TAB order has the focus after setWorksheet; exactly one focused component.
        QCOMPARE(c.focusedComponent(), values);
        QVERIFY(values->isFocusedComponent());
        QCOMPARE(focusedCount(c), 1);

        QSignalSpy changed(&c, &WorksheetCanvas::focusedComponentChanged);
        c.focusNext();
        QCOMPARE(c.focusedComponent(), graph);
        c.focusNext();
        QCOMPARE(c.focusedComponent(), gauge);
        c.focusNext(); // wraps
        QCOMPARE(c.focusedComponent(), values);
        c.focusNext(true); // SHIFT+TAB wraps back
        QCOMPARE(c.focusedComponent(), gauge);
        QCOMPARE(changed.size(), 4);
        QCOMPARE(focusedCount(c), 1);

        // A click focuses (and the graph's Chrome draws the focus border).
        auto* td = qobject_cast<TimeDistanceGraph*>(graph);
        QVERIFY(td != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(td->staticUpToDate(), 30000);
        const int chrome = td->chromeBuilds();
        sendMouse(graph, QEvent::MouseButtonPress, QPointF(300, 300), Qt::LeftButton, Qt::LeftButton);
        sendMouse(graph, QEvent::MouseButtonRelease, QPointF(300, 300), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(c.focusedComponent(), graph);
        QVERIFY(td->isFocusedComponent());
        QVERIFY(td->inputs().focused);
        QVERIFY(td->chromeBuilds() > chrome);
        QCOMPARE(focusedCount(c), 1);
        // The focused graph reports its trace width to the worksheet (cursor steps).
        QCOMPARE(w.view->widthPx(), td->layout().traceWidthPx);

        // TAB in a component cycles the components (the canvas owns TAB).
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(graph));
        QTest::keyClick(graph, Qt::Key_Tab);
        QCOMPARE(c.focusedComponent(), gauge);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(c.focusedComponent(), graph);
        // Keyboard focus moved into a component (the values list's table) focuses that component.
        qobject_cast<ValuesList*>(values)->table().setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(c.focusedComponent(), values);

        // ESC from a dock: returnFocus gives the keyboard focus back to the remembered component.
        auto* dockStandIn = new QWidget(rig.host.get());
        dockStandIn->setFocusPolicy(Qt::StrongFocus);
        dockStandIn->show();
        dockStandIn->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), dockStandIn);
        QCOMPARE(c.focusedComponent(), values); // the worksheet keeps its focused component
        c.returnFocus();
        QTRY_VERIFY(values->isAncestorOf(QApplication::focusWidget()) ||
                    QApplication::focusWidget() == values);

        // The focus is remembered per worksheet for the session.
        c.focusComponent(graph);
        project::WorksheetDef other = w.sheet;
        other.id = uuidRandom();
        other.name = "Other";
        other.components.push_back(
            timeDistanceSpec({{"Throttle"}}, project::Rect{0.0, 0.0, 1.0, 1.0}, 0, kSecondGraphId));
        const project::WorksheetDef first = c.worksheet();
        c.setWorksheet(other, *w.context);
        QCOMPARE(c.components().size(), 1);
        QCOMPARE(c.focusedComponent(), c.componentById(Uuid::parse(kSecondGraphId)));
        QTRY_COMPARE(QApplication::focusWidget(),
                     static_cast<QWidget*>(c.focusedComponent())); // was in the canvas
        // A worksheet switch while a dock has the keyboard focus leaves it there.
        dockStandIn->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), dockStandIn);
        c.setWorksheet(first, *w.context);
        QCOMPARE(c.focusedComponent(), c.componentById(Uuid::parse(kGraphId)));
        QVERIFY(c.focusedComponent()->isFocusedComponent());
        QCOMPARE(focusedCount(c), 1);
        QTest::qWait(20);
        QCOMPARE(QApplication::focusWidget(), dockStandIn);
    }

    void maximiseAndRestore() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        ComponentWidget* graph = c.componentById(Uuid::parse(kGraphId));
        ComponentWidget* values = c.componentById(Uuid::parse(kValuesId));
        const QRect normal = graph->geometry();
        // Double-click on the caption bar maximises (06 section 4.3 item 6).
        sendMouse(graph, QEvent::MouseButtonDblClick, QPointF(200, 6), Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(c.maximisedComponent(), graph);
        QCOMPARE(graph->geometry(), c.contentsRect());
        QVERIFY(!values->isVisible());
        QVERIFY(graph->isVisible());
        QCOMPARE(c.focusedComponent(), graph);
        // The maximised graph rebuilds at its new size.
        auto* td = qobject_cast<TimeDistanceGraph*>(graph);
        QTRY_VERIFY_WITH_TIMEOUT(
            td->staticUpToDate() && td->layers().logicalSize() == c.contentsRect().size(), 30000);
        // TAB stays on the maximised component.
        c.focusNext();
        QCOMPARE(c.focusedComponent(), graph);
        // Restore (ESC / the same action).
        QVERIFY(c.restoreMaximised());
        QVERIFY(!c.restoreMaximised());
        QCOMPARE(graph->geometry(), normal);
        QVERIFY(values->isVisible());
        c.toggleMaximise(values);
        QCOMPARE(c.maximisedComponent(), values);
        c.toggleMaximise(values);
        QCOMPARE(c.maximisedComponent(), nullptr);
        // A double-click below the caption does not maximise (it starts the graph's rubber band).
        sendMouse(graph, QEvent::MouseButtonDblClick, QPointF(300, 300), Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(c.maximisedComponent(), nullptr);
    }

    void editsApplyThroughTheCanvas() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        QSignalSpy edited(&c, &WorksheetCanvas::componentEdited);
        ComponentWidget* graph = c.componentById(Uuid::parse(kGraphId));
        graph->addChannel(QStringLiteral("Wheel Speed FR"), false);
        QCOMPARE(edited.size(), 1);
        QVERIFY(graph->channelNames().contains(QStringLiteral("Wheel Speed FR")));
        const project::ComponentSpec& stored = c.worksheet().components[0];
        QCOMPARE(stored.id, Uuid::parse(kGraphId));
        QCOMPARE(stored, graph->spec());
        QCOMPARE(edited.constLast().at(0).value<project::ComponentSpec>(), graph->spec());
        // The values list appends a row.
        ComponentWidget* values = c.componentById(Uuid::parse(kValuesId));
        QVERIFY(values->acceptsChannelDrop());
        values->addChannel(QStringLiteral("Wheel Speed FR"), false);
        QCOMPARE(edited.size(), 2);
        QCOMPARE(values->channelNames(),
                 (QStringList{QStringLiteral("Throttle"), QStringLiteral("Wheel Speed FL"),
                              QStringLiteral("Wheel Speed FR")}));
        QCOMPARE(c.worksheet().components[1], values->spec());
    }

    void relayoutCoalescedTo30Hz() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        rig.host->resize(1400, 900);
        // The burst below must find the throttle idle (a late show/resize pass restarts it, which a fixed
        // wait does not see under load): wait for that, then nothing runs the event loop until the burst is
        // over.
        QTRY_VERIFY(!c.layoutThrottleActive());
        const int passes = c.layoutPasses();
        for (int i = 1; i <= 15; ++i) {
            c.setGeometry(0, 0, 1001 + 20 * i, 601 + 10 * i); // synchronous resize events
        }
        QCOMPARE(c.layoutPasses(), passes + 1); // the leading edge
        QTRY_COMPARE(c.layoutPasses(), passes + 2);
        QTest::qWait(WorksheetCanvas::kLayoutCoalesceMs * 2);
        QCOMPARE(c.layoutPasses(), passes + 2);
        const QRect content = c.contentsRect();
        QCOMPARE(content.size(), QSize(1301, 751));
        for (ComponentWidget* cw : c.components()) {
            QCOMPARE(cw->geometry(), WorksheetCanvas::placement(cw->spec().rect, content));
        }
    }

    // D-058 (04 section 6.2): the components of a worksheet switched away from are kept hidden (the 30 s
    // Static release then applies) and shown again on a switch back; another definition under the same key
    // or another context rebuilds; clear() destroys every worksheet's components.
    void switchedAwayWorksheetsKeepTheirComponents() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        const project::WorksheetDef first = c.worksheet();
        QPointer<ComponentWidget> graph = c.componentById(Uuid::parse(kGraphId));
        auto* td = qobject_cast<TimeDistanceGraph*>(graph.data());
        QVERIFY(td != nullptr);
        // Everything that can still arrive (maths outputs, lap indexes) has arrived and the graph is quiet: a
        // rebuild after the flip could then only come from the flip itself.
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), 30000);
        int submitted = -1;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const bool quiet = td->staticUpToDate() && !td->layers().staticImage().isNull() &&
                                   !td->sizeThrottleActive() && !c.layoutThrottleActive() &&
                                   td->staticBuildsSubmitted() == submitted;
                submitted = td->staticBuildsSubmitted();
                QTest::qWait(100);
                return quiet;
            }(),
            30000);

        project::WorksheetDef other = w.sheet;
        other.id = uuidRandom();
        other.name = "Other";
        other.components = {
            timeDistanceSpec({{"Throttle"}}, project::Rect{0.0, 0.0, 1.0, 1.0}, 0, kSecondGraphId)};
        c.setWorksheet(other, *w.context);
        QCOMPARE(c.components().size(), 1);
        QCOMPARE(c.keptWorksheetCount(), 1);
        QVERIFY(!graph.isNull());                                   // kept
        QVERIFY(!graph->isVisible());                               // hidden
        QVERIFY(c.componentById(Uuid::parse(kGraphId)) == nullptr); // not part of the shown worksheet

        c.setWorksheet(first, *w.context);
        QCOMPARE(c.keptWorksheetCount(), 1);                            // "Other" is kept now
        QCOMPARE(c.componentById(Uuid::parse(kGraphId)), graph.data()); // the same widget
        QVERIFY(graph->isVisible());
        QVERIFY(!td->layers().staticImage().isNull());
        QTest::qWait(50);
        QCOMPARE(td->staticBuildsSubmitted(), submitted); // no rebuild for the flip
        QCOMPARE(focusedCount(c), 1);
        // Showing the shown worksheet again changes nothing.
        c.setWorksheet(first, *w.context);
        QCOMPARE(c.componentById(Uuid::parse(kGraphId)), graph.data());

        // Another definition under the kept key rebuilds that worksheet's components.
        project::WorksheetDef edited = other;
        edited.components.front().rect = project::Rect{0.0, 0.0, 0.5, 1.0};
        c.setWorksheet(edited, *w.context);
        QCOMPARE(c.components().size(), 1);
        QCOMPARE(c.components().front()->spec(), edited.components.front());

        c.clear();
        QVERIFY(graph.isNull());
        QCOMPARE(c.keptWorksheetCount(), 0);
        QVERIFY(c.components().isEmpty());
    }

    void schemeReachesComponents() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        Rig rig(w, sheetOf(w));
        WorksheetCanvas& c = *rig.canvas;
        auto* td = qobject_cast<TimeDistanceGraph*>(c.componentById(Uuid::parse(kGraphId)));
        QTRY_VERIFY_WITH_TIMEOUT(td->staticUpToDate(), 30000);
        const int delivered = td->staticBuildsDelivered();
        c.applyScheme(*shippedScheme("dark"));
        QTRY_VERIFY_WITH_TIMEOUT(td->staticUpToDate() && td->staticBuildsDelivered() > delivered, 30000);
        QCOMPARE(td->inputs().scheme->background, std::string("dark"));
    }
};

QTEST_MAIN(TstWorksheetCanvas)
#include "tst_worksheet_canvas.moc"
