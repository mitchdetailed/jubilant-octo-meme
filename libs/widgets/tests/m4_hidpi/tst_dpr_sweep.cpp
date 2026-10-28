// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M4 HiDPI sweep (07 section 7.1 row 3, AC-M4-07; 04 section 6.6; 06 section 17; D-025, D-209): one
// worksheet holding all ten MVP component types (time_distance, outing, scatter, histogram, channel_report,
// time_report, track_position, values_list, gauge_numeric, gauge_bar), laid out from scratch in a window on
// each of four offscreen screens at DPR 1, 1.25, 1.5 and 2 (dpr_test_support.hpp: 96/120/144/192 dpi, the
// way Windows reports a 100/125/150/200 % monitor). For every component and DPR:
//   * no layout drift in logical units: the canvas places it at the same logical rectangle at every DPR (the
//     1 % grid, 06 section 17 item 1), and its own layout (the time_distance trace area, key column, caption
//     and x axis; the panel scissors of every drawn type; the report and values tables' rows and columns)
//     stays within one logical pixel (plus a device pixel's rounding) of the DPR 1 layout;
//   * rendered at device pixels (04 section 6.6 items 1-2): the layers are at the window's DPR, the Static
//     image is lround(logical x DPR) device pixels and tagged with the DPR, the three DrawLists carry it;
//   * crisp: what the widget paints is its layer composite pixel for pixel, in the widget (QWidget::grab) and
//     in its window at the device rectangle Qt gives it (no image of another DPR scaled into place, so no
//     blur); and at DPR 1.25 and 1.5 the axis-aligned geometry of the Static, Dynamic and Chrome lists covers
//     whole device pixels as RasterBackend draws them (04 section 6.6 item 4, D-209; dpr_test_support.hpp
//     crispnessProblems). At DPR 1 and 2 the backend draws as the authoritative goldens show (D-025); what
//     that check finds there is reported (qInfo), not enforced (docs/milestones/M4.md, hidpi open issues).
// The expectations come from the design rules and from the DPR 1 run, never from the code under test at the
// same DPR. The DrawList JSON goldens at DPR 1.25 and 1.5 (exact, FixedTextMetrics) are
// tests/golden/images/tst_dpr_goldens.cpp's.

#include "components/plots_test_support.hpp"
#include "components/reports_track/rt_support.hpp"
#include "m4_hidpi/dpr_test_support.hpp"

#include <esses/base/cancel.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/layered_component.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/components/values_list.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QElapsedTimer>
#include <QHeaderView>
#include <QPixmap>
#include <QTableView>
#include <QTest>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::dprtest;

