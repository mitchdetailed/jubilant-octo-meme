// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterPlot (04 section 10; D-196, D-198), offscreen: the Static build through LayeredComponent and
// ScatterGeometry (points of the visible span; a zoom rebuilds them, a cursor move only the Dynamic layer);
// overlays drawn beneath Main in their colours; a click on a point moves the cursor to that point's Main-lap
// time; LEFT/RIGHT step one Main point; G, S, Y, K and L as spec edits; the trend equation (pointsTrend) and
// Copy Equation; the box zoom and the wheel on the component's own axes, ESC and the axis double-click; the
// colour legend with a colour channel; Export Data as CSV; renderToImage. Expected values come from the
// synthetic sources' formulas (plots_test_support.hpp).

#include "components/plots_test_support.hpp"

#include <esses/store/column.hpp>
#include <esses/widgets/components/scatter_plot.hpp>

#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

constexpr const char* kBody =
    R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"}],"cursorStyle":"ring"})";

// Fills the SummaryPyramids of a source (what the loader does after an import), so the channel scales are
// those of the whole column from the first build on.
void buildSummaries(const model::DataSource& src) {
    for (const model::DataLayer& layer : src.layers) {
        for (const model::Channel& ch : layer.channels) {
            QVERIFY(store::seriesValues(ch.series).buildSummary(CancelToken{}).ok());
        }
    }
}

// The widget-logical position of a data point on tile 0's axes.
QPointF logicalOf(const ScatterPlot& s, double x, double y) {
    const std::vector<plot::ScatterAxes> axes = s.axes();
    const plot::ScatterAxes& a = axes.front();
    const double dpr = static_cast<double>(s.inputs().layout.dpr);
    const double px =
        static_cast<double>(a.plot.x) + (x - a.xMin) / (a.xMax - a.xMin) * static_cast<double>(a.plot.w);
    const double py =
        static_cast<double>(a.plot.y) + (a.yMax - y) / (a.yMax - a.yMin) * static_cast<double>(a.plot.h);
    return {px / dpr, py / dpr};
}

} // namespace

