// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M4-07's automated half (07 section 7.2 "Qt Test dpr_switch"; 07 section 7.1 row 3; 06 section 17 items
// 3-4; 04 section 6.6; D-025, D-209), offscreen, in the real shell (MainWindow on a private configuration
// directory, the shipped workbook, a written synthetic log) on the four screens of
// libs/widgets/tests/m4_hidpi/dpr_test_support.hpp (96/120/144/192 dpi: DPR 1, 1.25, 1.5, 2, as Windows
// reports 100/125/150/200 % monitors). A window moved to another screen is what a drag across monitors does:
// Qt moves it to that screen and changes its DPR (ScreenChangeInternal, DevicePixelRatioChange), and the
// window keeps its logical size as on Windows (moveTo).
//   * windowAcrossMonitors: the main window dragged 1 -> 2 -> 1.25 -> 1.5 -> 1. At each DPR the shell's
//     DprWatcher (HiDpiShell) counts the change, every component of the worksheet is laid out at the new DPR,
//     and the time_distance graph re-renders there: its Static image is lround(logical x DPR) device pixels
//     tagged with the DPR, its lists carry the DPR, its logical placement is the same and its layout
//     (caption, key column, trace area, x axis) stays within a rounding of the DPR 1 layout (no drift), and
//     the window shows its layers pixel for pixel (device-exact, no blur). Back at DPR 1 the graph has the
//     very layout and pixels it started with.
//   * floatingDockAcrossMonitors (data: both dock backends): a dock holding a time_distance (the floating
//     time_distance of the criterion) floats; the shell watches its window; dragged to DPR 2, 1.5 and back to
//     1 it re-renders at each DPR while the main window's graph stays at DPR 1 (mixed DPI: each window at its
//     monitor's DPR); then, floating on the DPR 2 monitor, it is docked back into the DPR 1 window: Qt sends
//     the moved widgets no DPR event; the shell re-checks the window after the dock change and the graph is
//     at DPR 1 (rebuilt by its resize or by that re-check), with nothing of the window left at DPR 2.
//   * trackMapDockAcrossMonitors: the shell's own drawn dock, the Track Map (track_position), floated and
//     dragged to DPR 1.5 renders there.
// A display whose scale changes in place reaches the same DevicePixelRatioChange path; the offscreen platform
// cannot change a screen's DPI at run time through public API, so it is the manual check's step
// (docs/milestones/M4.md section 6.3), with the two-monitor drag.
// A graph is rendered at a DPR (renderedAt) once nothing can re-lay it out any more: no size change waits in
// its 30 Hz throttle (staticUpToDate) or in its canvas's, so a QTRY_* wait, which checks again after the
// first success, never sees it stop holding (the floating graph on macOS before the M4 fix).

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "../../libs/widgets/tests/m4_hidpi/dpr_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/dpr_watcher.hpp>
#include <esses/widgets/shell/hidpi_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWindow>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::dprtest;