namespace {

constexpr int kTimeoutMs = 60'000;
constexpr const char* kVenue = "Sweep Venue";
constexpr QSize kCanvasSize{1210, 790};
// Allowed difference of a layout coordinate from its DPR 1 value, in logical pixels: the DPR 1 layout rounds
// to whole pixels and the DPR d one to whole device pixels (04 section 6.6 item 4), each of up to two
// rounded terms (a position is a sum of snapped sizes), 2 px; and the rows sized by text are measured with
// QFontMetricsF at the device pixel size (D-025), where the hinted line height of the golden font differs
// from the DPR 1 one by up to a logical pixel, 1 px more. (tst_dpr_goldens, with FixedTextMetrics, holds the
// builders to the 2 px of the rounding alone.)
constexpr double kDriftLogical = 3.0;

struct Placed {
    const char* type;
    project::Rect rect;
    const char* body; // nullptr: the cmptest spec helper's body
};

// Three rows; the fractions give logical edges that are not multiples of 4 (fractional device positions at
// 1.25 and 1.5).
const std::vector<Placed>& placements() {
    static const std::vector<Placed> all{
        {"time_distance", {0.0, 0.0, 0.5, 0.4}, nullptr},
        {"outing",
         {0.5, 0.0, 0.75, 0.4},
         R"({"groups":[{"name":"Speed","traces":[{"channel":"Wheel Speed FL"}]},)"
         R"({"name":"Throttle","traces":[{"channel":"Throttle"}]}]})"},
        {"values_list", {0.75, 0.0, 1.0, 0.4}, nullptr},
        {"scatter",
         {0.0, 0.4, 0.33, 0.7},
         R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Throttle"}],"cursorStyle":"ring"})"},
        {"histogram",
         {0.33, 0.4, 0.66, 0.7},
         R"({"traces":[{"channel":"Throttle"}],"bins":{"mode":"even","bandMode":"width",)"
         R"("dataMin":{"v":0,"unit":"%"},"dataMax":{"v":100,"unit":"%"},"bandWidth":{"v":10,"unit":"%"},)"
         R"("lessMinBand":false,"greaterMaxBand":false},"yMode":"percent"})"},
        {"track_position",
         {0.66, 0.4, 1.0, 0.7},
         R"({"showZoomSpan":true,"showSectionLabels":true,"showSectionMarks":true,"showSectionColours":true})"},
        {"gauge_numeric", {0.0, 0.7, 0.17, 1.0}, R"({"channel":"Throttle","displayDps":1})"},
        {"gauge_bar",
         {0.17, 0.7, 0.33, 1.0},
         R"({"channel":"Throttle","scale":{"mode":"manual","min":{"v":0,"unit":"%"},)"
         R"("max":{"v":100,"unit":"%"}}})"},
        {"channel_report",
         {0.33, 0.7, 0.66, 1.0},
         R"({"report":{"scope":"selectedRanges","channels":[{"name":"Throttle","stats":["avg","max"]},)"
         R"({"name":"Wheel Speed FL","stats":["min","max"]}],"options":{}},"layout":"channelsInRows",)"
         R"("timeFormat":"project"})"},
        {"time_report",
         {0.66, 0.7, 1.0, 1.0},
         R"({"scope":"selectedOutings","rowGroup":"Outings:Laps:Track Sections:Default",)"
         R"("summaries":{"eclectic":true,"rollingMin":false,"total":true},"timeFormat":"s.mmm"})"},
    };
    return all;
}

std::string idOfIndex(std::size_t i) {
    return "5e1f0000-0000-4000-8000-0000000000" + std::to_string(10 + i);
}

project::WorksheetDef sweepSheet(const World& w) {
    project::WorksheetDef def = w.sheet;
    def.components.clear();
    const std::vector<Placed>& all = placements();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Placed& p = all[i];
        project::ComponentSpec spec;
        const std::string type = p.type;
        if (type == "time_distance") {
            spec = timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}, p.rect, 0, idOfIndex(i));
        } else if (type == "values_list") {
            spec = valuesListSpec({"Wheel Speed FL", "Throttle"}, p.rect, 0, idOfIndex(i));
        } else {
            spec.id = Uuid::parse(idOfIndex(i));
            spec.type = type;
            spec.rect = p.rect;
            spec.body = JsonDoc::parse(p.body).value();
        }
        spec.z = static_cast<int>(i);
        def.components.push_back(std::move(spec));
    }
    return def;
}

// The pieces of a drawn component the checks read (nullptr layers for the table components).
struct Drawn {
    const qt::LayerCache* layers = nullptr;
    const plot::ComponentBuildInputs* inputs = nullptr;
    bool upToDate = true;
    int submitted = 0;
};

Drawn drawnOf(const ComponentWidget* c) {
    Drawn d;
    if (const auto* g = qobject_cast<const TimeDistanceGraph*>(c)) {
        d = Drawn{&g->layers(), &g->inputs(), g->staticUpToDate(), g->staticBuildsSubmitted()};
    } else if (const auto* l = qobject_cast<const LayeredComponent*>(c)) {
        d = Drawn{&l->layers(), &l->inputs(), l->staticUpToDate(), l->staticBuildsSubmitted()};
    } else if (const auto* t = qobject_cast<const TrackPosition*>(c)) {
        d = Drawn{&t->layers(), &t->inputs(), t->staticUpToDate(), t->staticSubmitted()};
    }
    return d;
}

bool reportBusy(const ComponentWidget* c) {
    if (const auto* r = qobject_cast<const ChannelReport*>(c)) {
        return r->model().busy();
    }
    if (const auto* r = qobject_cast<const TimeReport*>(c)) {
        return r->model().busy();
    }
    return false;
}

