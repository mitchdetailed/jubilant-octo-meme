// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceGraph (04 sections 6.2-6.3 and 8; AC-M1-01, -02, -09, -12), offscreen: the Static build on the
// TaskPool delivered through the Dispatcher; a cursor move (programmatic and mouse drag) rebuilds and
// repaints only the Dynamic layer; wheel zoom about the pointer, x-axis pan drag, rubber band and keyboard
// zoom; D-017 overlay colours and draw order in both domains (channel colours, overlayPalette, single colour
// Main); F9 through the WorksheetViewModel (traces aligned at the start line in distance mode, the zoom back
// within one sample, the "no distance data" banner); like-units grouping; Manual vs Channel scale; the
// active channel by key click; 30 Hz Size coalescing; renderToImage; nothing blocks a NoBlock GUI thread.

#include "components/component_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;
constexpr int kTimeoutMs = 30'000;

std::atomic<int> gAsserts{0};
void recordingHandler(const char*, const char*, int, const char*) {
    gAsserts.fetch_add(1);
}

// Main = `ref` with the zoom on it (W): the worksheet keeps the zoom of the first Main otherwise (N/P rule).
void selectMain(World& w, const model::RangeRef& ref) {
    w.selection.setMain(ref);
    w.view->zoomToMainRange();
    w.view->setCursor(0, qt::CursorSource::Programmatic);
}

// One graph in a plain host widget (child geometry changes deliver resize events synchronously).
struct Rig {
    explicit Rig(World& world, const project::ComponentSpec& spec, QSize size = {800, 400}) {
        host = std::make_unique<QWidget>();
        host->resize(size);
        graph = new TimeDistanceGraph(spec, *world.context, host.get());
        graph->setGeometry(QRect(QPoint(0, 0), size));
        host->show();
    }
    std::unique_ptr<QWidget> host;
    TimeDistanceGraph* graph = nullptr;
};

// Waits until the Static layer is current and stays so (late maths/distance results trigger rebuilds).
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

void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

void sendWheel(QWidget* w, QPointF pos, int deltaY) {
    QWheelEvent e(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, deltaY), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

// The layout is float device px; the tests compute logical positions in double (an exact widening).
double wide(float v) {
    return static_cast<double>(v);
}

// Logical centre of a device-px rectangle.
QPointF centreOf(const plot::RectPx& r, float dpr) {
    return {(wide(r.x) + 0.5 * wide(r.w)) / wide(dpr), (wide(r.y) + 0.5 * wide(r.h)) / wide(dpr)};
}

// Distinct consecutive stroke colours of a Static list (a trace may be several polylines).
std::vector<project::Rgba> traceColourOrder(const plot::DrawList& list) {
    std::vector<project::Rgba> out;
    for (const project::Rgba c : strokeColours(list)) {
        if (out.empty() || out.back() != c) {
            out.push_back(c);
        }
    }
    return out;
}

bool hasText(const plot::DrawList& list, std::string_view needle, bool exact = false) {
    for (const plot::TextRun& run : list.texts) {
        const std::string_view t = list.textOf(run);
        if (exact ? t == needle : t.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// Per polyline of `colour` (in emission order), the x (device px) where the trace first falls below the
// middle of the panel (the throttle's u = 0.25 crossing).
std::vector<float> crossings(const plot::DrawList& list, project::Rgba colour, const plot::RectPx& panel) {
    std::vector<float> out;
    const float mid = panel.y + 0.5F * panel.h;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind != plot::CmdKind::Polyline || c.colour != colour) {
            continue;
        }
        for (std::uint32_t v = 1; v < c.vertexCount; ++v) {
            const float x0 = list.verts[2 * (c.vertexOffset + v - 1)];
            const float y0 = list.verts[2 * (c.vertexOffset + v - 1) + 1];
            const float x1 = list.verts[2 * (c.vertexOffset + v)];
            const float y1 = list.verts[2 * (c.vertexOffset + v) + 1];
            if (y0 <= mid && y1 > mid) { // y grows downwards: the value falls through the middle
                out.push_back(x0 + (x1 - x0) * (mid - y0) / (y1 - y0));
                break;
            }
        }
    }
    return out;
}

// The largest numeric text run anchored inside `rect` (a vertical axis strip); -inf when there is none.
double maxLabelIn(const plot::DrawList& list, const plot::RectPx& rect) {
    double best = -std::numeric_limits<double>::infinity();
    for (const plot::TextRun& run : list.texts) {
        if (run.x < rect.x || run.x > rect.x + rect.w || run.y < rect.y - 1.0F ||
            run.y > rect.y + rect.h + 1.0F) {
            continue;
        }
        bool ok = false;
        const double v =
            QString::fromUtf8(list.textOf(run).data(), static_cast<qsizetype>(list.textOf(run).size()))
                .toDouble(&ok);
        if (ok) {
            best = std::max(best, v);
        }
    }
    return best;
}

QJsonObject bodyOf(const project::ComponentSpec& spec) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
}

int regionArea(const QRegion& r) {
    int a = 0;
    for (const QRect& rect : r) {
        a += rect.width() * rect.height();
    }
    return a;
}

} // namespace

