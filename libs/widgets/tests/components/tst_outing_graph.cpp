// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OutingGraph (04 section 9; D-196 (d), D-198), offscreen: the whole outing is built once and a zoom or
// cursor change rebuilds only the Dynamic layer (the zoom rectangle, the cursor line, the key values); the
// zoom rectangle's handles (an end: setZoom; the bottom edge: a pan at constant span); a double-click on a
// lap of the range band zooms the worksheet to it; the wheel and the rubber band set the worksheet zoom (ESC
// cancels the band); a click places the cursor; enableOverlays widens the overlay policy; S, L and
// like-units grouping as spec edits; Export Data as CSV.

#include "components/plots_test_support.hpp"

#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/properties_shell/component_commands.hpp>

#include <QTest>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

constexpr const char* kBody = R"({"groups":[{"name":"Speed","traces":[{"channel":"Wheel Speed FL"}]},)"
                              R"({"name":"Throttle","traces":[{"channel":"Throttle"}]}]})";

// One logical pixel of the outing in worksheet units (the tolerance of a mouse position), measured inside the
// trace area (positions outside it clamp).
double pixelOf(const OutingGraph& g) {
    const double at = g.zoomRectLogical().center().x();
    const double a = *g.worksheetXAt(at);
    const double b = *g.worksheetXAt(at + 1.0);
    return std::abs(b - a);
}

} // namespace

