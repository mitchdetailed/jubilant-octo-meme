// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The time_distance completion in the widget (04 section 8, parity-first section 5.4; 07 section 6.1),
// offscreen: the graph's commands as undoable body edits (F3, M, E, SHIFT+E, Y, G, S) and what they show; the
// datum's appearance rebuilding the key (moving it stays Dynamic); F3 building no VarianceTable without an
// overlay (AC-M3-05) and one per overlay with them; the measurement hover line and its double-click; band
// double-clicks zooming to a range or a section without moving the cursor; the scrollbar's thumb drag, track
// paging and double-click; the vertical zoom and pan of the active group (keys, rubber band, scrollbar);
// overlapped mode's Lo/Hi handles and the panel splitter as group edits; the offset sub-axis drag moving the
// traces while pending and committing one batch at the release (AC-M3-09); the offset step of a slot (one
// sample period of the active channel in that slot's source, 0.25 m in distance); copy of the cursor values;
// Export Data; the tooltips of markers, annotations and status rows. The sidecar commands (annotations, the
// offsets' undo) run in the shell: tests/ui/tst_offset_axis.

#include "components/component_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;
constexpr int kTimeoutMs = 30'000;

void selectMain(World& w, const model::RangeRef& ref) {
    w.selection.setMain(ref);
    w.view->zoomToMainRange();
    w.view->setCursor(0, qt::CursorSource::Programmatic);
}

// One graph in a plain host widget whose spec edits are applied at once (the canvas's part).
struct Rig {
    explicit Rig(World& world, const project::ComponentSpec& spec, QSize size = {900, 480}) {
        host = std::make_unique<QWidget>();
        host->resize(size);
        graph = new TimeDistanceGraph(spec, *world.context, host.get());
        graph->setGeometry(QRect(QPoint(0, 0), size));
        QObject::connect(graph, &ComponentWidget::specEditRequested, graph,
                         [this](const project::ComponentSpec& next, const QString& text) {
                             ++edits;
                             lastText = text;
                             graph->applySpec(next);
                         });
        host->show();
    }
    std::unique_ptr<QWidget> host;
    TimeDistanceGraph* graph = nullptr;
    int edits = 0;
    QString lastText;
};

bool waitQuiet(TimeDistanceGraph& g, int quietMs = 150) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kTimeoutMs) {
        if (!QTest::qWaitFor([&g] { return g.staticUpToDate(); }, kTimeoutMs)) {
            return false;
        }
        const int submitted = g.staticBuildsSubmitted();
        QTest::qWait(quietMs);
        if (g.staticUpToDate() && g.staticBuildsSubmitted() == submitted) {
            return true;
        }
    }
    return false;
}

void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}

void click(QWidget* w, QPointF pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

void doubleClick(QWidget* w, QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, mods);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
    sendMouse(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton, mods);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
}

void drag(QWidget* w, QPointF from, QPointF to) {
    sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    const QPointF mid = 0.5 * (from + to);
    sendMouse(w, QEvent::MouseMove, mid, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

double wide(float v) {
    return static_cast<double>(v);
}

// Logical point at fractions (fx, fy) of a device-px rectangle.
QPointF at(const plot::RectPx& r, double fx, double fy, float dpr) {
    return {(wide(r.x) + fx * wide(r.w)) / wide(dpr), (wide(r.y) + fy * wide(r.h)) / wide(dpr)};
}

QJsonObject bodyOf(const project::ComponentSpec& spec) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
}

// A spec with the M3 elements the test needs (bands and scrollbars stay default on).
project::ComponentSpec graphSpec(const std::vector<std::vector<std::string>>& groups,
                                 const QJsonObject& displayExtra = {}) {
    project::ComponentSpec spec = timeDistanceSpec(groups);
    QJsonObject body = bodyOf(spec);
    QJsonObject display = body.value(QStringLiteral("display")).toObject();
    for (auto it = displayExtra.begin(); it != displayExtra.end(); ++it) {
        display.insert(it.key(), it.value());
    }
    body.insert(QStringLiteral("display"), display);
    spec.body =
        model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString()).value();
    spec.activeChannel = groups.front().front();
    return spec;
}

class Sink final : public ExportSink {
public:
    bool write(const QByteArray& bytes) override {
        data += bytes;
        return true;
    }
    QByteArray data;
};

} // namespace