namespace {

constexpr int kTimeoutMs = 60'000;
constexpr const char* kGraphDock = "test.floatingGraph";
// Logical layout tolerance against DPR 1 (tst_dpr_sweep.cpp kDriftLogical: two roundings and one hinted text
// line).
constexpr double kDriftLogical = 3.0;
const QPoint kAt(40, 40);

// What one DPR shows of one graph.
struct Look {
    qreal dpr = 0.0;
    QRect geometry; // logical, in its parent
    QSize staticSize;
    qreal staticDpr = 0.0;
    double dynamicDpr = 0.0;
    double chromeDpr = 0.0;
    std::map<std::string, QRectF> rects; // logical
    QImage composite;
    long long grabDiff = -2;
    long long windowDiff = -2;
};

QRectF logicalOf(const plot::RectPx& r, double dpr) {
    return {static_cast<double>(r.x) / dpr, static_cast<double>(r.y) / dpr, static_cast<double>(r.w) / dpr,
            static_cast<double>(r.h) / dpr};
}

Look lookOf(TimeDistanceGraph& g) {
    Look l;
    l.dpr = g.devicePixelRatioF();
    l.geometry = g.geometry();
    const qt::LayerCache& layers = g.layers();
    l.staticSize = layers.staticImage().size();
    l.staticDpr = layers.staticImage().devicePixelRatio();
    l.dynamicDpr = static_cast<double>(layers.dynamicList().dpr);
    l.chromeDpr = static_cast<double>(layers.chromeList().dpr);
    const plot::TdLayout& lay = g.layout();
    const double dpr = static_cast<double>(lay.dpr);
    l.rects["bounds"] = logicalOf(lay.bounds, dpr);
    l.rects["caption"] = logicalOf(lay.caption, dpr);
    l.rects["keyColumn"] = logicalOf(lay.keyColumn, dpr);
    l.rects["traceArea"] = logicalOf(lay.traceArea, dpr);
    l.rects["xAxis"] = logicalOf(lay.xAxis, dpr);
    l.composite = layers.composite();
    l.grabDiff = differingPixels(g.grab().toImage(), l.composite);
    // In its window: the interior (a boundary row or column may be shared with a neighbour, deviceRectOf).
    QWidget* window = g.window();
    const QImage shot = window->grab().toImage();
    const QRect device = deviceRectOf(QRect(g.mapTo(window, QPoint(0, 0)), g.size()), l.dpr);
    const QRect inner = device.adjusted(1, 1, -1, -1).intersected(QRect(QPoint(0, 0), shot.size()));
    l.windowDiff = differingPixels(shot.copy(inner), l.composite.copy(inner.translated(-device.topLeft())));
    return l;
}

QSize deviceSize(QSize logical, qreal dpr) {
    return {static_cast<int>(std::lround(logical.width() * dpr)),
            static_cast<int>(std::lround(logical.height() * dpr))};
}

// Back at DPR 1, the composite equals the first one; on a mismatch both and their difference (red where they
// differ) are written to <build>/tests/ui/artefacts/ (CI uploads them), so a difference can be read: text
// only or geometry too (macOS, D-220).
long long differsFromFirst(const Look& back, const Look& first, const QString& name) {
    const long long n = differingPixels(back.composite, first.composite);
    if (n != 0) {
        const QString dir = QString::fromStdWString(
            (std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts").lexically_normal().wstring());
        QDir().mkpath(dir);
        first.composite.save(dir + QLatin1Char('/') + name + QStringLiteral(".first.png"));
        back.composite.save(dir + QLatin1Char('/') + name + QStringLiteral(".back.png"));
        if (n > 0) {
            QImage diff = rgb32(first.composite);
            const QImage b = rgb32(back.composite);
            for (int y = 0; y < diff.height(); ++y) {
                auto* d = reinterpret_cast<QRgb*>(diff.scanLine(y));
                const auto* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
                for (int x = 0; x < diff.width(); ++x) {
                    d[x] =
                        (d[x] & 0x00FFFFFFU) != (q[x] & 0x00FFFFFFU) ? qRgb(255, 0, 0) : qRgb(255, 255, 255);
                }
            }
            diff.save(dir + QLatin1Char('/') + name + QStringLiteral(".diff.png"));
        }
    }
    return n;
}

double driftOf(const Look& a, const Look& b) {
    double worst = 0.0;
    for (const auto& [name, r] : a.rects) {
        const QRectF& s = b.rects.at(name);
        worst = std::max({worst, std::abs(r.x() - s.x()), std::abs(r.y() - s.y()),
                          std::abs(r.width() - s.width()), std::abs(r.height() - s.height())});
    }
    return worst;
}

// A WorksheetCanvas holding `w` runs its 30 Hz layout throttle: a layout it holds may still place `w` anew.
bool canvasLayoutThrottled(const QWidget& w) {
    for (const QWidget* p = w.parentWidget(); p != nullptr; p = p->parentWidget()) {
        if (const auto* canvas = qobject_cast<const WorksheetCanvas*>(p);
            canvas != nullptr && canvas->layoutThrottleActive()) {
            return true;
        }
    }
    return false;
}

// What a graph shows now, for a failure message. It only reads (uitest::poolSettled would deliver events), so
// it describes the state the check saw.
QString stateOf(TimeDistanceGraph& g) {
    const TaskPool::Stats pool = g.context().render.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : pool.queued) {
        queued += q;
    }
    const QImage& image = g.layers().staticImage();
    const QSize wanted = deviceSize(g.size(), g.devicePixelRatioF());
    return QStringLiteral(
               "widget DPR %1, laid out at %2, layers at %3, %4x%5 logical, Static image %6x%7 at %8 "
               "(%9 wanted); ")
               .arg(g.devicePixelRatioF())
               .arg(g.layoutDevicePixelRatio())
               .arg(g.layers().devicePixelRatio())
               .arg(g.width())
               .arg(g.height())
               .arg(image.width())
               .arg(image.height())
               .arg(image.devicePixelRatio())
               .arg(QStringLiteral("%1x%2").arg(wanted.width()).arg(wanted.height())) +
           QStringLiteral(
               "size change pending %1, size throttle %2, canvas layout throttle %3; Static up to date "
               "%4, builds submitted %5, delivered %6, shown %7; pool queued %8, parked %9, running "
               "%10; visible %11")
               .arg(g.sizeChangePending() ? 1 : 0)
               .arg(g.sizeThrottleActive() ? 1 : 0)
               .arg(canvasLayoutThrottled(g) ? 1 : 0)
               .arg(g.staticUpToDate() ? 1 : 0)
               .arg(g.staticBuildsSubmitted())
               .arg(g.staticBuildsDelivered())
               .arg(g.staticBuildShown())
               .arg(queued)
               .arg(pool.parked)
               .arg(pool.running)
               .arg(g.isVisible() ? 1 : 0);
}

// renderedAt's last result for one graph and DPR since a window last moved (moveTo resets it). A QTRY_* macro
// evaluates its condition again once it holds (Qt 6.12 qtestcase.h: QTRY_TIMEOUT_DEBUG_IMPL, then QVERIFY),
// so a state that holds and then stops holding fails the wait whatever its timeout, and the macro's message
// shows no state: renderedAt logs the state it fell to.
struct LastRendered {
    const TimeDistanceGraph* graph = nullptr;
    qreal dpr = 0.0;
    bool held = false;
};
LastRendered gLastRendered;

// The graph is laid out, rendered and settled at `dpr`: its window's DPR, laid out and its layers built at
// it, its pool settled (no late result can arrive), no WorksheetCanvas holding it still laying it out (the
// canvas's 30 Hz throttle) and its Static layer up to date, which since M4 includes no size change waiting in
// its own 30 Hz throttle (sizeChangePending; before, the image of the size before passed for current and the
// delivery rebuilt the graph after this had held).
bool renderedAt(TimeDistanceGraph& g, qreal dpr) {
    const bool now = g.devicePixelRatioF() == dpr && g.layoutDevicePixelRatio() == dpr &&
                     g.layers().devicePixelRatio() == dpr && uitest::poolSettled(g.context().render.pool()) &&
                     !canvasLayoutThrottled(g) && g.staticUpToDate();
    if (gLastRendered.graph == &g && gLastRendered.dpr == dpr && gLastRendered.held && !now) {
        qWarning("renderedAt(DPR %s) held and no longer holds: %s", qPrintable(dprText(dpr)),
                 qPrintable(stateOf(g)));
    }
    gLastRendered = LastRendered{&g, dpr, now};
    return now;
}

// What the criterion asks of a re-render at `dpr` against the DPR 1 look `base`.
void checkLook(const Look& now, const Look& base, qreal dpr, const char* what) {
    const QString at = QStringLiteral("%1 at DPR %2").arg(QLatin1String(what), dprText(dpr));
    const auto rectText = [](const QRect& r) {
        return QStringLiteral("(%1, %2) %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    };
    QVERIFY2(now.geometry == base.geometry,
             qPrintable(at + QStringLiteral(": logical placement moved from %1 to %2")
                                 .arg(rectText(base.geometry), rectText(now.geometry))));
    QVERIFY2(now.staticSize == deviceSize(now.geometry.size(), dpr) && now.staticDpr == dpr,
             qPrintable(at + QStringLiteral(": Static image %1x%2 at %3")
                                 .arg(now.staticSize.width())
                                 .arg(now.staticSize.height())
                                 .arg(now.staticDpr)));
    QVERIFY2(now.dynamicDpr == dpr && now.chromeDpr == dpr, qPrintable(at + QStringLiteral(": list DPR")));
    const double drift = driftOf(now, base);
    QVERIFY2(drift <= kDriftLogical,
             qPrintable(at + QStringLiteral(": layout moved %1 logical px").arg(drift)));
    QVERIFY2(now.grabDiff == 0,
             qPrintable(at + QStringLiteral(": %1 pixels differ from its layers").arg(now.grabDiff)));
    QVERIFY2(
        now.windowDiff == 0,
        qPrintable(at +
                   QStringLiteral(": in its window %1 pixels differ from its layers").arg(now.windowDiff)));
}

// A drag across monitors as Windows makes it: the window changes screen and DPR and keeps its logical size
// (the platform applies the rectangle WM_DPICHANGED suggests, scaled by the DPR ratio). The offscreen
// platform keeps the native size instead: once the move is reported, a 1280 x 800 window of DPR 1 is 640 x
// 400 logical pixels at DPR 2. So the window changes screen, moves (the platform reports its geometry, which
// the widget follows), and then gets its logical size back, as on Windows.
bool moveTo(QWidget& top, qreal dpr, QPoint offset) {
    gLastRendered = LastRendered{}; // what held before the move may stop holding
    const QSize logical = top.size();
    QWindow* window = top.windowHandle();
    window->setScreen(screenForDpr(dpr));
    top.move(pointOn(dpr, offset));
    if (!QTest::qWaitFor([&] { return top.devicePixelRatioF() == dpr && top.size() == window->size(); },
                         kTimeoutMs)) {
        return false;
    }
    top.resize(logical);
    return QTest::qWaitFor([&] { return top.size() == logical && window->size() == logical; }, kTimeoutMs);
}

project::ComponentSpec graphSpec() {
    QJsonObject body{
        {QStringLiteral("display"),
         QJsonObject{{QStringLiteral("traceStyle"), QStringLiteral("line")},
                     {QStringLiteral("lineWidth"), 1},
                     {QStringLiteral("showGrid"), true},
                     {QStringLiteral("key"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("labels")},
                                                         {QStringLiteral("pos"), QStringLiteral("left")},
                                                         {QStringLiteral("values"), true}}}}},
        {QStringLiteral("groups"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Speed")},
                 {QStringLiteral("scaleMode"), QStringLiteral("channel")},
                 {QStringLiteral("traces"),
                  QJsonArray{QJsonObject{{QStringLiteral("channel"), QStringLiteral("Wheel Speed FL")}}}}},
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Throttle")},
                 {QStringLiteral("scaleMode"), QStringLiteral("channel")},
                 {QStringLiteral("traces"),
                  QJsonArray{QJsonObject{{QStringLiteral("channel"), QStringLiteral("Throttle")}}}}}}}};
    project::ComponentSpec spec;
    spec.id = Uuid::parse("7a2d1f9e-1a51-4f5a-9d43-0a7d2d5f0f10");
    spec.type = "time_distance";
    spec.rect = project::Rect{0.0, 0.0, 1.0, 1.0};
    spec.body = JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString()).value();
    return spec;
}

