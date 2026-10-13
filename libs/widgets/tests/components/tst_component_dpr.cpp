// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HiDPI (04 section 6.6, 06 section 17), offscreen with two screens configured through the offscreen
// platform's configfile: "lo" at 96 dpi (DPR 1) and "hi" at 192 dpi (DPR 2). A graph laid out on "hi" builds
// its layers at device size (DrawList dpr 2, Static image 2x, lines and fonts scaled in the builders); moving
// the window from "lo" to "hi" delivers a DPR change that rebuilds every layer at the new DPR while the
// canvas placement stays in logical pixels.

#include "components/component_test_support.hpp"

#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QGuiApplication>
#include <QScreen>
#include <QTest>
#include <QWindow>
#include <fstream>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr const char* kGraphId = "7a2d1f9e-1a51-4f5a-9d43-0a7d2d5f0011";
constexpr int kTimeoutMs = 30'000;

QScreen* screenNamed(const QString& name) {
    for (QScreen* s : QGuiApplication::screens()) {
        if (s->name() == name) {
            return s;
        }
    }
    return nullptr;
}

project::WorksheetDef sheetOf(const World& w) {
    project::WorksheetDef def = w.sheet;
    def.components.push_back(
        timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}, project::Rect{0.0, 0.0, 0.5, 1.0}, 0, kGraphId));
    return def;
}

} // namespace

class TstComponentDpr : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        prepareFontEnvironment();
        // The platform argument list is ':'-separated, so the file is named relative to the working
        // directory (a Windows drive letter would split the argument).
        std::filesystem::current_path(std::filesystem::path(ESSES_TEST_BINARY_DIR));
        const std::filesystem::path config = "tst_component_dpr.screens.json";
        std::ofstream out(config, std::ios::binary | std::ios::trunc);
        out << R"({"synchronousWindowSystemEvents": false, "windowFrameMargins": false, "screens": [)"
            << R"({"name": "lo", "x": 0, "y": 0, "width": 1600, "height": 1000, "logicalDpi": 96,)"
            << R"( "logicalBaseDpi": 96, "dpr": 1.0},)"
            << R"({"name": "hi", "x": 1600, "y": 0, "width": 1600, "height": 1000, "logicalDpi": 192,)"
            << R"( "logicalBaseDpi": 96, "dpr": 1.0}]})";
        out.close();
        qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + config.generic_string()).c_str());
    }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
        QVERIFY(screenNamed(QStringLiteral("lo")) != nullptr);
        QVERIFY(screenNamed(QStringLiteral("hi")) != nullptr);
        QCOMPARE(screenNamed(QStringLiteral("lo"))->devicePixelRatio(), 1.0);
        QCOMPARE(screenNamed(QStringLiteral("hi"))->devicePixelRatio(), 2.0);
    }

    void layersAtDeviceSizeOnAHighDprScreen() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        QWidget host;
        host.setScreen(screenNamed(QStringLiteral("hi")));
        host.setGeometry(1700, 100, 800, 400);
        auto* canvas = new WorksheetCanvas(&host);
        canvas->setGeometry(0, 0, 800, 400);
        canvas->setWorksheet(sheetOf(w), *w.context);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto* g = qobject_cast<TimeDistanceGraph*>(canvas->componentById(Uuid::parse(kGraphId)));
        QVERIFY(g != nullptr);
        QCOMPARE(g->devicePixelRatioF(), 2.0);
        QTRY_VERIFY_WITH_TIMEOUT(g->staticUpToDate(), kTimeoutMs);
        QCOMPARE(g->geometry(), QRect(0, 0, 400, 400)); // logical placement
        QCOMPARE(g->layers().devicePixelRatio(), 2.0);
        QCOMPARE(g->layers().staticImage().size(), QSize(800, 800));
        QCOMPARE(g->layers().staticImage().devicePixelRatio(), 2.0);
        QCOMPARE(g->layout().dpr, 2.0F);
        QCOMPARE(g->inputs().layout.widthPx, 800);
        QCOMPARE(g->layers().dynamicList().dpr, 2.0F);
        QCOMPARE(g->layoutDevicePixelRatio(), 2.0);
        // The cursor maps logical mouse positions through the device-px layout.
        const plot::RectPx t = g->layout().traceArea;
        const double x = (static_cast<double>(t.x) + 0.5 * static_cast<double>(t.w)) / 2.0;
        QVERIFY(std::abs(g->logicalXOf(*g->domainXAt(x)) - x) < 0.5);
    }

    void dprChangeRebuildsEveryLayer() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        QWidget host;
        host.setScreen(screenNamed(QStringLiteral("lo")));
        host.setGeometry(100, 100, 800, 400);
        auto* canvas = new WorksheetCanvas(&host);
        canvas->setGeometry(0, 0, 800, 400);
        canvas->setWorksheet(sheetOf(w), *w.context);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto* g = qobject_cast<TimeDistanceGraph*>(canvas->componentById(Uuid::parse(kGraphId)));
        QTRY_VERIFY_WITH_TIMEOUT(g->staticUpToDate(), kTimeoutMs);
        QCOMPARE(g->layers().devicePixelRatio(), 1.0);
        QCOMPARE(g->layers().staticImage().size(), QSize(400, 400));
        const int delivered = g->staticBuildsDelivered();
        const int chrome = g->chromeBuilds();
        const QRect placed = g->geometry();

        // To the 2x screen.
        host.windowHandle()->setScreen(screenNamed(QStringLiteral("hi")));
        host.move(1700, 100);
        QTRY_COMPARE_WITH_TIMEOUT(g->devicePixelRatioF(), 2.0, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(g->layers().devicePixelRatio() == 2.0 && g->staticUpToDate(), kTimeoutMs);
        QVERIFY(g->staticBuildsDelivered() > delivered);
        QVERIFY(g->chromeBuilds() > chrome); // Size|Scheme: every layer
        QCOMPARE(g->layers().staticImage().size(), QSize(800, 800));
        QCOMPARE(g->layout().dpr, 2.0F);
        QCOMPARE(g->geometry(), placed); // logical placement unchanged
    }
};

QTEST_MAIN(TstComponentDpr)
#include "tst_component_dpr.moc"