class TstTimeDistanceM3 : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    // F3, M, E, SHIFT+E, Y, G, S: undoable body edits through handleCommand, each shown at once.
    void graphCommandsEditTheBody() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        w.selection.toggleOverlay0(lapRef(*a, 2));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
        QVERIFY(waitQuiet(g));
        const auto toggled = [&](const char* id, auto check) {
            const int before = rig.edits;
            QVERIFY(g.handleCommand(QString::fromLatin1(id), {}));
            QCOMPARE(rig.edits, before + 1);
            QVERIFY(waitQuiet(g));
            check();
        };
        toggled("graph.variance", [&] {
            QVERIFY(g.body().variance.show);
            QVERIFY(g.layout().variance.h > 0.0F);
            QCOMPARE(g.staticInfo().varianceTables, 1); // one per visible overlay
        });
        toggled("graph.measurements", [&] {
            QVERIFY(g.body().keyStats);
            QVERIFY(g.layout().keyColumns.measurements);
            QCOMPARE(g.staticInfo().measurements.size(), std::size_t{4}); // two traces x two slots
        });
        toggled("graph.status_errors", [&] { QVERIFY(g.body().showStatusStrip); });
        toggled("graph.device_events", [&] { QVERIFY(!g.body().showDeviceEvents); });
        toggled("cursor.style", [&] { QCOMPARE(g.body().cursorStyle, std::string("crosshair")); });
        toggled("cursor.style", [&] { QCOMPARE(g.body().cursorStyle, std::string("none")); });
        toggled("cursor.style", [&] { QCOMPARE(g.body().cursorStyle, std::string("line")); });
        toggled("component.cycle_layout", [&] {
            QVERIFY(g.body().overlapped());
            QVERIFY(g.layout().overlapped);
            QVERIFY(g.layout().groups[0].loHandle.w > 0.0F);
        });
        toggled("component.cycle_layout", [&] { QVERIFY(!g.layout().overlapped); });
        toggled("component.cycle_style", [&] { QCOMPARE(g.body().traceStyle, std::string("points")); });
        // The edits persist as body members (04 section 8.14).
        const QJsonObject display = bodyOf(g.spec()).value(QStringLiteral("display")).toObject();
        QCOMPARE(display.value(QStringLiteral("variance")).toObject().value(QStringLiteral("show")).toBool(),
                 true);
        QCOMPARE(display.value(QStringLiteral("key")).toObject().value(QStringLiteral("stats")).toBool(),
                 true);
        QCOMPARE(display.value(QStringLiteral("showDeviceEvents")).toBool(true), false);
        QVERIFY(!g.handleCommand(QStringLiteral("no.such_command"), {}));
    }

    // AC-M3-05's widget half: F3 without an overlay builds no VarianceTable and shows no panel.
    void varianceNeedsAnOverlay() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}},
                             {{QStringLiteral("variance"), QJsonObject{{QStringLiteral("show"), true}}}}));
        TimeDistanceGraph& g = *rig.graph;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.staticInfo().varianceTables, 0);
        QCOMPARE(g.layout().variance.h, 0.0F);
        w.selection.toggleOverlay(lapRef(*a, 2));
        w.selection.toggleOverlay(lapRef(*a, 3));
        QVERIFY(waitQuiet(g));
        QVERIFY(g.layout().variance.h > 0.0F);
        QCOMPARE(g.staticInfo().varianceTables, 2);
        QVERIFY(!g.staticInfo().varianceNoData);
    }

    // Placing the datum changes the key (value@datum, delta): one Static rebuild; moving it is Dynamic only.
    void datumAppearsInTheKey() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        QVERIFY(!g.layout().keyColumns.datum);
        const int submitted = g.staticBuildsSubmitted();
        w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic);
        w.view->placeDatum();
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.staticBuildsSubmitted(), submitted + 1);
        QVERIFY(g.layout().keyColumns.datum);
        const int after = g.staticBuildsSubmitted();
        w.view->setCursor(20 * kSec, qt::CursorSource::Programmatic);
        w.view->placeDatum(); // moved: Dynamic only
        QTest::qWait(60);
        QCOMPARE(g.staticBuildsSubmitted(), after);
        w.view->toggleDatum(); // hidden: the key loses the columns
        QVERIFY(waitQuiet(g));
        QVERIFY(!g.layout().keyColumns.datum);
    }

    // Hovering a max cell draws its position line; a double-click moves the cursor there (04 section 8.4).
    void measurementHoverAndDoubleClick() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Throttle"}},
                             {{QStringLiteral("key"), QJsonObject{{QStringLiteral("stats"), true}}}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        QVERIFY(g.hasMouseTracking());
        QCOMPARE(g.staticInfo().measurements.size(), std::size_t{1});
        const plot::TdMeasurementEntry m = g.staticInfo().measurements.front();
        QVERIFY(m.valid);
        const plot::TdGroupLayout& gl = g.layout().groups.front();
        const plot::RectPx cell = gl.keyGeometry.cellRect(0, 0, plot::KeyCell::Max);
        const float dpr = g.layout().dpr;
        const QPointF p((wide(gl.key.x + cell.x) + 0.5 * wide(cell.w)) / wide(dpr),
                        (wide(gl.key.y + cell.y) + 0.5 * wide(cell.h)) / wide(dpr));
        QCOMPARE(g.hitAt(p).kind, plot::HitKind::KeyMeasurement);
        sendMouse(&g, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        QVERIFY(g.hoverLineX().has_value());
        QVERIFY(std::abs(wide(*g.hoverLineX()) / wide(dpr) - g.logicalXOf(m.maxX)) < 1.0);
        // The throttle's maximum: the cursor jumps to that sample.
        sendMouse(&g, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(lapAt(*a, 1).start + w.view->state().cursorT, m.maxT);
        // Leaving the widget removes the line.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&g, &leave);
        QVERIFY(!g.hoverLineX().has_value());
    }

    // Range and section band entries: a double-click zooms to the entry, a click never moves the cursor.
    void bandDoubleClickZooms() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 2));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}}));
        TimeDistanceGraph& g = *rig.graph;
        w.view->zoomFull(); // the whole outing: the neighbouring laps in the range band
        QVERIFY(waitQuiet(g));
        const plot::TdLayout& L = g.layout();
        QVERIFY(L.bands.range.h > 0.0F);
        QVERIFY(L.bands.marker.h > 0.0F);
        QVERIFY(g.staticInfo().ranges.size() >= 5); // four other laps and the Main slot
        const float dpr = L.dpr;
        // The lap before Main (another range of its group, left of it).
        const auto lap2 =
            std::find_if(g.staticInfo().ranges.begin(), g.staticInfo().ranges.end(),
                         [](const plot::TdBandEntry& e) { return e.slot < 0 && e.x1 <= 1.0 && e.x1 > -1.0; });
        QVERIFY(lap2 != g.staticInfo().ranges.end());
        const QPointF inLap2(g.logicalXOf(0.5 * (lap2->x0 + lap2->x1)),
                             (wide(L.bands.range.y) + 0.5 * wide(L.bands.range.h)) / wide(dpr));
        const Micros cursor = w.view->state().cursorT;
        click(&g, inLap2);
        QCOMPARE(w.view->state().cursorT, cursor); // bands never move the cursor
        doubleClick(&g, inLap2);
        QVERIFY(std::abs(w.view->state().zoom.a - lap2->x0) < 1.0);
        QVERIFY(std::abs(w.view->state().zoom.b - lap2->x1) < 1.0);
        // Beacons in the marker band name themselves.
        QVERIFY(waitQuiet(g));
        const auto bcn =
            std::find_if(g.staticInfo().markers.begin(), g.staticInfo().markers.end(),
                         [](const plot::TdMarkerEntry& m) { return m.kind == plot::BandMarkerKind::Beacon; });
        QVERIFY(bcn != g.staticInfo().markers.end());
        const QPointF onBeacon(g.logicalXOf(bcn->x),
                               (wide(L.bands.marker.y) + 0.5 * wide(L.bands.marker.h)) / wide(dpr));
        QVERIFY(g.toolTipAt(onBeacon).startsWith(QStringLiteral("BCN")));
    }

    // The horizontal scrollbar: the thumb pans, the track pages, a double-click shows the whole outing.
    void horizontalScrollbar() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 2));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const plot::TdLayout& L = g.layout();
        QVERIFY(L.scrollbar.h > 0.0F);
        const plot::XSpan extent = g.staticInfo().scrollExtent;
        const double outing = 40.0 + 50.0 + 48.0 + 49.0 + 30.0;
        QVERIFY(std::abs(extent.length() - outing * 1e6) < 1.0);
        const plot::XSpan before = w.view->state().zoom;
        // Drag the thumb right by a tenth of the bar: the span moves by a tenth of the outing.
        const float dpr = L.dpr;
        const double thumbX = (wide(L.scrollbar.x) + (0.5 * (before.a + before.b) - extent.a) /
                                                         extent.length() * wide(L.scrollbar.w)) /
                              wide(dpr);
        const double y = (wide(L.scrollbar.y) + 0.5 * wide(L.scrollbar.h)) / wide(dpr);
        QCOMPARE(g.hitAt({thumbX, y}).handleKind, plot::HandleKind::ScrollThumb);
        drag(&g, {thumbX, y}, {thumbX + 0.1 * wide(L.scrollbar.w) / wide(dpr), y});
        const plot::XSpan moved = w.view->state().zoom;
        QVERIFY(std::abs((moved.a - before.a) - 0.1 * extent.length()) < 0.01 * extent.length());
        QCOMPARE(moved.length(), before.length());
        // A double-click: the whole outing.
        QVERIFY(waitQuiet(g));
        doubleClick(&g, {thumbX, y});
        QVERIFY(std::abs(w.view->state().zoom.a - extent.a) < 1.0);
        QVERIFY(std::abs(w.view->state().zoom.b - extent.b) < 1.0);
    }

    // ALT+UP / ALT+DOWN / ALT+F2, SHIFT+UP / DOWN and the ALT rubber band act on the active group only.
    void verticalZoomOfTheActiveGroup() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.layout().activeGroup, 0);
        const auto full = g.staticInfo().scales[0];
        const auto throttle = g.staticInfo().scales[1];
        QVERIFY(full.has_value());
        QVERIFY(g.handleCommand(QStringLiteral("zoom.v_in"), {}));
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.verticalZoomOf(0), (plot::TdGroupZoom{0.25, 0.75}));
        const auto zoomed = g.staticInfo().scales[0];
        QVERIFY(zoomed.has_value());
        QVERIFY(std::abs((zoomed->second - zoomed->first) - 0.5 * (full->second - full->first)) < 1e-9);
        QCOMPARE(g.staticInfo().scales[1], throttle); // the other group keeps its scale
        QVERIFY(g.handleCommand(QStringLiteral("pan.up"), {}));
        QVERIFY(std::abs(g.verticalZoomOf(0).lo - 0.30) < 1e-12);
        QVERIFY(g.handleCommand(QStringLiteral("pan.down"), {}));
        QVERIFY(g.handleCommand(QStringLiteral("zoom.v_out"), {}));
        QCOMPARE(g.verticalZoomOf(0), plot::TdGroupZoom{});
        QVERIFY(g.handleCommand(QStringLiteral("zoom.v_in"), {}));
        QVERIFY(g.handleCommand(QStringLiteral("zoom.v_full"), {}));
        QCOMPARE(g.verticalZoomOf(0), plot::TdGroupZoom{});
        // ALT + rubber band over the panel's middle half: the shown fraction [0.25, 0.75].
        QVERIFY(waitQuiet(g));
        const plot::RectPx panel = g.layout().groups[0].panel;
        const float dpr = g.layout().dpr;
        doubleClick(&g, at(panel, 0.5, 0.25, dpr), Qt::AltModifier);
        QVERIFY(g.rubberBandActive());
        sendMouse(&g, QEvent::MouseMove, at(panel, 0.6, 0.75, dpr), Qt::NoButton, Qt::NoButton,
                  Qt::AltModifier);
        const plot::XSpan zoomBefore = w.view->state().zoom;
        click(&g, at(panel, 0.6, 0.75, dpr));
        QVERIFY(!g.rubberBandActive());
        QCOMPARE(w.view->state().zoom, zoomBefore); // vertical only
        QVERIFY(std::abs(g.verticalZoomOf(0).lo - 0.25) < 0.02);
        QVERIFY(std::abs(g.verticalZoomOf(0).hi - 0.75) < 0.02);
        // A double-click on the vertical scrollbar: full again.
        QVERIFY(waitQuiet(g));
        const plot::RectPx vs = g.layout().groups[0].vScroll;
        QVERIFY(vs.w > 0.0F);
        doubleClick(&g, at(vs, 0.5, 0.5, dpr));
        QCOMPARE(g.verticalZoomOf(0), plot::TdGroupZoom{});
    }

    // Overlapped mode: dragging a Hi handle edits the group's region; panels mode: the splitter edits the
    // edges.
    void handlesEditTheGroups() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        // Panels: drag the splitter from the middle to 30 % of the area.
        const plot::TdLayout& L = g.layout();
        const float dpr = L.dpr;
        const plot::RectPx t = L.traceArea;
        const QPointF split((wide(L.groups[0].axisStrip.x) + 0.5 * wide(L.groups[0].axisStrip.w)) / wide(dpr),
                            wide(L.groups[0].panel.y + L.groups[0].panel.h) / wide(dpr));
        QCOMPARE(g.hitAt(split).handleKind, plot::HandleKind::PanelSplitter);
        drag(&g, split, {split.x(), (wide(t.y) + 0.3 * wide(t.h)) / wide(dpr)});
        QVERIFY(waitQuiet(g));
        QVERIFY(std::abs(g.body().groups[0].edgeBottom - 0.3) < 0.01);
        QVERIFY(std::abs(g.body().groups[1].edgeTop - 0.3) < 0.01);
        QCOMPARE(g.body().groups[1].edgeBottom, 1.0);
        // Overlapped: the Throttle group's Hi handle up to 80 %.
        QVERIFY(g.handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QVERIFY(waitQuiet(g));
        const plot::RectPx hi = g.layout().groups[1].hiHandle;
        const QPointF handle = at(hi, 0.5, 0.5, dpr);
        QCOMPARE(g.hitAt(handle).handleKind, plot::HandleKind::GroupHi);
        const plot::RectPx area = g.layout().traceArea;
        drag(&g, handle, {handle.x(), (wide(area.y) + 0.8 * wide(area.h)) / wide(dpr)});
        QVERIFY(std::abs(g.body().groups[1].hi - 0.8) < 0.01);
        QVERIFY(g.body().overlapped());
    }

    // AC-M3-09 in the widget: dragging the Main sub-axis moves the traces while the offset is pending and
    // commits one batch at the release; the offset step is the active channel's sample period in that slot's
    // source, or 0.25 m in distance mode.
    void offsetAxisDragAndStep() {
        World w;
        const auto a = w.publish(SourceOptions{});
        SourceOptions slow;
        slow.path = "slow.ld";
        slow.rateHz = 20;
        const auto b = w.publish(slow);
        selectMain(w, lapRef(*a, 1));
        w.selection.toggleOverlay0(lapRef(*b, 1));
        int commits = 0; // one per range committed (the shell pushes an OffsetRangeCommand per call)
        w.view->setOffsetCommit([&](const model::RangeRef&, plot::Domain, double) { ++commits; });
        Rig rig(w, graphSpec({{"Wheel Speed FL"}}));
        TimeDistanceGraph& g = *rig.graph;
        g.setFocusedComponent(true); // the focused graph reports the offset steps
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.offsetStep(0), std::optional<double>(20'000.0)); // 50 Hz
        QCOMPARE(g.offsetStep(1), std::optional<double>(50'000.0)); // the overlay's source: 20 Hz
        QCOMPARE(w.view->samplePeriodOf(a->id), Micros{20'000});
        QCOMPARE(w.view->samplePeriodOf(b->id), Micros{50'000});
        // CTRL+SHIFT+RIGHT / UP and ALT+SHIFT+LEFT (the offset rows act on the view model): each range by its
        // own source's step, pending until 250 ms after the last key.
        w.view->offsetRange(lapRef(*a, 1), 1);
        QCOMPARE(w.view->pendingOffsets().at(lapRef(*a, 1)), 20'000.0);
        w.view->offsetRange(lapRef(*a, 1), 10);
        QCOMPARE(w.view->pendingOffsets().at(lapRef(*a, 1)), 220'000.0);
        w.view->offsetRange(lapRef(*b, 1), -1);
        QCOMPARE(w.view->pendingOffsets().at(lapRef(*b, 1)), -50'000.0);
        QTRY_COMPARE_WITH_TIMEOUT(commits, 2, 2000); // the burst: one commit per range
        QVERIFY(w.view->state().offsetAxis);
        // The drag: the traces follow at once (the pending offset in the inputs), one batch at the release.
        QVERIFY(waitQuiet(g));
        const plot::TdLayout& L = g.layout();
        QVERIFY(L.offsetMain.h > 0.0F);
        const float dpr = L.dpr;
        const QPointF from = at(L.offsetMain, 0.4, 0.5, dpr);
        const QPointF to(from.x() + 0.1 * wide(L.traceArea.w) / wide(dpr), from.y());
        sendMouse(&g, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        QVERIFY(w.view->offsetDragActive());
        sendMouse(&g, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        const double expected = 0.1 * w.view->state().zoom.length();
        QVERIFY(std::abs(w.view->pendingOffsets().at(lapRef(*a, 1)) - expected) < 0.01 * expected);
        QVERIFY(waitQuiet(g));
        QVERIFY(std::abs(static_cast<double>(g.inputs().slots[0].range.offsetUs) - expected) <
                0.01 * expected);
        QTest::qWait(400); // no commit in the middle of a drag
        QCOMPARE(commits, 2);
        sendMouse(&g, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(commits, 3); // the whole drag: one commit at the release
        QVERIFY(!w.view->offsetDragActive());
        // Distance mode: 0.25 m.
        w.view->setDomain(plot::Domain::Distance);
        QCOMPARE(g.offsetStep(0), std::optional<double>(0.25));
    }

    // CTRL+SHIFT+C: name, value and unit rows at the Main cursor; with a datum the datum value and delta too.
    void copyCursorValues() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        QVERIFY(g.handleCommand(QStringLiteral("graph.copy_cursor_values"), {}));
        const QStringList rows = QApplication::clipboard()->text().split(QLatin1Char('\n'));
        QCOMPARE(rows.size(), 2);
        const QStringList throttle = rows[1].split(QLatin1Char('\t'));
        QCOMPARE(throttle.size(), 3);
        QCOMPARE(throttle[0], QStringLiteral("Throttle"));
        QCOMPARE(throttle[2], QStringLiteral("%"));
        // The synthetic throttle at 10 s of a 50 s lap: 50 + 50 cos(2 pi / 5).
        QVERIFY(std::abs(throttle[1].toDouble() - (50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * 0.2))) <
                0.06);
        w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic);
        w.view->placeDatum();
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        const QStringList withDatum =
            g.cursorValuesText().split(QLatin1Char('\n'))[1].split(QLatin1Char('\t'));
        QCOMPARE(withDatum.size(), 5);
        QVERIFY(std::abs(withDatum[3].toDouble() - (withDatum[1].toDouble() - withDatum[2].toDouble())) <
                0.11);
    }

    // Export Data: the graph's channels over the zoom as CSV (04 section 8.13).
    void exportCsv() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, graphSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        w.view->zoomTo(plot::XSpan{0.0, 2.0 * 1e6});
        QVERIFY(waitQuiet(g));
        Sink sink;
        QVERIFY(g.exportData(sink, ExportOptions{}));
        const QList<QByteArray> lines = sink.data.trimmed().split('\n');
        QVERIFY(lines.size() >= 100); // 2 s at 50 Hz
        QVERIFY(lines.front().contains("Wheel Speed FL"));
        QVERIFY(lines.front().contains("Throttle"));
    }

    // Tooltips: annotations from the Main source's sidecar, device events and status rows.
    // The marker band's annotation Edit / Remove (and Add) commit the Main source's sidecar through the
    // selection's SidecarCommit hook with their own undo text (04 section 8.11 item 4).
    void annotationEditsCommitTheSidecar() {
        World w;
        auto src = std::const_pointer_cast<model::DataSource>(makeSource(SourceOptions{}, w.rt.cache()));
        model::Annotation note;
        note.id = uuidFromHash(hash128("annotation:edit"));
        note.tUs = lapAt(*src, 1).start + 12 * kSec;
        note.text = "Brake early";
        src->sidecar.setAnnotation("Normal", note);
        w.maths.addSource(src);
        w.selection.sourcePublished(src);
        QStringList texts;
        w.selection.setSidecarCommit(
            [&](std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc) {
                texts << w.selection.sidecarCommitText();
                w.selection.sourceReplaced(current->withSidecar(
                    std::move(doc), nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction()));
            });
        selectMain(w, lapRef(*src, 1));
        Rig rig(w, graphSpec({{"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const model::MarkerId id{note.id};
        const auto annotations = [&w] {
            return w.selection.source(w.selection.snapshot().main.src)->sidecar.layer("Normal").annotations;
        };
        QVERIFY(g.editAnnotation(id, QStringLiteral("Brake later")));
        QCOMPARE(texts.back(), QStringLiteral("Edit annotation"));
        QCOMPARE(annotations().size(), std::size_t{1});
        QCOMPARE(annotations().front().text, std::string("Brake later"));
        QVERIFY(waitQuiet(g));
        const plot::TdLayout& L = g.layout();
        const float dpr = L.dpr;
        const QPointF onNote(g.logicalXOf(12.0 * 1e6),
                             (wide(L.bands.marker.y) + 0.5 * wide(L.bands.marker.h)) / wide(dpr));
        QCOMPARE(g.toolTipAt(onNote), QStringLiteral("Brake later"));
        // Add at the cursor: the Main source's absolute time, the active channel noted.
        w.view->setCursor(30 * kSec, qt::CursorSource::Programmatic);
        QVERIFY(g.addAnnotationAtCursor(QStringLiteral("Apex")));
        QCOMPARE(texts.back(), QStringLiteral("Add annotation"));
        const std::vector<model::Annotation> notes = annotations();
        QCOMPARE(notes.size(), std::size_t{2});
        const auto added = std::find_if(notes.begin(), notes.end(),
                                        [](const model::Annotation& a) { return a.text == "Apex"; });
        QVERIFY(added != notes.end());
        QCOMPARE(added->tUs, lapAt(*src, 1).start + 30 * kSec);
        QCOMPARE(added->channel, std::optional<std::string>("Throttle"));
        // Right-click the first one: the menu's Remove.
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.hitAt(onNote).kind, plot::HitKind::Annotation);
        sendMouse(&g, QEvent::MouseButtonPress, onNote, Qt::RightButton, Qt::RightButton);
        QMenu* menu = g.findChild<QMenu*>(QStringLiteral("esses.annotationMenu"));
        QVERIFY(menu != nullptr);
        QAction* remove = menu->findChild<QAction*>(QStringLiteral("removeAnnotation"));
        QVERIFY(remove != nullptr);
        remove->trigger();
        menu->close();
        QCOMPARE(texts.back(), QStringLiteral("Remove annotation"));
        QCOMPARE(annotations().size(), std::size_t{1});
        QCOMPARE(annotations().front().text, std::string("Apex"));
        QVERIFY(!g.removeAnnotation(id)); // already gone
        QVERIFY(!g.editAnnotation(id, QStringLiteral("x")));
    }

    // The project's channel override limits (Channel Editor scale min / max) reach the graph: Channel scaling
    // takes them and the min / max scale lines draw there (04 section 8.3 items 2 and 5).
    void overrideLimitsScaleAndDrawLines() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PathOverrides o;
        const std::filesystem::path root = dir.path().toStdWString();
        o.configDir = root / "config";
        o.dataDir = repoRoot() / "data";
        o.cacheDir = root / "cache";
        o.logDir = root / "logs";
        Result<Paths> paths = Paths::resolve(o);
        QVERIFY(paths.ok());
        qt::ProjectSession session(std::move(paths).value());
        project::ChannelOverrides overrides;
        project::ChannelOverrides::Entry entry;
        entry.scaleMin = model::Quantityd{10.0, units::kNoUnit, 0, "%"};
        entry.scaleMax = model::Quantityd{90.0, units::kNoUnit, 0, "%"};
        overrides.set("Throttle", entry);
        session.setOverrides(overrides);
        World w("time", false, &session);
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        project::ComponentSpec spec = graphSpec({{"Throttle"}});
        QJsonObject body = bodyOf(spec);
        QJsonArray groups = body.value(QStringLiteral("groups")).toArray();
        QJsonObject group = groups.at(0).toObject();
        group.insert(
            QStringLiteral("lines"),
            QJsonObject{
                {QStringLiteral("min"), QJsonObject{{QStringLiteral("enabled"), true},
                                                    {QStringLiteral("colour"), QStringLiteral("#2e7d32")}}},
                {QStringLiteral("max"), QJsonObject{{QStringLiteral("enabled"), true},
                                                    {QStringLiteral("colour"), QStringLiteral("#c62828")}}}});
        groups.replace(0, group);
        body.insert(QStringLiteral("groups"), groups);
        spec.body = model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString())
                        .value();
        Rig rig(w, spec);
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const plot::ResolvedChannel& rc = g.inputs().slots.front().channels.front();
        QVERIFY(rc.limitMin.has_value() && rc.limitMax.has_value());
        const std::pair<double, double> scale =
            g.staticInfo().scales.front().value_or(std::pair<double, double>{});
        QVERIFY(std::abs(scale.first - 10.0) < 1e-9);
        QVERIFY(std::abs(scale.second - 90.0) < 1e-9);
        int lines = 0;
        g.setBuildObserver([&](const plot::ComponentBuild& build) {
            for (const plot::DrawCmd& c : build.list.cmds) {
                lines +=
                    c.kind == plot::CmdKind::Lines && (c.colour == 0x2E7D32FFU || c.colour == 0xC62828FFU)
                        ? 1
                        : 0;
            }
        });
        g.onViewChanged(w.view->state(), plot::mask(plot::Change::Scale));
        QVERIFY(waitQuiet(g));
        QCOMPARE(lines, 2);
    }

    void tooltipsNameMarkersAndStatus() {
        World w;
        SourceOptions o;
        auto src = std::const_pointer_cast<model::DataSource>(makeSource(o, w.rt.cache()));
        model::Annotation note;
        note.id = uuidFromHash(hash128("annotation:test"));
        note.tUs = lapAt(*src, 1).start + 12 * kSec;
        note.text = "Brake early";
        src->sidecar.setAnnotation("Normal", note);
        model::Marker event;
        event.id = model::MarkerId{uuidFromHash(hash128("event:test"))};
        event.cls = model::MarkerClass::DeviceEvent;
        event.group = "DeviceEvents";
        event.t = lapAt(*src, 1).start + 30 * kSec;
        event.comment = "Power cycle";
        src->layers.front().markers.push_back(event);
        model::sortMarkers(src->layers.front().markers);
        w.maths.addSource(src);
        w.selection.sourcePublished(src);
        selectMain(w, lapRef(*src, 1));
        Rig rig(w, graphSpec({{"Throttle"}},
                             {{QStringLiteral("showStatusStrip"), true},
                              {QStringLiteral("statusChannels"), QJsonArray{QStringLiteral("Throttle")}}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const plot::TdLayout& L = g.layout();
        const float dpr = L.dpr;
        const double markerY = (wide(L.bands.marker.y) + 0.5 * wide(L.bands.marker.h)) / wide(dpr);
        QCOMPARE(g.toolTipAt({g.logicalXOf(12.0 * 1e6), markerY}), QStringLiteral("Brake early"));
        QCOMPARE(g.toolTipAt({g.logicalXOf(30.0 * 1e6), markerY}), QStringLiteral("Power cycle"));
        QVERIFY(g.staticInfo().markers.size() >= 2);
        QVERIFY(L.statusStrip.h > 0.0F);
        const QString status = g.toolTipAt(at(L.statusStrip, 0.5, 0.5, dpr));
        QVERIFY2(status.startsWith(QStringLiteral("Throttle: ")), qPrintable(status));
        // SHIFT+E hides the device events.
        QVERIFY(g.handleCommand(QStringLiteral("graph.device_events"), {}));
        QVERIFY(waitQuiet(g));
        QVERIFY(g.toolTipAt({g.logicalXOf(30.0 * 1e6), markerY}).isEmpty());
        QCOMPARE(g.toolTipAt({g.logicalXOf(12.0 * 1e6), markerY}), QStringLiteral("Brake early"));
    }
};

QTEST_MAIN(TstTimeDistanceM3)
#include "tst_time_distance_m3.moc"