// The shell on the screen of `dpr` with a written log open and a flying lap selected.
struct Rig {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt = uitest::makeRuntime();
    std::unique_ptr<MainWindow> w;

    bool start(qreal dpr) {
        if (!config.isValid() || !logs.isValid()) {
            return false;
        }
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
            qt::test::SyntheticOptions o;
            o.path = "dpr.ld";
            ld = qt::test::writeLd(*qt::test::makeSource(o, rt->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "dpr");
        }
        w = std::make_unique<MainWindow>(*rt, uitest::pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        w->setScreen(screenForDpr(dpr));
        w->setGeometry(QRect(pointOn(dpr, kAt), QSize(1280, 800)));
        if (!uitest::showAndActivate(*w)) {
            return false;
        }
        w->openLog(ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == 1; }, kTimeoutMs)) {
            return false;
        }
        const model::SourceId src = w->selection().sources().front()->id;
        if (!QTest::qWaitFor([&] { return w->distance().state(src) == qt::DistanceState::Ready; },
                             kTimeoutMs)) {
            return false;
        }
        const std::vector<model::RangeRef> laps = w->selection().displayedRanges(src);
        if (laps.size() < 3) {
            return false;
        }
        w->selection().setMain(laps[2]);
        return true;
    }
    HiDpiShell* hidpi() const { return shellExtension<HiDpiShell>(*w); }
    ~Rig() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }
};

} // namespace