class TstOutingGraph : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void theOutingIsStaticAndTheZoomRectangleDynamic() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1)); // Main lap 1: 40 s .. 90 s of a 217 s outing
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::MainOnly);
        QCOMPARE(rig.c->commandContext(), qt::CommandContext::Graph);
        QCOMPARE(rig.c->channelNames(),
                 (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Throttle")}));
        // The zoom rectangle spans the Main lap (worksheet x 0 .. 50 s).
        const QRectF z = rig.c->zoomRectLogical();
        QVERIFY(z.width() > 0.0);
        const double px = pixelOf(*rig.c);
        QVERIFY(std::abs(*rig.c->worksheetXAt(z.left()) - 0.0) <= 1.5 * px);
        QVERIFY(std::abs(*rig.c->worksheetXAt(z.right()) - 50.0e6) <= 1.5 * px);
        // A zoom and a cursor move rebuild only the Dynamic layer (04 section 7.4).
        const int submitted = rig.c->staticBuildsSubmitted();
        const int dynamic = rig.c->dynamicBuilds();
        w.view->zoomTo(plot::XSpan{-20.0e6, 60.0e6});
        w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic);
        QTest::qWait(50);
        QCOMPARE(rig.c->staticBuildsSubmitted(), submitted);
        QVERIFY(rig.c->dynamicBuilds() >= dynamic + 2);
        QVERIFY(std::abs(*rig.c->worksheetXAt(rig.c->zoomRectLogical().left()) + 20.0e6) <= 1.5 * px);
        // A selection change rebuilds the traces.
        w.selection.setMain(lapRef(*src, 2));
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->staticBuildsSubmitted() > submitted);
        // The handles' geometry is cached per inputs, zoom and domain (D-203 (ab)): it follows the new Main
        // and every later zoom.
        for (const plot::XSpan zoom : {w.view->state().zoom, plot::XSpan{5.0e6, 25.0e6}}) {
            w.view->zoomTo(zoom);
            const QRectF r = rig.c->zoomRectLogical();
            const double unit = pixelOf(*rig.c);
            QVERIFY(std::abs(*rig.c->worksheetXAt(r.left()) - w.view->state().zoom.a) <= 1.5 * unit);
            QVERIFY(std::abs(*rig.c->worksheetXAt(r.right()) - w.view->state().zoom.b) <= 1.5 * unit);
        }
    }

    void dragTheZoomRectangleEndsAndBottomEdge() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        const double px = pixelOf(*rig.c);
        QRectF z = rig.c->zoomRectLogical();
        const double midY = z.center().y();
        // The end handle.
        QCOMPARE(rig.c->hitAt(QPointF(z.right(), midY)).handleKind, plot::HandleKind::ZoomEnd);
        QCOMPARE(rig.c->hitAt(QPointF(z.left() + 2.0, midY)).handleKind, plot::HandleKind::ZoomStart);
        const double to = rig.c->logicalXOfWorksheet(70.0e6);
        sendMouse(rig.c, QEvent::MouseButtonPress, QPointF(z.right(), midY), Qt::LeftButton, Qt::LeftButton);
        sendMouse(rig.c, QEvent::MouseMove, QPointF(to, midY), Qt::NoButton, Qt::LeftButton);
        sendMouse(rig.c, QEvent::MouseButtonRelease, QPointF(to, midY), Qt::LeftButton, Qt::NoButton);
        QVERIFY(std::abs(w.view->state().zoom.a - 0.0) < 1.0);
        QVERIFY(std::abs(w.view->state().zoom.b - 70.0e6) <= 1.5 * px);
        // The bottom edge pans at constant span.
        const plot::XSpan before = w.view->state().zoom;
        z = rig.c->zoomRectLogical();
        const QPointF edge(z.center().x(), z.bottom());
        QCOMPARE(rig.c->hitAt(edge).handleKind, plot::HandleKind::ZoomPan);
        const double dx = rig.c->logicalXOfWorksheet(*rig.c->worksheetXAt(edge.x()) + 20.0e6) - edge.x();
        sendMouse(rig.c, QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
        sendMouse(rig.c, QEvent::MouseMove, edge + QPointF(dx, 0.0), Qt::NoButton, Qt::LeftButton);
        sendMouse(rig.c, QEvent::MouseButtonRelease, edge + QPointF(dx, 0.0), Qt::LeftButton, Qt::NoButton);
        const plot::XSpan after = w.view->state().zoom;
        QVERIFY(std::abs(after.length() - before.length()) < 1.0);
        QVERIFY(std::abs(after.a - (before.a + 20.0e6)) <= 1.5 * px);
        // The traces were never rebuilt by the drags.
        QCOMPARE(rig.c->staticBuildsDelivered(), rig.c->staticBuildsSubmitted());
    }

    void doubleClickALapZoomsToIt() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        // Find lap cell 2 of the range band (the band row lies under the caption).
        std::optional<QPointF> cell;
        for (int y = 18; y < 40 && !cell; ++y) {
            for (int x = 0; x < rig.c->width() && !cell; x += 2) {
                const plot::HitResult h = rig.c->hitAt(QPointF(x, y));
                if (h.kind == plot::HitKind::Band && h.band == 2) {
                    cell = QPointF(x, y);
                }
            }
        }
        QVERIFY(cell.has_value());
        doubleClick(rig.c, *cell);
        // Lap 2 runs 90 s .. 138 s: worksheet x from the Main start (40 s).
        QCOMPARE(w.view->state().zoom, (plot::XSpan{50.0e6, 98.0e6}));
        QVERIFY(rig.c->zoomToLap(0));
        QCOMPARE(w.view->state().zoom, (plot::XSpan{-40.0e6, 0.0}));
        QVERIFY(!rig.c->zoomToLap(99));
    }

    void wheelRubberBandAndClickActOnTheWorksheet() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        const double px = pixelOf(*rig.c);
        const QRectF z = rig.c->zoomRectLogical();
        // A click in a panel (inside the zoom rectangle, away from its handles) places the cursor.
        const QPointF p(rig.c->logicalXOfWorksheet(20.0e6), z.top() + 0.3 * z.height());
        QCOMPARE(rig.c->hitAt(p).kind, plot::HitKind::Panel);
        click(rig.c, p);
        QVERIFY(std::abs(static_cast<double>(w.view->state().cursorT) - 20.0e6) <= 1.5 * px);
        // The wheel: the worksheet zoom x0.8 about the pointer.
        const plot::XSpan before = w.view->state().zoom;
        const double at = *rig.c->worksheetXAt(p.x());
        sendWheel(rig.c, p, 120);
        const plot::XSpan after = w.view->state().zoom;
        QVERIFY(std::abs(after.length() - 0.8 * before.length()) < 1.0);
        QVERIFY(std::abs((at - after.a) - 0.8 * (at - before.a)) < 1.0);
        // The rubber band: ESC cancels, double-click, move, click zooms.
        const QPointF a(rig.c->logicalXOfWorksheet(-30.0e6), z.top() + 0.3 * z.height());
        const QPointF b(rig.c->logicalXOfWorksheet(10.0e6), z.top() + 0.3 * z.height());
        doubleClick(rig.c, a);
        QVERIFY(rig.c->rubberBandActive());
        sendKey(rig.c, Qt::Key_Escape);
        QVERIFY(!rig.c->rubberBandActive());
        QCOMPARE(w.view->state().zoom, after);
        doubleClick(rig.c, a);
        sendMouse(rig.c, QEvent::MouseMove, b, Qt::NoButton, Qt::NoButton);
        click(rig.c, b);
        QVERIFY(!rig.c->rubberBandActive());
        QVERIFY(std::abs(w.view->state().zoom.a + 30.0e6) <= 1.5 * px);
        QVERIFY(std::abs(w.view->state().zoom.b - 10.0e6) <= 1.5 * px);
    }

    void togglesGroupingAndOverlays() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->activeChannel().value_or(QString()), QStringLiteral("Wheel Speed FL"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_style"), {}));
        QCOMPARE(rig.c->body().graph.traceStyle, std::string("points"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.legend"), {}));
        QCOMPARE(rig.c->body().graph.keyMode, std::string("none"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.legend"), {}));
        QCOMPARE(rig.c->body().graph.keyMode, std::string("labels"));
        // The programmatic forms of the wheel and the zoom rectangle (D-203 (w)): the worksheet zoom.
        QVERIFY(rig.c->handleCommand(QStringLiteral("zoom.outing_rect"), {5.0e6, 15.0e6}));
        QCOMPARE(w.view->state().zoom, (plot::XSpan{5.0e6, 15.0e6}));
        QVERIFY(!rig.c->handleCommand(QStringLiteral("zoom.outing_rect"), {}));
        QVERIFY(rig.c->handleCommand(QStringLiteral("zoom.wheel"), {1}));
        QVERIFY(std::abs(w.view->state().zoom.length() - 0.8 * 10.0e6) < 1.0);
        // G, Y, E and SHIFT+E are the time/distance graph's: the outing graph has none of them (D-203 (h)).
        QVERIFY(!rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QVERIFY(!hasGenericComponentEdit(QStringLiteral("outing"), QStringLiteral("component.cycle_layout")));
        QVERIFY(!hasGenericComponentEdit(QStringLiteral("outing"), QStringLiteral("cursor.style")));
        QVERIFY(!hasGenericComponentEdit(QStringLiteral("outing"), QStringLiteral("graph.status_errors")));
        QVERIFY(!hasGenericComponentEdit(QStringLiteral("track_position"),
                                         QStringLiteral("component.colour_legend")));
        // Like-units grouping: a speed joins the Speed group; CTRL makes a new group.
        rig.c->addChannel(QStringLiteral("Wheel Speed FR"), false);
        QCOMPARE(rig.c->body().graph.groups.size(), std::size_t{2});
        QCOMPARE(rig.c->body().graph.groups[0].traces.size(), std::size_t{2});
        rig.c->addChannel(QStringLiteral("Wheel Speed FR"), false); // already there: no edit
        rig.c->addChannel(QStringLiteral("Throttle"), true);
        QCOMPARE(rig.c->body().graph.groups.size(), std::size_t{3});
        QCOMPARE(rig.edits, 5);
        // enableOverlays.
        project::ComponentSpec spec = rig.c->spec();
        plot::OutingBody body = rig.c->body();
        body.enableOverlays = true;
        spec.body = body.toJson();
        rig.c->applySpec(spec);
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
    }

    void exportsTheChannelsAsCsv() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<OutingGraph> rig(w, specOf("outing", kBody), QSize(600, 260));
        QVERIFY(waitQuiet(*rig.c));
        MemorySink sink;
        QVERIFY(rig.c->exportData(sink, ExportOptions{}));
        const QList<QByteArray> lines = csvLines(sink.bytes);
        QCOMPARE(lines.front(), QByteArray("Time [s],\"Wheel Speed FL [m/s]\",\"Throttle [%]\""));
        QCOMPARE(lines.size(), 1 + 50 * 50);
        const SourceShape shape;
        const QList<QByteArray> first = lines[1].split(',');
        QCOMPARE(first[0].toDouble(), 40.0);
        QVERIFY(std::abs(first[1].toDouble() - shape.wheelFl(2000)) < 1e-9);
        QVERIFY(std::abs(first[2].toDouble() - shape.throttle(2000)) < 1e-9);
    }
};

QTEST_MAIN(TstOutingGraph)
#include "tst_outing_graph.moc"