bool allSettled(World& w, const QList<ComponentWidget*>& cs) {
    if (!poolSettled(w.rt.pool())) {
        return false;
    }
    return std::all_of(cs.begin(), cs.end(),
                       [](const ComponentWidget* c) { return drawnOf(c).upToDate && !reportBusy(c); });
}

int submittedSum(const QList<ComponentWidget*>& cs) {
    int n = 0;
    for (const ComponentWidget* c : cs) {
        n += drawnOf(c).submitted;
    }
    return n;
}

// Settled, and still settled with no new Static build after a quiet period (a trailing size-throttle timer
// or a pending-value retry may still request one).
bool waitSettled(World& w, const QList<ComponentWidget*>& cs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kTimeoutMs) {
        if (!QTest::qWaitFor([&] { return allSettled(w, cs); }, kTimeoutMs)) {
            return false;
        }
        const int before = submittedSum(cs);
        QTest::qWait(200);
        if (allSettled(w, cs) && submittedSum(cs) == before) {
            return true;
        }
    }
    return false;
}

QRectF logicalOf(const plot::RectPx& r, double dpr) {
    return {static_cast<double>(r.x) / dpr, static_cast<double>(r.y) / dpr, static_cast<double>(r.w) / dpr,
            static_cast<double>(r.h) / dpr};
}

double drift(const QRectF& a, const QRectF& b) {
    return std::max({std::abs(a.x() - b.x()), std::abs(a.y() - b.y()), std::abs(a.width() - b.width()),
                     std::abs(a.height() - b.height())});
}

std::vector<QRectF> scissorsOf(const plot::DrawList& list, double dpr) {
    std::vector<QRectF> out;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind == plot::CmdKind::Scissor && c.rect.w > 0.0F) {
            out.push_back(logicalOf(c.rect, dpr));
        }
    }
    return out;
}

// What one DPR's run measured of one component.
struct Measure {
    QString type;
    QRect geometry; // logical, in the canvas
    qreal layoutDpr = 0.0;
    qreal widgetDpr = 0.0;
    // drawn components
    bool drawn = false;
    qreal layersDpr = 0.0;
    QSize staticSize;
    qreal staticDpr = 0.0;
    double dynamicDpr = 0.0;
    double chromeDpr = 0.0;
    double staticListDpr = 0.0;
    QSize staticListSize;
    QStringList alignment;
    std::vector<QRectF> scissors; // logical
    long long grabDiff = -2;      // widget grab vs layer composite
    long long windowDiff = -2;    // window grab at the device rectangle vs layer composite
    // time_distance
    std::map<std::string, QRectF> tdRects; // logical
    // tables
    QSize grabSize;
    qreal grabDpr = 0.0;
    std::vector<int> columns;
    std::vector<int> rows;
};

QTableView* mainTable(ComponentWidget* c) {
    if (auto* r = qobject_cast<ChannelReport*>(c)) {
        return r->tableView();
    }
    if (auto* r = qobject_cast<TimeReport*>(c)) {
        return r->tableView();
    }
    return c->findChild<QTableView*>();
}