class TstScatterPlot : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void buildsPointsAndRebuildsOnlyTheDynamicLayerOnACursorMove() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(w, specOf("scatter", kBody));
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->staticBuildsDelivered() >= 1);
        QCOMPARE(rig.c->channelNames(),
                 (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Throttle")}));
        QCOMPARE(rig.c->activeChannel().value_or(QString()), QStringLiteral("Throttle"));
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
        QCOMPARE(rig.c->commandContext(), qt::CommandContext::Summary);
        QVERIFY(!rig.c->hits().empty());
        const std::vector<plot::ScatterAxes> axes = rig.c->axes();
        QCOMPARE(axes.size(), std::size_t{1});
        QVERIFY(std::isfinite(axes[0].xMin) && axes[0].xMax > axes[0].xMin);

        const int submitted = rig.c->staticBuildsSubmitted();
        const int dynamic = rig.c->dynamicBuilds();
        w.view->setCursor(12 * kSec, qt::CursorSource::Programmatic);
        QCOMPARE(rig.c->staticBuildsSubmitted(), submitted); // 04 section 2.2: Cursor -> Dynamic only
        QVERIFY(rig.c->dynamicBuilds() > dynamic);
        QVERIFY(!rig.c->lastDynamicDamage().isEmpty());

        // The worksheet zoom selects the data span: a zoom rebuilds the points.
        w.view->zoomTo(plot::XSpan{0.0, 20.0e6});
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->staticBuildsSubmitted() > submitted);
        QVERIFY(!rig.c->renderToImage(rig.c->devicePixelRatioF()).isNull());
    }

    void overlaysAreDrawnBeneathMainInTheirColours() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        w.selection.toggleOverlay0(lapRef(*src, 2)); // Overlay 0: its own role colour (D-017)
        Rig<ScatterPlot> rig(w, specOf("scatter", kBody));
        plot::DrawList last;
        rig.c->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        // Points commands in draw order: the overlay's first, Main's last.
        std::vector<project::Rgba> order;
        for (const plot::DrawCmd& c : last.cmds) {
            if (c.kind == plot::CmdKind::Points && (order.empty() || order.back() != c.colour)) {
                order.push_back(c.colour);
            }
        }
        QVERIFY(order.size() >= 2);
        const plot::Slot mainSlot = w.selection.snapshot().visibleSlots().front();
        const plot::Slot overlaySlot = w.selection.snapshot().visibleSlots().back();
        const project::Scheme& scheme = *w.scheme;
        const int index = rig.c->inputs().slots.front().channels[1].descriptor->colourIndex;
        QCOMPARE(order.back(),
                 plot::traceColour(scheme, w.selection.snapshot(), mainSlot, std::max(index, 0)));
        QCOMPARE(order.front(),
                 plot::traceColour(scheme, w.selection.snapshot(), overlaySlot, std::max(index, 0)));
    }

    void aClickMovesTheCursorToTheNearestMainPoint() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1)); // the 50 s lap starting at 40 s
        Rig<ScatterPlot> rig(w, specOf("scatter", kBody));
        QVERIFY(waitQuiet(*rig.c));
        const SourceShape shape;
        const std::size_t i = 50 * 50; // 10 s into the lap
        const QPointF p = logicalOf(*rig.c, shape.wheelFl(i), shape.throttle(i));
        QCOMPARE(rig.c->hitAt(p).kind, plot::HitKind::Panel);
        click(rig.c, p);
        QCOMPARE(w.view->state().cursorT, 10 * kSec);
        // Far from every point (a corner of the plot area): nothing moves.
        const plot::ScatterAxes a = rig.c->axes().front();
        const double dpr = static_cast<double>(rig.c->inputs().layout.dpr);
        QVERIFY(!rig.c->pickPoint(QPointF((static_cast<double>(a.plot.x) + 1.0) / dpr,
                                          (static_cast<double>(a.plot.y) + 1.0) / dpr)));
        QCOMPARE(w.view->state().cursorT, 10 * kSec);
        // LEFT / RIGHT (Summary context): one Main point (one 50 Hz X sample); CTRL: ten.
        QVERIFY(rig.c->handleCommand(QStringLiteral("cursor.right"), {}));
        QCOMPARE(w.view->state().cursorT, 10 * kSec + 20'000);
        QVERIFY(rig.c->handleCommand(QStringLiteral("cursor.left_fast"), {}));
        QCOMPARE(w.view->state().cursorT, 10 * kSec - 180'000);
    }

    void cyclesAndTogglesAreSpecEdits() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(w, specOf("scatter",
                                       R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"},)"
                                       R"({"yChannel":"Wheel Speed FR"}]})"));
        QVERIFY(waitQuiet(*rig.c));
        // G: overlapped -> tiled -> average -> overlapped.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QCOMPARE(rig.c->body().layout, std::string("tiled"));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->axes().size(), std::size_t{2}); // one tile per trace
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QCOMPARE(rig.c->body().layout, std::string("average"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QCOMPARE(rig.c->body().layout, std::string("overlapped"));
        // S: points -> pointsTrend -> trend -> points.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_style"), {}));
        QCOMPARE(rig.c->body().style, std::string("pointsTrend"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_style"), {}));
        QCOMPARE(rig.c->body().style, std::string("trend"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_style"), {}));
        QCOMPARE(rig.c->body().style, std::string("points"));
        // Y: cross -> ring -> none -> cross.
        QVERIFY(rig.c->handleCommand(QStringLiteral("cursor.style"), {}));
        QCOMPARE(rig.c->body().cursorStyle, std::string("ring"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("cursor.style"), {}));
        QCOMPARE(rig.c->body().cursorStyle, std::string("none"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("cursor.style"), {}));
        QCOMPARE(rig.c->body().cursorStyle, std::string("cross"));
        // K and L: the header's legends.
        QVERIFY(!rig.c->spec().legend.colour);
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.colour_legend"), {}));
        QVERIFY(rig.c->spec().legend.colour);
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.legend"), {}));
        QVERIFY(!rig.c->spec().legend.channels);
        QCOMPARE(rig.edits, 11);
        QVERIFY(!rig.lastUndoText.isEmpty());
        // ALT+n is not the scatter's (a BandAxis has no per-band visibility, D-198): the shell's turn.
        QVERIFY(!rig.c->handleCommand(QStringLiteral("component.colour_band"), {QVariant(0)}));
        QCOMPARE(rig.edits, 11);
        // addChannel: a Y trace, CTRL: the X channel.
        rig.c->addChannel(QStringLiteral("Corr Speed"), false);
        QCOMPARE(rig.c->body().traces.size(), std::size_t{3});
        rig.c->addChannel(QStringLiteral("Wheel Speed FR"), true);
        QCOMPARE(rig.c->body().xChannel, std::string("Wheel Speed FR"));
    }

    void trendEquationIsShownAndCopied() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(w, specOf("scatter",
                                       R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"}],)"
                                       R"("style":"pointsTrend","trend":{"type":"poly","order":3}})"));
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->equationText().startsWith(QStringLiteral("y = ")));
        QVERIFY(rig.c->equationText().contains(QStringLiteral("x^3")));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.copy_equation"), {}));
        QCOMPARE(QGuiApplication::clipboard()->text(), rig.c->equationText());
        // Points only: no equation, nothing to copy.
        rig.c->cycleStyle(); // trend
        rig.c->cycleStyle(); // points
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->equationText().isEmpty());
        QVERIFY(!rig.c->copyEquation());
    }

    void boxZoomWheelAndAxisRestoreOnTheOwnAxes() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(w, specOf("scatter", kBody));
        QVERIFY(waitQuiet(*rig.c));
        const plot::ScatterAxes full = rig.c->axes().front();
        const double x0 = full.xMin + 0.25 * (full.xMax - full.xMin);
        const double x1 = full.xMin + 0.75 * (full.xMax - full.xMin);
        const double y0 = full.yMin + 0.20 * (full.yMax - full.yMin);
        const double y1 = full.yMin + 0.60 * (full.yMax - full.yMin);
        const plot::XSpan worksheetZoom = w.view->state().zoom;
        // ESC drops a box in progress.
        doubleClick(rig.c, logicalOf(*rig.c, x0, y1));
        QVERIFY(rig.c->boxZoomActive());
        sendKey(rig.c, Qt::Key_Escape);
        QVERIFY(!rig.c->boxZoomActive());
        QVERIFY(!rig.c->axisZoom().has_value());
        // Double-click, move, click.
        doubleClick(rig.c, logicalOf(*rig.c, x0, y1));
        sendMouse(rig.c, QEvent::MouseMove, logicalOf(*rig.c, x1, y0), Qt::NoButton, Qt::NoButton);
        QVERIFY(countCmds(rig.c->layers().dynamicList(), plot::CmdKind::FilledRect) >= 1); // the box
        click(rig.c, logicalOf(*rig.c, x1, y0));
        QVERIFY(rig.c->axisZoom().has_value());
        const double tolX = (full.xMax - full.xMin) / 400.0; // a device pixel or so
        const double tolY = (full.yMax - full.yMin) / 200.0;
        QVERIFY(std::abs(rig.c->axisZoom()->xMin - x0) < tolX);
        QVERIFY(std::abs(rig.c->axisZoom()->xMax - x1) < tolX);
        QVERIFY(std::abs(rig.c->axisZoom()->yMin - y0) < tolY);
        QVERIFY(std::abs(rig.c->axisZoom()->yMax - y1) < tolY);
        QVERIFY(waitQuiet(*rig.c));
        const plot::ScatterAxes zoomed = rig.c->axes().front(); // manual scales from the zoom
        QCOMPARE(zoomed.xMin, rig.c->axisZoom()->xMin);
        QCOMPARE(zoomed.yMax, rig.c->axisZoom()->yMax);
        QCOMPARE(w.view->state().zoom, worksheetZoom); // not the worksheet span (04 section 10.1 item 8)
        QCOMPARE(rig.edits, 0);                        // view state, not a spec edit
        // The wheel: x0.8 per notch about the pointer.
        const double cx = 0.5 * (zoomed.xMin + zoomed.xMax);
        const double cy = 0.5 * (zoomed.yMin + zoomed.yMax);
        sendWheel(rig.c, logicalOf(*rig.c, cx, cy), 120);
        QVERIFY(waitQuiet(*rig.c));
        const ScatterPlot::AxisZoom z = *rig.c->axisZoom();
        QVERIFY(std::abs((z.xMax - z.xMin) - 0.8 * (zoomed.xMax - zoomed.xMin)) < 2.0 * tolX);
        QVERIFY(std::abs((z.yMax - z.yMin) - 0.8 * (zoomed.yMax - zoomed.yMin)) < 2.0 * tolY);
        // A double-click on an axis restores.
        const plot::ScatterAxes now = rig.c->axes().front();
        const double dpr = static_cast<double>(rig.c->inputs().layout.dpr);
        const QPointF xAxis((static_cast<double>(now.plot.x) + 0.5 * static_cast<double>(now.plot.w)) / dpr,
                            (static_cast<double>(now.plot.y + now.plot.h) + 4.0 * dpr) / dpr);
        QCOMPARE(rig.c->hitAt(xAxis).kind, plot::HitKind::Axis);
        doubleClick(rig.c, xAxis);
        QVERIFY(!rig.c->axisZoom().has_value());
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->axes().front().xMin, full.xMin);
    }

    void colourChannelShowsItsLegend() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        project::ComponentSpec spec =
            specOf("scatter",
                   R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"}],)"
                   R"("colourChannel":{"channel":"Wheel Speed FR","axis":{"mode":"even","bandMode":"count",)"
                   R"("bandCount":5,"palette":"rainbow"}}})");
        spec.legend.colour = true;
        Rig<ScatterPlot> rig(w, spec);
        plot::DrawList last;
        rig.c->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->channelNames().size(), 3);
        QVERIFY(textsOf(last, plot::TextStyleId::ColourBand).size() >= 5); // a lower limit per band
        rig.c->toggleColourLegend();
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(textsOf(last, plot::TextStyleId::ColourBand).empty());
    }

    void intervalAverageAndGating() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(
            w, specOf("scatter", R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"}],)"
                                 R"("intervalAverage":{"enabled":true,"binWidth":{"v":2,"unit":"m/s"}}})"));
        plot::DrawList last;
        rig.c->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        // The interval average: one stepped line (two vertices per non-empty 2 m/s bin), twice the line
        // width.
        const float stepWidth = 2.0F * std::max(1.0F, std::round(last.dpr));
        int stepped = 0;
        for (const plot::DrawCmd& c : last.cmds) {
            if (c.kind == plot::CmdKind::Polyline && c.widthPx == stepWidth && c.vertexCount >= 4 &&
                c.vertexCount % 2 == 0) {
                ++stepped;
            }
        }
        QCOMPARE(stepped, 1);
        // Gating: the throttle is exactly 0 % half way through the lap (sample 3250) and above 0 elsewhere,
        // so gating by the throttle drops exactly that point.
        MemorySink ungated;
        QVERIFY(rig.c->exportData(ungated, ExportOptions{}));
        QCOMPARE(csvLines(ungated.bytes).size(), 1 + 2500);
        QCOMPARE(SourceShape{}.throttle(3250), 0.0);
        plot::ScatterBody body = rig.c->body();
        body.gatingChannel = "Throttle";
        project::ComponentSpec spec = rig.c->spec();
        spec.body = body.toJson();
        rig.c->applySpec(spec);
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->channelNames().size(), 2); // the gating channel is the Y channel too
        MemorySink gated;
        QVERIFY(rig.c->exportData(gated, ExportOptions{}));
        QCOMPARE(csvLines(gated.bytes).size(), 1 + 2499);
    }

    void exportsEveryPointAsCsv() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<ScatterPlot> rig(w, specOf("scatter", kBody));
        QVERIFY(waitQuiet(*rig.c));
        MemorySink sink;
        QVERIFY(rig.c->exportData(sink, ExportOptions{}));
        const QList<QByteArray> lines = csvLines(sink.bytes);
        QCOMPARE(lines.front(), QByteArray("set,x,y"));
        QCOMPARE(lines.size(), 1 + 50 * 50); // every 50 Hz sample of the 50 s lap (the zoom)
        const SourceShape shape;
        const QList<QByteArray> first = lines[1].split(',');
        QCOMPARE(first[0], QByteArray("\"Throttle@Main\""));
        QVERIFY(std::abs(first[1].toDouble() - shape.wheelFl(40 * 50)) < 1e-9);
        QVERIFY(std::abs(first[2].toDouble() - shape.throttle(40 * 50)) < 1e-9);
        // All data: the whole outing.
        MemorySink all;
        ExportOptions o;
        o.range = ExportOptions::Range::AllData;
        QVERIFY(rig.c->exportData(all, o));
        QCOMPARE(csvLines(all.bytes).size(), 1 + 217 * 50);
    }
};

QTEST_MAIN(TstScatterPlot)
#include "tst_scatter_plot.moc"