class TstDprSwitch : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        uitest::prepareFontEnvironment();
        useSweepScreens("tst_dpr_switch", std::filesystem::path(ESSES_TEST_BINARY_DIR));
    }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        registerBuiltinComponents();
        for (const ScreenSpec& s : kScreens) {
            QScreen* screen = screenNamed(QString::fromLatin1(s.name));
            QVERIFY2(screen != nullptr, s.name);
            QCOMPARE(screen->devicePixelRatio(), s.dpr);
        }
    }

    void cleanup() {
        gLastRendered = LastRendered{}; // a later row's graph may live at a destroyed one's address
        QCOMPARE(uitest::assertFailures(), 0);
    }

    void windowAcrossMonitors() {
        Rig r;
        QVERIFY(r.start(1.0));
        TimeDistanceGraph* g = uitest::graphOf(*r.w);
        QVERIFY(g != nullptr);
        QVERIFY(uitest::waitQuiet(*g));
        QVERIFY2(renderedAt(*g, 1.0), qPrintable(stateOf(*g)));
        HiDpiShell* hidpi = r.hidpi();
        QVERIFY(hidpi != nullptr && hidpi->watcher() != nullptr);
        DprWatcher& watcher = *hidpi->watcher();
        QVERIFY(watcher.watched().contains(r.w.get()));
        QCOMPARE(watcher.lastDpr(r.w.get()), 1.0);
        QSignalSpy spy(&watcher, &DprWatcher::dprChanged);
        const Look first = lookOf(*g);
        checkLook(first, first, 1.0, "the graph");
        int changes = watcher.changes();

        for (const qreal dpr : {2.0, 1.25, 1.5, 1.0}) {
            QVERIFY(moveTo(*r.w, dpr, kAt));
            ++changes;
            QTRY_COMPARE_WITH_TIMEOUT(watcher.changes(), changes, kTimeoutMs);
            QCOMPARE(watcher.lastDpr(r.w.get()), dpr);
            QCOMPARE(spy.last().at(0).value<QWidget*>(), static_cast<QWidget*>(r.w.get()));
            QCOMPARE(spy.last().at(2).toReal(), dpr);
            QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*g, dpr), qPrintable(stateOf(*g)), kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*g));
            for (ComponentWidget* c : r.w->canvas().components()) {
                QTRY_COMPARE_WITH_TIMEOUT(c->layoutDevicePixelRatio(), dpr, kTimeoutMs);
            }
            checkLook(lookOf(*g), first, dpr, "the graph");
        }
        // Back on the DPR 1 monitor: the same layout and the same pixels as before the trip.
        const Look back = lookOf(*g);
        QVERIFY(back.rects == first.rects);
        QCOMPARE(differsFromFirst(back, first, QStringLiteral("dpr_switch_window")), 0LL);
        QCOMPARE(spy.count(), 4);
    }

    void floatingDockAcrossMonitors_data() {
        QTest::addColumn<QString>("backend");
        for (const QString& name : DockHost::availableBackends()) {
            QTest::newRow(qPrintable(name)) << name;
        }
    }

    void floatingDockAcrossMonitors() {
        QFETCH(QString, backend);
        const QByteArray previous = qgetenv("ESSES_DOCK_BACKEND");
        qputenv("ESSES_DOCK_BACKEND", backend.toLatin1());
        const auto restore = qScopeGuard([&] {
            if (previous.isEmpty()) {
                qunsetenv("ESSES_DOCK_BACKEND");
            } else {
                qputenv("ESSES_DOCK_BACKEND", previous);
            }
        });
        Rig r;
        QVERIFY(r.start(1.0));
        QCOMPARE(QString::fromLatin1(r.w->docks().backend().name()), backend);
        TimeDistanceGraph* main = uitest::graphOf(*r.w);
        QVERIFY(main != nullptr);
        QVERIFY(uitest::waitQuiet(*main));
        HiDpiShell* hidpi = r.hidpi();
        QVERIFY(hidpi != nullptr && hidpi->watcher() != nullptr);
        DprWatcher& watcher = *hidpi->watcher();

        // A dock holding a time_distance over the active worksheet's context.
        WorksheetContext* context = r.w->trackMapWindow().context();
        QVERIFY(context != nullptr);
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("floatingGraphContent"));
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* graph =
            qobject_cast<TimeDistanceGraph*>(ComponentWidget::create(graphSpec(), *context, content));
        QVERIFY(graph != nullptr);
        layout->addWidget(graph);
        r.w->docks().addDock(QString::fromLatin1(kGraphDock), QStringLiteral("Graph"), content,
                             DockArea::Right, DockState::Docked);
        r.w->docks().focus(QString::fromLatin1(kGraphDock)); // its tab current where the area has several
        QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*graph, 1.0) && graph->isVisible(), qPrintable(stateOf(*graph)),
                                  kTimeoutMs);

        r.w->docks().setState(QString::fromLatin1(kGraphDock), DockState::Floating);
        QWidget* floating = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((floating = r.w->docks().windowOf(QString::fromLatin1(kGraphDock))) !=
                                         nullptr &&
                                     floating != r.w.get() && floating->isVisible(),
                                 kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(watcher.watched().contains(floating), kTimeoutMs); // the shell follows it
        floating->resize(720, 420);
        QVERIFY(moveTo(*floating, 1.0, QPoint(200, 260)));
        QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*graph, 1.0), qPrintable(stateOf(*graph)), kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*graph));
        const Look first = lookOf(*graph);
        checkLook(first, first, 1.0, "the floating graph");

        for (const qreal dpr : {2.0, 1.5, 1.0}) {
            QVERIFY(moveTo(*floating, dpr, QPoint(200, 260)));
            QTRY_COMPARE_WITH_TIMEOUT(watcher.lastDpr(floating), dpr, kTimeoutMs);
            QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*graph, dpr), qPrintable(stateOf(*graph)), kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*graph));
            checkLook(lookOf(*graph), first, dpr, "the floating graph");
            // Mixed DPI: the main window's graph stays at its own monitor's DPR.
            QCOMPARE(main->layoutDevicePixelRatio(), 1.0);
            QCOMPARE(main->layers().devicePixelRatio(), 1.0);
            QCOMPARE(watcher.lastDpr(r.w.get()), 1.0);
        }
        const Look back = lookOf(*graph);
        QVERIFY(back.rects == first.rects);
        QCOMPARE(differsFromFirst(back, first, QStringLiteral("dpr_switch_floating_") + backend), 0LL);

        // Floating on the DPR 2 monitor, then docked back into the DPR 1 window: Qt moves the widgets without
        // a DPR event. The graph rebuilds at DPR 1 through its resize when its docked size differs, else
        // through the shell's re-check after the dock change (that path alone: tst_dpr_watcher
        // reparentedComponentRebuildsOnRefresh); the re-check runs either way and leaves nothing at DPR 2.
        QVERIFY(moveTo(*floating, 2.0, QPoint(200, 260)));
        QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*graph, 2.0), qPrintable(stateOf(*graph)), kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*graph));
        const int before = hidpi->dockRefreshes();
        r.w->docks().setState(QString::fromLatin1(kGraphDock), DockState::Docked);
        QTRY_COMPARE_WITH_TIMEOUT(r.w->docks().windowOf(QString::fromLatin1(kGraphDock)),
                                  static_cast<QWidget*>(r.w.get()), kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(hidpi->dockRefreshes() > before, kTimeoutMs);
        QTRY_VERIFY2_WITH_TIMEOUT(renderedAt(*graph, 1.0), qPrintable(stateOf(*graph)), kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*graph));
        QCOMPARE(DprWatcher::refreshComponents(*r.w), 0); // every component of the window is at DPR 1
        const Look docked = lookOf(*graph);
        QCOMPARE(docked.staticSize, deviceSize(docked.geometry.size(), 1.0));
        QCOMPARE(docked.staticDpr, 1.0);
        QCOMPARE(docked.grabDiff, 0LL);
    }

    void trackMapDockAcrossMonitors() {
        Rig r;
        QVERIFY(r.start(1.0));
        HiDpiShell* hidpi = r.hidpi();
        QVERIFY(hidpi != nullptr && hidpi->watcher() != nullptr);
        const QString id = QStringLiteral("trackmap");
        r.w->docks().setState(id, DockState::Floating);
        QWidget* floating = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((floating = r.w->docks().windowOf(id)) != nullptr && floating != r.w.get() &&
                                     floating->isVisible(),
                                 kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(hidpi->watcher()->watched().contains(floating), kTimeoutMs);
        TrackPosition* map = r.w->trackMapWindow().trackPosition();
        QVERIFY(map != nullptr);
        floating->resize(420, 360);
        QVERIFY(moveTo(*floating, 1.0, QPoint(300, 300)));
        QTRY_VERIFY_WITH_TIMEOUT(map->layers().devicePixelRatio() == 1.0 && map->staticUpToDate(),
                                 kTimeoutMs);
        const QRect placed = map->geometry();
        QVERIFY(moveTo(*floating, 1.5, QPoint(300, 300)));
        QTRY_VERIFY_WITH_TIMEOUT(map->layoutDevicePixelRatio() == 1.5 &&
                                     map->layers().devicePixelRatio() == 1.5 &&
                                     uitest::poolSettled(r.rt->pool()) && map->staticUpToDate(),
                                 kTimeoutMs);
        QCOMPARE(map->geometry(), placed);
        QCOMPARE(map->layers().staticImage().size(), deviceSize(map->size(), 1.5));
        QCOMPARE(map->layers().staticImage().devicePixelRatio(), 1.5);
        QCOMPARE(differingPixels(map->grab().toImage(), map->layers().composite()), 0LL);
    }
};

QTEST_MAIN(TstDprSwitch)
#include "tst_dpr_switch.moc"