// `paintedLater`: the device rectangles (in `window`) of the components painted after `c`.
Measure measure(ComponentWidget* c, QWidget& window, const QImage& windowGrab, qreal dpr,
                const QList<QRect>& paintedLater) {
    Measure m;
    m.type = QString::fromLatin1(c->type());
    m.geometry = c->geometry();
    m.layoutDpr = c->layoutDevicePixelRatio();
    m.widgetDpr = c->devicePixelRatioF();
    const Drawn d = drawnOf(c);
    if (d.layers == nullptr) {
        const QPixmap grab = c->grab();
        m.grabSize = grab.size();
        m.grabDpr = grab.devicePixelRatio();
        if (QTableView* t = mainTable(c); t != nullptr && t->model() != nullptr) {
            for (int col = 0; col < t->model()->columnCount(); ++col) {
                m.columns.push_back(t->columnWidth(col));
            }
            for (int row = 0; row < t->model()->rowCount(); ++row) {
                m.rows.push_back(t->rowHeight(row));
            }
            m.rows.push_back(t->horizontalHeader()->height());
        }
        return m;
    }
    m.drawn = true;
    const qt::LayerCache& layers = *d.layers;
    m.layersDpr = layers.devicePixelRatio();
    m.staticSize = layers.staticImage().size();
    m.staticDpr = layers.staticImage().devicePixelRatio();
    m.dynamicDpr = static_cast<double>(layers.dynamicList().dpr);
    m.chromeDpr = static_cast<double>(layers.chromeList().dpr);
    // The Static list of the component's own inputs (the build it rasterised), from the same builders.
    const qt::ComponentBuilders builders = qt::OffscreenRenderer::builtinBuilders(c->type());
    const Result<plot::ComponentBuild> built = builders.buildStatic(*d.inputs, CancelToken{});
    if (built) {
        const plot::DrawList& list = built.value().list;
        m.staticListDpr = static_cast<double>(list.dpr);
        m.staticListSize = QSize(list.widthPx, list.heightPx);
        m.scissors = scissorsOf(list, dpr);
        m.alignment << crispnessProblems(list, QStringLiteral("static"), true);
    } else {
        m.alignment
            << QStringLiteral("static build failed: %1").arg(QString::fromStdString(built.error().message));
    }
    m.alignment << crispnessProblems(layers.dynamicList(), QStringLiteral("dynamic"), false);
    m.alignment << crispnessProblems(layers.chromeList(), QStringLiteral("chrome"), false);
    const QImage composite = layers.composite();
    m.grabDiff = differingPixels(c->grab().toImage(), composite);
    const QRect device = deviceRectOf(QRect(c->mapTo(&window, QPoint(0, 0)), c->size()), dpr);
    // Where a component painted later shares a boundary column or row (deviceRectOf), that one shows; a
    // last column or row that rounds past the window's device edge is not on screen at all.
    QList<QRect> masked;
    for (const QRect& r : paintedLater) {
        if (r.intersects(device)) {
            masked << r.intersected(device).translated(-device.topLeft());
        }
    }
    const QRect onScreen(QPoint(0, 0), windowGrab.size());
    if (device.right() > onScreen.right()) {
        masked << QRect(onScreen.right() + 1 - device.left(), 0, device.right() - onScreen.right(),
                        device.height());
    }
    if (device.bottom() > onScreen.bottom()) {
        masked << QRect(0, onScreen.bottom() + 1 - device.top(), device.width(),
                        device.bottom() - onScreen.bottom());
    }
    m.windowDiff = device.size() == composite.size()
                       ? differingPixelsOutside(windowGrab.copy(device), composite, masked)
                       : -1;
    if (const auto* g = qobject_cast<const TimeDistanceGraph*>(c)) {
        const plot::TdLayout& lay = g->layout();
        m.tdRects["traceArea"] = logicalOf(lay.traceArea, dpr);
        m.tdRects["keyColumn"] = logicalOf(lay.keyColumn, dpr);
        m.tdRects["caption"] = logicalOf(lay.caption, dpr);
        m.tdRects["xAxis"] = logicalOf(lay.xAxis, dpr);
        m.tdRects["bounds"] = logicalOf(lay.bounds, dpr);
    }
    return m;
}

bool fractional(qreal dpr) {
    return std::abs(dpr - std::round(dpr)) > 1e-9;
}

QSize deviceSize(QSize logical, qreal dpr) {
    return {static_cast<int>(std::lround(logical.width() * dpr)),
            static_cast<int>(std::lround(logical.height() * dpr))};
}

} // namespace