class TstTimeDistanceGraph : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
        QVERIFY(ComponentWidget::isRegistered(QString::fromLatin1(TimeDistanceGraph::kType)));
    }

    void staticBuildDeliveredOnTheGuiThread() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL", "Wheel Speed FR"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QThread* gui = QThread::currentThread();
        bool onGui = true;
        g.setBuildObserver(
            [&](const plot::ComponentBuild&) { onGui = onGui && QThread::currentThread() == gui; });
        QVERIFY(waitQuiet(g));
        QVERIFY(onGui);
        QVERIFY(g.staticBuildsDelivered() >= 1);
        QCOMPARE(g.builtViewVersion(), w.view->state().version);
        const qreal dpr = g.devicePixelRatioF();
        QCOMPARE(g.layers().staticImage().size(), (QSizeF(800, 400) * dpr).toSize());
        QCOMPARE(g.layout().groups.size(), std::size_t{2}); // two panels
        QCOMPARE(g.channelNames(),
                 (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Wheel Speed FR"),
                              QStringLiteral("Throttle")}));
        QCOMPARE(g.commandContext(), qt::CommandContext::Graph);
        QVERIFY(g.contextCommands().contains(QStringLiteral("data.toggle_domain")));
        // The traces are in the image: the Main colour of the throttle trace inside its panel.
        const plot::RectPx p = g.layout().groups[1].panel;
        const QRect panel(static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(p.w),
                          static_cast<int>(p.h));
        QVERIFY(countColour(g.layers().staticImage(), panel, w.scheme->channelColour(2)) > 50);
        QVERIFY(!g.hits().empty());
    }

    // AC-M1-02's structural half: a cursor move is Dynamic only (no Static build, no Chrome rebuild, the
    // Static image untouched) and repaints only the damage (old and new cursor strips plus key values).
    void cursorMoveRepaintsOnlyDynamic() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        w.selection.toggleOverlay(lapRef(*a, 2));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
        // M3: a datum appearing adds value@datum and delta to the key (a Static rebuild); place it first, so
        // the datum move at the end is Dynamic only like the cursor.
        w.view->placeDatum();
        QVERIFY(waitQuiet(g));
        QTest::qWait(50); // let the first full paint go through

        const int submitted = g.staticBuildsSubmitted();
        const int chrome = g.chromeBuilds();
        const int dynamic = g.dynamicBuilds();
        const qint64 staticKey = g.layers().staticImage().cacheKey();
        const int area = g.width() * g.height();

        // Programmatic (Values window, keyboard): synchronous Dynamic rebuild.
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        QCOMPARE(g.dynamicBuilds(), dynamic + 1);
        const QRegion damage = g.lastDynamicDamage();
        QVERIFY(!damage.isEmpty());
        QVERIFY2(regionArea(damage) < area / 3, "the damage is the cursor strips and the key values");
        QTRY_VERIFY(!g.lastPaintRegion().isEmpty());
        QTRY_VERIFY((g.lastPaintRegion() - damage).isEmpty());
        QVERIFY(g.cursorLogicalX().has_value());
        QVERIFY(std::abs(*g.cursorLogicalX() - g.logicalXOf(10.0 * kSec)) < 1.0);

        // Mouse: press and drag in the trace area; the cursor follows the pointer (snapped to a sample).
        const plot::RectPx t = g.layout().traceArea;
        const float dpr = g.layout().dpr;
        const double y = (wide(t.y) + 0.5 * wide(t.h)) / wide(dpr);
        const double x1 = (wide(t.x) + 0.35 * wide(t.w)) / wide(dpr);
        const double x2 = (wide(t.x) + 0.60 * wide(t.w)) / wide(dpr);
        sendMouse(&g, QEvent::MouseButtonPress, {x1, y}, Qt::LeftButton, Qt::LeftButton);
        QVERIFY(std::abs(*g.cursorLogicalX() - x1) < 1.0);
        sendMouse(&g, QEvent::MouseMove, {x2, y}, Qt::NoButton, Qt::LeftButton);
        QVERIFY(std::abs(*g.cursorLogicalX() - x2) < 1.0);
        sendMouse(&g, QEvent::MouseButtonRelease, {x2, y}, Qt::LeftButton, Qt::NoButton);
        // Snapped to a 50 Hz sample of the Main lap.
        const Micros abs = lapAt(*a, 1).start + w.view->state().cursorT;
        QCOMPARE(abs % (kSec / 50), Micros{0});
        QTest::qWait(60);

        QCOMPARE(g.staticBuildsSubmitted(), submitted);
        QCOMPARE(g.chromeBuilds(), chrome);
        QCOMPARE(g.layers().staticImage().cacheKey(), staticKey);
        QVERIFY(g.dynamicBuilds() >= dynamic + 3);
        QVERIFY(g.staticUpToDate());
        // The cursor line of the Dynamic list sits at the cursor.
        bool lineAtCursor = false;
        const plot::DrawList& d = g.layers().dynamicList();
        for (const plot::DrawCmd& c : d.cmds) {
            if (c.kind == plot::CmdKind::Lines && c.vertexCount >= 2) {
                lineAtCursor = lineAtCursor || std::abs(wide(d.verts[2 * c.vertexOffset] / dpr) - x2) < 1.5;
            }
        }
        QVERIFY(lineAtCursor);
        // Moving the datum is Dynamic too.
        w.view->placeDatum();
        QTest::qWait(30);
        QCOMPARE(g.staticBuildsSubmitted(), submitted);
        QCOMPARE(g.layers().staticImage().cacheKey(), staticKey);
    }

    void zoomAndPan() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const double lap = static_cast<double>(lapAt(*a, 1).end - lapAt(*a, 1).start);
        QCOMPARE(w.view->state().zoom.a, 0.0);
        QCOMPARE(w.view->state().zoom.b, lap);

        const plot::RectPx t = g.layout().traceArea;
        const float dpr = g.layout().dpr;
        const double y = (wide(t.y) + 0.5 * wide(t.h)) / wide(dpr);

        // Wheel: x0.8 about the pointer; the domain value under the pointer stays put.
        const double px = (wide(t.x) + 0.3 * wide(t.w)) / wide(dpr);
        const double xp = *g.domainXAt(px);
        int delivered = g.staticBuildsDelivered();
        sendWheel(&g, {px, y}, 120);
        plot::XSpan z = w.view->state().zoom;
        QVERIFY(std::abs(z.a - (xp - xp * 0.8)) < 1.0);
        QVERIFY(std::abs(z.b - (xp + (lap - xp) * 0.8)) < 1.0);
        QVERIFY(waitQuiet(g));
        QVERIFY(g.staticBuildsDelivered() > delivered);
        QCOMPARE(g.builtViewVersion(), w.view->state().version);
        QCOMPARE(g.inputs().view.zoom, z);
        QVERIFY(std::abs(*g.domainXAt(px) - xp) < 1.0);
        // Wheel down: x1.25 back to the full lap (within the outing clamp).
        sendWheel(&g, {px, y}, -120);
        z = w.view->state().zoom;
        QVERIFY(std::abs(z.a) < 2.0);
        QVERIFY(std::abs(z.b - lap) < 2.0);
        // A high-resolution wheel or touchpad: quarter notches add up to one notch (x0.8 on the fourth); a
        // fraction in the other direction first cancels what was left over.
        sendWheel(&g, {px, y}, 30);
        sendWheel(&g, {px, y}, -30); // direction change: the remainder restarts from -30
        sendWheel(&g, {px, y}, 30);  // and from +30 again
        QCOMPARE(w.view->state().zoom, z);
        sendWheel(&g, {px, y}, 30);
        sendWheel(&g, {px, y}, 30);
        QCOMPARE(w.view->state().zoom, z);
        sendWheel(&g, {px, y}, 30); // 4 x 30 = 120: one notch
        {
            const plot::XSpan q = w.view->state().zoom;
            QVERIFY(std::abs(q.length() - z.length() * 0.8) < 1.0);
            QVERIFY(std::abs(*g.domainXAt(px) - xp) < 1.0);
        }
        sendWheel(&g, {px, y}, -120); // back to the full lap
        QVERIFY(std::abs(w.view->state().zoom.length() - z.length()) < 2.0);

        // Pan by dragging the x axis 50 px to the right: the span moves left by 50 px worth of domain.
        sendWheel(&g, {px, y}, 240); // zoom in first so there is room to pan
        QVERIFY(waitQuiet(g));
        const plot::XSpan before = w.view->state().zoom;
        const plot::RectPx xa = g.layout().xAxis;
        QVERIFY(xa.h > 0.0F);
        const QPointF axis = centreOf(xa, dpr);
        const double widthLogical = static_cast<double>(g.layout().traceWidthPx) / wide(dpr);
        delivered = g.staticBuildsDelivered();
        sendMouse(&g, QEvent::MouseButtonPress, axis, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseMove, axis + QPointF(50.0, 0.0), Qt::NoButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, axis + QPointF(50.0, 0.0), Qt::LeftButton, Qt::NoButton);
        const plot::XSpan after = w.view->state().zoom;
        const double expected = -50.0 / widthLogical * before.length();
        QVERIFY(std::abs((after.a - before.a) - expected) < 1.0);
        QVERIFY(std::abs(after.length() - before.length()) < 1.0);
        QVERIFY(waitQuiet(g));
        QVERIFY(g.staticBuildsDelivered() > delivered);

        // Rubber band: double-click, move, click.
        const double xa1 = (wide(t.x) + 0.2 * wide(t.w)) / wide(dpr);
        const double xa2 = (wide(t.x) + 0.7 * wide(t.w)) / wide(dpr);
        const double d1 = *g.domainXAt(xa1);
        const double d2 = *g.domainXAt(xa2);
        sendMouse(&g, QEvent::MouseButtonPress, {xa1, y}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, {xa1, y}, Qt::LeftButton, Qt::NoButton);
        sendMouse(&g, QEvent::MouseButtonDblClick, {xa1, y}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, {xa1, y}, Qt::LeftButton, Qt::NoButton);
        QVERIFY(g.rubberBandActive());
        const int submittedBand = g.staticBuildsSubmitted();
        sendMouse(&g, QEvent::MouseMove, {xa2, y}, Qt::NoButton, Qt::NoButton);
        QCOMPARE(g.staticBuildsSubmitted(), submittedBand); // the band is Dynamic feedback
        // The button-less move reached the graph (mouse tracking while the band is open): the band's
        // rectangle in the Dynamic list spans xa1..xa2.
        {
            float bandWidth = -1.0F;
            for (const plot::DrawCmd& c : g.layers().dynamicList().cmds) {
                if (c.kind == plot::CmdKind::FilledRect &&
                    std::abs(wide(c.rect.x) - xa1 * wide(dpr)) <= 1.5) {
                    bandWidth = std::max(bandWidth, c.rect.w);
                }
            }
            QVERIFY2(std::abs(wide(bandWidth) - (xa2 - xa1) * wide(dpr)) <= 2.0,
                     "the band follows the pointer");
        }
        sendMouse(&g, QEvent::MouseButtonPress, {xa2, y}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, {xa2, y}, Qt::LeftButton, Qt::NoButton);
        QVERIFY(!g.rubberBandActive());
        z = w.view->state().zoom;
        QVERIFY(std::abs(z.a - d1) < 1.0);
        QVERIFY(std::abs(z.b - d2) < 1.0);

        // Keyboard zoom (the qtmodel command handlers act on the view): a Static rebuild follows.
        QVERIFY(waitQuiet(g));
        delivered = g.staticBuildsDelivered();
        w.view->zoomOut();
        QVERIFY(waitQuiet(g));
        QVERIFY(g.staticBuildsDelivered() > delivered);
        QCOMPARE(g.inputs().view.zoom, w.view->state().zoom);
    }

    // D-017: Main channel colour (or mainSingleColour with J), Overlay 0 roles.overlay0, Overlay n the
    // overlayStyle colour; draw order overlays n..1, Overlay 0, Main last; in both domains.
    void overlayColours() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        w.selection.toggleOverlay0(lapRef(*a, 2));
        w.selection.toggleOverlay(lapRef(*a, 3));
        Rig rig(w, timeDistanceSpec({{"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        plot::DrawList last;
        g.setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
        QVERIFY(waitQuiet(g));

        const project::Scheme& s = *w.scheme;
        const project::Rgba channel = s.channelColour(0);
        const project::Rgba overlay0 = s.role("overlay0");
        QCOMPARE(traceColourOrder(last), (std::vector<project::Rgba>{channel, overlay0, channel}));
        const plot::RectPx p = g.layout().groups[0].panel;
        const QRect panel(static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(p.w),
                          static_cast<int>(p.h));
        QVERIFY(countColour(g.layers().staticImage(), panel, overlay0) > 50);

        // overlayStyle colourSource "overlayPalette": Overlay 1 takes overlayPalette[0].
        project::Scheme palette = s;
        palette.overlayStyle.colourSource = "overlayPalette";
        g.onSchemeChanged(palette);
        QVERIFY(waitQuiet(g));
        const project::Rgba overlay1 = s.overlayColour(1);
        QCOMPARE(traceColourOrder(last), (std::vector<project::Rgba>{overlay1, overlay0, channel}));
        QVERIFY(countColour(g.layers().staticImage(), panel, overlay1) > 50);

        // J: single colour Main.
        w.selection.setSingleColourMain(true);
        QVERIFY(waitQuiet(g));
        const project::Rgba single = s.role("mainSingleColour");
        QCOMPARE(traceColourOrder(last), (std::vector<project::Rgba>{overlay1, overlay0, single}));
        // The key's Main value colour follows too (Dynamic, built with the same inputs).
        QCOMPARE(g.inputs().selection.singleColourMain, true);

        // Distance mode: the same slots and colours.
        w.view->setDomain(plot::Domain::Distance);
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.inputs().view.domain, plot::Domain::Distance);
        QCOMPARE(traceColourOrder(last), (std::vector<project::Rgba>{overlay1, overlay0, single}));
        // Every lap has the same throttle per distance, so Main covers the overlays in the image; the overlay
        // traces span the panel in the list.
        std::uint32_t overlayVertices = 0;
        for (const plot::DrawCmd& c : last.cmds) {
            if (c.kind == plot::CmdKind::Polyline && c.colour == overlay0) {
                overlayVertices += c.vertexCount;
            }
        }
        QVERIFY(overlayVertices > static_cast<std::uint32_t>(g.layout().traceWidthPx));

        // F4 hides the overlays.
        w.selection.toggleOverlaysHidden();
        QVERIFY(waitQuiet(g));
        QCOMPARE(traceColourOrder(last), (std::vector<project::Rgba>{single}));
    }

    // AC-M1-09 through the graph: F9 converts the zoom through the Main lap's distance index, the traces of
    // Main and Overlay 1 align at the start line in distance mode (the same throttle feature at the same x),
    // F9 back restores the zoom within one sample; a source without distance data shows the banner.
    void f9Toggle() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));             // 50 s
        w.selection.toggleOverlay(lapRef(*a, 2)); // 48 s, Overlay 1
        Rig rig(w, timeDistanceSpec({{"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        plot::DrawList last;
        g.setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
        QVERIFY(waitQuiet(g));
        const plot::XSpan timeZoom = w.view->state().zoom;
        const project::Rgba channel = w.scheme->channelColour(0); // Main and Overlay 1 (overlayStyle channel)
        const plot::RectPx panel = g.layout().groups[0].panel;

        // Time mode: laps align at their start, so the throttle's falling crossing of the panel middle
        // (u = 0.25 of each lap) lies 0.5 s apart: 1 % of the 50 s span.
        std::vector<float> cross = crossings(last, channel, panel);
        QCOMPARE(cross.size(), std::size_t{2}); // Overlay 1 first, Main last
        const float pxPerSecond = static_cast<float>(g.layout().traceWidthPx) / 50.0F;
        QVERIFY2(std::abs((cross[1] - cross[0]) - 0.5F * pxPerSecond) < 2.0F, "time mode: 0.5 s apart");

        // F9: distance through the Main lap's index; the same feature at the same distance.
        w.view->toggleDomain();
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.inputs().view.domain, plot::Domain::Distance);
        QCOMPARE(g.builtViewVersion(), w.view->state().version);
        const plot::XSpan distZoom = w.view->state().zoom;
        QVERIFY(std::abs(distZoom.a) < 1.0);
        QVERIFY(std::abs(distZoom.b - 2000.0) < 5.0); // the lap's distance
        QVERIFY(!hasText(g.layers().chromeList(), "No distance data"));
        QVERIFY(!w.view->noDistance());
        cross = crossings(last, channel, panel);
        QCOMPARE(cross.size(), std::size_t{2});
        QVERIFY2(std::abs(cross[1] - cross[0]) < 1.5F, "distance mode: aligned");

        // F9 back: the time zoom within one sample (20 ms at 50 Hz).
        w.view->toggleDomain();
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.inputs().view.domain, plot::Domain::Time);
        QVERIFY(std::abs(w.view->state().zoom.a - timeZoom.a) <= 20'000.0);
        QVERIFY(std::abs(w.view->state().zoom.b - timeZoom.b) <= 20'000.0);

        // A Main source without distance data: the banner in Chrome (AC-M1-09).
        SourceOptions nb;
        nb.path = "no-distance.ld";
        nb.wheelSpeeds = false;
        const auto b = w.publish(nb);
        w.selection.toggleOverlay(lapRef(*a, 2)); // remove the overlay
        w.selection.setMain(lapRef(*b, 1));
        w.view->setDomain(plot::Domain::Distance);
        QTRY_VERIFY_WITH_TIMEOUT(w.distance.state(b->id) == qt::DistanceState::NoDistance, kTimeoutMs);
        QVERIFY(waitQuiet(g));
        QTRY_VERIFY(hasText(g.layers().chromeList(), "No distance data"));
        QVERIFY(w.view->noDistance());
    }

    // 04 section 8.2 item 3: like-units grouping; CTRL (newPanel) -> a new group; applySpec rebuilds.
    void likeUnitsGrouping() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        QSignalSpy edits(&g, &ComponentWidget::specEditRequested);
        const auto lastBody = [&edits] {
            return bodyOf(edits.constLast().at(0).value<project::ComponentSpec>());
        };
        const auto tracesOf = [](const QJsonObject& body, int group) {
            QStringList names;
            for (const QJsonValue& t : body.value(QStringLiteral("groups"))
                                           .toArray()
                                           .at(group)
                                           .toObject()
                                           .value(QStringLiteral("traces"))
                                           .toArray()) {
                names << t.toObject().value(QStringLiteral("channel")).toString();
            }
            return names;
        };

        // A speed joins the speed group.
        g.addChannel(QStringLiteral("Wheel Speed FR"), false);
        QCOMPARE(edits.size(), 1);
        QJsonObject body = lastBody();
        QCOMPARE(body.value(QStringLiteral("groups")).toArray().size(), 2);
        QCOMPARE(tracesOf(body, 0),
                 (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Wheel Speed FR")}));
        // A maths output of the same quantity (Corr Speed) joins it as well.
        g.addChannel(QStringLiteral("Corr Speed"), false);
        QCOMPARE(edits.size(), 2);
        QCOMPARE(tracesOf(lastBody(), 0),
                 (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Corr Speed")}));
        // CTRL: a new group at the bottom named after the channel.
        g.addChannel(QStringLiteral("Wheel Speed FR"), true);
        body = lastBody();
        QCOMPARE(body.value(QStringLiteral("groups")).toArray().size(), 3);
        QCOMPARE(body.value(QStringLiteral("groups"))
                     .toArray()
                     .at(2)
                     .toObject()
                     .value(QStringLiteral("name"))
                     .toString(),
                 QStringLiteral("Wheel Speed FR"));
        // A channel the Main source lacks: its own group too.
        g.addChannel(QStringLiteral("Brake Pres"), false);
        QCOMPARE(lastBody().value(QStringLiteral("groups")).toArray().size(), 3);
        // Already present in its like-units group: no edit.
        const qsizetype before = edits.size();
        g.addChannel(QStringLiteral("Wheel Speed FL"), false);
        QCOMPARE(edits.size(), before);

        // The edit applied (the canvas does this): three panels, rebuilt.
        project::ComponentSpec next = edits.at(2).at(0).value<project::ComponentSpec>();
        g.applySpec(next);
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.layout().groups.size(), std::size_t{3});
        QCOMPARE(g.channelNames().size(), 3);
        // The original spec was never edited in place.
        QCOMPARE(g.spec(), next);
    }

    // 04 section 8.3 items 1-2: Manual vs Channel/Auto scale.
    void manualAndChannelScale() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}), {800, 600});
        TimeDistanceGraph& g = *rig.graph;
        plot::DrawList last;
        g.setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        QVERIFY(waitQuiet(g));
        const plot::RectPx strip = g.layout().groups[1].axisStrip;
        QVERIFY(maxLabelIn(last, strip) <= 100.0); // throttle 0..100 %
        QSignalSpy edits(&g, &ComponentWidget::specEditRequested);
        g.setGroupScale(1, plot::ScaleMode::Manual, 0.0, 400.0, QStringLiteral("%"));
        QCOMPARE(edits.size(), 1);
        const project::ComponentSpec manual = edits.constLast().at(0).value<project::ComponentSpec>();
        const QJsonObject group = bodyOf(manual).value(QStringLiteral("groups")).toArray().at(1).toObject();
        QCOMPARE(group.value(QStringLiteral("scaleMode")).toString(), QStringLiteral("manual"));
        QCOMPARE(group.value(QStringLiteral("scaleMax")).toObject().value(QStringLiteral("v")).toDouble(),
                 400.0);
        QCOMPARE(group.value(QStringLiteral("scaleMax")).toObject().value(QStringLiteral("unit")).toString(),
                 QStringLiteral("%"));
        g.applySpec(manual);
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.body().groups[1].scaleMode, plot::ScaleMode::Manual);
        QVERIFY(maxLabelIn(last, strip) >= 300.0);
        // The other group keeps its channel scale.
        QCOMPARE(g.body().groups[0].scaleMode, plot::ScaleMode::Channel);
        // Back to Channel/Auto.
        g.setGroupScale(1, plot::ScaleMode::Channel);
        g.applySpec(edits.constLast().at(0).value<project::ComponentSpec>());
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.body().groups[1].scaleMode, plot::ScaleMode::Channel);
        QVERIFY(maxLabelIn(last, strip) <= 100.0);
    }

    void activeChannelByKeyClick() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        QSignalSpy edits(&g, &ComponentWidget::specEditRequested);
        QSignalSpy focus(&g, &ComponentWidget::focusRequested);
        const plot::TdGroupLayout& gl = g.layout().groups[1];
        QVERIFY(!gl.keyGeometry.rowRects.empty());
        plot::RectPx row = gl.keyGeometry.rowRects[0];
        row.x += gl.key.x;
        row.y += gl.key.y;
        const QPointF at(wide(row.x / g.layout().dpr) + 4.0, wide((row.y + 0.5F * row.h) / g.layout().dpr));
        sendMouse(&g, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(focus.size(), 1);
        QCOMPARE(edits.size(), 1);
        const project::ComponentSpec next = edits.constLast().at(0).value<project::ComponentSpec>();
        QCOMPARE(next.activeChannel, std::string("Throttle"));
        g.applySpec(next);
        QCOMPARE(g.activeChannel(), std::optional<QString>(QStringLiteral("Throttle")));
        QVERIFY(waitQuiet(g));
        // Input is routed through the HitTester of the Static layer: a vertical axis strip neither moves the
        // cursor nor pans; a panel places the cursor.
        const plot::RectPx strip = g.layout().groups[0].axisStrip;
        const QPointF onStrip = centreOf(strip, g.layout().dpr);
        QCOMPARE(g.hitAt(onStrip).kind, plot::HitKind::Axis);
        QCOMPARE(g.hitAt(onStrip).group, 0);
        const plot::ViewState before = w.view->state();
        sendMouse(&g, QEvent::MouseButtonPress, onStrip, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseMove, onStrip + QPointF(40.0, 0.0), Qt::NoButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, onStrip + QPointF(40.0, 0.0), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(w.view->state().cursorT, before.cursorT);
        QCOMPARE(w.view->state().zoom, before.zoom);
        const QPointF inPanel = centreOf(g.layout().groups[0].panel, g.layout().dpr);
        QCOMPARE(g.hitAt(inPanel).kind, plot::HitKind::Panel);
        QCOMPARE(g.hitAt(QPointF(-5.0, -5.0)).kind, plot::HitKind::None);
    }

    // 04 section 6.3 item 6: a burst of resizes is delivered at 30 Hz; the final size always rebuilds.
    void resizeCoalescedTo30Hz() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        rig.host->resize(1000, 500);
        // The burst below must find the throttle idle (a late show/resize pass restarts it): wait for that
        // instead of a fixed time, then nothing runs the event loop until the burst is over.
        QTRY_VERIFY(!g.sizeThrottleActive());
        const int delivered = g.sizeChangesDelivered();
        for (int i = 1; i <= 20; ++i) {
            g.setGeometry(0, 0, 700 + 10 * i, 380 + i); // synchronous resize events (visible child)
        }
        QCOMPARE(g.sizeChangesDelivered(), delivered + 1); // the leading edge only
        QTRY_COMPARE(g.sizeChangesDelivered(), delivered + 2);
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.sizeChangesDelivered(), delivered + 2);
        const qreal dpr = g.devicePixelRatioF();
        QCOMPARE(g.layers().staticImage().size(), (QSizeF(900, 400) * dpr).toSize());
        QCOMPARE(g.layout().bounds.w, static_cast<float>(std::lround(900 * dpr)));
    }

    // 04 section 7.2 item 9: Print to Clipboard paints the current layers; another DPR scales Static.
    void renderToImageUsesCurrentLayers() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Throttle"}}), {400, 300});
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const qreal dpr = g.devicePixelRatioF();
        const QImage now = g.renderToImage(dpr);
        QCOMPARE(now, g.layers().composite());
        const QImage twice = g.renderToImage(dpr * 2.0);
        QCOMPARE(twice.size(), (QSizeF(400, 300) * dpr * 2.0).toSize());
        QCOMPARE(twice.devicePixelRatio(), dpr * 2.0);
    }

    // 04 section 1 item 4 / 6.2: a hidden component keeps its image and builds nothing; changes made while
    // it was hidden are built when it is shown again.
    void hiddenBuildsOnShow() {
        World w;
        const auto a = w.publish(SourceOptions{});
        selectMain(w, lapRef(*a, 1));
        Rig rig(w, timeDistanceSpec({{"Throttle"}}));
        TimeDistanceGraph& g = *rig.graph;
        QVERIFY(waitQuiet(g));
        const qint64 key = g.layers().staticImage().cacheKey();
        const int submitted = g.staticBuildsSubmitted();
        rig.host->hide();
        QTest::qWait(30);
        rig.host->show();
        QVERIFY(QTest::qWaitForWindowExposed(rig.host.get()));
        QTest::qWait(60);
        QCOMPARE(g.staticBuildsSubmitted(), submitted);
        QCOMPARE(g.layers().staticImage().cacheKey(), key);
        rig.host->hide();
        w.view->zoomIn();
        QTest::qWait(60);
        QCOMPARE(g.staticBuildsSubmitted(), submitted); // nothing while hidden
        QVERIFY(!g.staticUpToDate());
        rig.host->show();
        QVERIFY(waitQuiet(g));
        QCOMPARE(g.staticBuildsSubmitted(), submitted + 1);
        QCOMPARE(g.inputs().view.zoom, w.view->state().zoom);
    }

    // AC-M1-12 for the graph: Static builds, Dynamic values, F9 and the cursor on a NoBlock GUI thread never
    // reach a blocking wait (Column::require and Future::get assert there).
    void nothingBlocksTheGuiThread() {
#if ESSES_ASSERTS_ENABLED
        const AssertHandler previous = setAssertHandler(&recordingHandler);
        gAsserts.store(0);
        {
            World w("time", true);
            QVERIFY(ThreadRole::isNoBlock());
            const auto a = w.publish(SourceOptions{});
            selectMain(w, lapRef(*a, 1));
            w.selection.toggleOverlay(lapRef(*a, 2));
            Rig rig(w, timeDistanceSpec({{"Wheel Speed FL", "Corr Speed"}, {"Throttle"}}));
            TimeDistanceGraph& g = *rig.graph;
            QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(a), kTimeoutMs);
            QVERIFY(waitQuiet(g));
            w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic);
            w.view->toggleDomain();
            QVERIFY(waitQuiet(g));
            w.view->zoomIn();
            QVERIFY(waitQuiet(g));
            QVERIFY(!hasText(g.layers().dynamicList(), "\xE2\x80\xA6")); // every key value resolved
        }
        ThreadRole::set(ThreadRole::Normal);
        setAssertHandler(previous);
        QCOMPARE(gAsserts.load(), 0);
#else
        QSKIP("assertions are compiled out in this configuration");
#endif
    }
};

QTEST_MAIN(TstTimeDistanceGraph)
#include "tst_time_distance_graph.moc"