class TstDprSweep : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        prepareFontEnvironment();
        useSweepScreens("tst_dpr_sweep", std::filesystem::path(ESSES_TEST_BINARY_DIR));
    }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
        for (const ScreenSpec& s : kScreens) {
            QScreen* screen = screenNamed(QString::fromLatin1(s.name));
            QVERIFY2(screen != nullptr, s.name);
            QCOMPARE(screen->devicePixelRatio(), s.dpr);
            QCOMPARE(screen->geometry().size(), QSize(kScreenLogicalW, kScreenLogicalH));
        }
    }

    // Every component type at every DPR of the sweep (see the header).
    void sweep() {
        std::map<double, std::vector<Measure>> runs;
        for (const ScreenSpec& s : kScreens) {
            const qreal dpr = s.dpr;
            World w;
            qt::TrackMapService maps(nullptr);
            w.render.setTrackMaps(&maps);
            static_cast<void>(
                maps.setMap(kVenue, std::make_shared<const analysis::TrackMap>(qt::rttest::rectMap(kVenue))));
            auto src = std::make_shared<model::DataSource>(*makeSource(SourceOptions{}, w.rt.cache()));
            src->details.push_back(qt::rttest::textDetail("Venue", kVenue));
            src->baseDetails = src->details;
            rtw::publish(w, src);
            plotstest::selectMain(w, lapRef(*src, 1));
            w.view->setCursor(12 * plotstest::kSec, qt::CursorSource::Programmatic);
            {
                QWidget host;
                host.setScreen(screenNamed(QString::fromLatin1(s.name)));
                host.setGeometry(QRect(pointOn(dpr, QPoint(40, 40)), kCanvasSize));
                auto* canvas = new WorksheetCanvas(&host);
                canvas->setGeometry(QRect(QPoint(0, 0), kCanvasSize));
                canvas->setWorksheet(sweepSheet(w), *w.context);
                host.show();
                QVERIFY(QTest::qWaitForWindowExposed(&host));
                QCOMPARE(host.devicePixelRatioF(), dpr);
                const QList<ComponentWidget*> cs = canvas->components();
                QCOMPARE(cs.size(), static_cast<qsizetype>(placements().size()));
                QVERIFY2(waitSettled(w, cs), qPrintable(QStringLiteral("DPR %1 never settled").arg(dpr)));
                const QImage windowGrab = host.grab().toImage();
                QCOMPARE(windowGrab.size(), deviceSize(kCanvasSize, dpr));
                std::vector<Measure>& out = runs[dpr];
                for (qsizetype i = 0; i < cs.size(); ++i) { // paint order
                    QList<QRect> later;
                    for (qsizetype k = i + 1; k < cs.size(); ++k) {
                        later << deviceRectOf(QRect(cs[k]->mapTo(&host, QPoint(0, 0)), cs[k]->size()), dpr);
                    }
                    out.push_back(measure(cs[i], host, windowGrab, dpr, later));
                }
            }
            poolSettled(w.rt.pool());
        }

        const std::vector<Measure>& base = runs.at(1.0);
        double worstDrift = 0.0;
        for (const auto& [dprKey, ms] : runs) {
            const qreal dpr = dprKey;
            QCOMPARE(ms.size(), base.size());
            for (std::size_t i = 0; i < ms.size(); ++i) {
                const Measure& m = ms[i];
                const Measure& b = base[i];
                const QString at = QStringLiteral("%1 at DPR %2").arg(m.type, dprText(dpr));
                QVERIFY2(m.type == b.type, qPrintable(at));
                // Logical placement: the same 1 % grid rectangle at every DPR.
                QVERIFY2(m.geometry == b.geometry, qPrintable(at + QStringLiteral(": placement moved")));
                QVERIFY2(m.widgetDpr == dpr, qPrintable(at));
                QVERIFY2(m.layoutDpr == dpr,
                         qPrintable(at + QStringLiteral(": laid out at %1").arg(m.layoutDpr)));
                if (!m.drawn) {
                    // Qt draws the tables at the window's DPR; their rows and columns keep their logical
                    // size.
                    QVERIFY2(m.grabDpr == dpr && m.grabSize == deviceSize(m.geometry.size(), dpr),
                             qPrintable(at + QStringLiteral(": grab %1x%2 at %3")
                                                 .arg(m.grabSize.width())
                                                 .arg(m.grabSize.height())
                                                 .arg(m.grabDpr)));
                    QVERIFY2(m.columns.size() == b.columns.size() && m.rows.size() == b.rows.size(),
                             qPrintable(at + QStringLiteral(": table shape")));
                    for (std::size_t k = 0; k < m.columns.size(); ++k) {
                        worstDrift =
                            std::max(worstDrift, std::abs(static_cast<double>(m.columns[k] - b.columns[k])));
                        QVERIFY2(std::abs(m.columns[k] - b.columns[k]) <= kDriftLogical,
                                 qPrintable(at + QStringLiteral(": column %1 is %2, %3 at DPR 1")
                                                     .arg(k)
                                                     .arg(m.columns[k])
                                                     .arg(b.columns[k])));
                    }
                    for (std::size_t k = 0; k < m.rows.size(); ++k) {
                        worstDrift =
                            std::max(worstDrift, std::abs(static_cast<double>(m.rows[k] - b.rows[k])));
                        QVERIFY2(std::abs(m.rows[k] - b.rows[k]) <= kDriftLogical,
                                 qPrintable(at + QStringLiteral(": row %1 is %2, %3 at DPR 1")
                                                     .arg(k)
                                                     .arg(m.rows[k])
                                                     .arg(b.rows[k])));
                    }
                    continue;
                }
                // Device pixels: layers, Static image and lists at the window's DPR.
                const QSize device = deviceSize(m.geometry.size(), dpr);
                QVERIFY2(m.layersDpr == dpr, qPrintable(at));
                QVERIFY2(m.staticSize == device && m.staticDpr == dpr,
                         qPrintable(at + QStringLiteral(": Static image %1x%2 at %3, expected %4x%5")
                                             .arg(m.staticSize.width())
                                             .arg(m.staticSize.height())
                                             .arg(m.staticDpr)
                                             .arg(device.width())
                                             .arg(device.height())));
                QVERIFY2(m.dynamicDpr == dpr && m.chromeDpr == dpr && m.staticListDpr == dpr,
                         qPrintable(at + QStringLiteral(": list DPR")));
                QVERIFY2(m.staticListSize == device, qPrintable(at + QStringLiteral(": Static list size")));
                // Crisp: whole device pixels at a fractional DPR (reported at 1 and 2), and painted 1:1.
                if (fractional(dpr)) {
                    QVERIFY2(m.alignment.isEmpty(),
                             qPrintable(at + QStringLiteral(": ") + m.alignment.join(QStringLiteral("; "))));
                } else if (!m.alignment.isEmpty()) {
                    qInfo("%s (authoritative rendering, not enforced): %s", qPrintable(at),
                          qPrintable(m.alignment.join(QStringLiteral("; "))));
                }
                QVERIFY2(m.grabDiff == 0, qPrintable(at + QStringLiteral(": the widget paints %1 pixels that "
                                                                         "differ from its layers")
                                                              .arg(m.grabDiff)));
                QVERIFY2(m.windowDiff == 0, qPrintable(at + QStringLiteral(": in its window %1 pixels differ "
                                                                           "from its layers")
                                                                .arg(m.windowDiff)));
                // No drift: the panels and, for time_distance, the main layout rectangles.
                QVERIFY2(m.scissors.size() == b.scissors.size(),
                         qPrintable(at + QStringLiteral(": %1 panels, %2 at DPR 1")
                                             .arg(m.scissors.size())
                                             .arg(b.scissors.size())));
                for (std::size_t k = 0; k < m.scissors.size(); ++k) {
                    const double dd = drift(m.scissors[k], b.scissors[k]);
                    worstDrift = std::max(worstDrift, dd);
                    QVERIFY2(dd <= kDriftLogical,
                             qPrintable(at + QStringLiteral(": panel %1 moved %2 px").arg(k).arg(dd)));
                }
                for (const auto& [name, r] : m.tdRects) {
                    const double dd = drift(r, b.tdRects.at(name));
                    worstDrift = std::max(worstDrift, dd);
                    QVERIFY2(
                        dd <= kDriftLogical,
                        qPrintable(
                            at +
                            QStringLiteral(": %1 moved %2 px").arg(QString::fromStdString(name)).arg(dd)));
                }
            }
        }
        qInfo("largest layout difference from DPR 1: %.3f logical px", worstDrift);
    }
};

QTEST_MAIN(TstDprSweep)
#include "tst_dpr_sweep.moc"
