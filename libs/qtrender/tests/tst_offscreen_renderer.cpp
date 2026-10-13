// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OffscreenRenderer (04 section 6.7): Static builds run on the TaskPool and Dynamic/Chrome on the calling
// thread; the output equals the LayerCache composite of the same lists (the AC-M1-05 mechanism); rendering is
// deterministic; worksheets place components by their fractional rectangles in z order at DPR 1 and 2;
// unknown types and failed builds render the placeholder.

#include "qtrender_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>

#include <QGuiApplication>
#include <QTest>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

using namespace esses;
using esses::qttest::pixel;
using esses::qttest::sameColour;

namespace {

plot::DrawList sized(const plot::ComponentBuildInputs& in) {
    plot::DrawList l;
    l.widthPx = in.layout.widthPx;
    l.heightPx = in.layout.heightPx;
    l.dpr = in.layout.dpr;
    return l;
}

// A component that fills itself with `colour`, draws a trace, a cursor and a caption.
qt::ComponentBuilders solidBuilders(plot::Rgba colour) {
    qt::ComponentBuilders b;
    b.buildStatic = [colour](const plot::ComponentBuildInputs& in,
                             const CancelToken&) -> Result<plot::ComponentBuild> {
        plot::ComponentBuild build;
        build.list = sized(in);
        build.list.clear(colour);
        std::vector<float> trace;
        for (int x = 0; x < in.layout.widthPx; x += 3) {
            trace.push_back(static_cast<float>(x));
            trace.push_back(static_cast<float>(in.layout.heightPx / 2 + (x % 11) - 5));
        }
        build.list.polyline(trace.data(), trace.size() / 2, 0x000000FFU, in.layout.dpr);
        build.viewVersion = in.view.version;
        return build;
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
        plot::DrawList l = sized(in);
        const float x = static_cast<float>(in.layout.widthPx) / 3.0F + 0.5F;
        const std::vector<float> cursor = {x, 0.0F, x, static_cast<float>(in.layout.heightPx)};
        l.polyline(cursor.data(), 2, 0xFF3B30FFU, in.layout.dpr);
        return l;
    };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool) {
        plot::DrawList l = sized(in);
        l.textRun("Caption", 4.0F * in.layout.dpr, 2.0F * in.layout.dpr, plot::TextStyleId::Caption,
                  plot::TextAnchor::TopLeft, 0x202020FFU);
        return l;
    };
    return b;
}

project::ComponentSpec spec(const char* type, project::Rect rect, int z) {
    project::ComponentSpec s;
    s.type = type;
    s.rect = rect;
    s.z = z;
    return s;
}

} // namespace

class TestOffscreenRenderer : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { qttest::registerGoldenFonts(); }

    void renderComponentSizeAndDpr_data() {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<QSize>("device");
        QTest::newRow("dpr1") << 1.0 << QSize(40, 30);
        QTest::newRow("dpr1.5") << 1.5 << QSize(60, 45);
        QTest::newRow("dpr2") << 2.0 << QSize(80, 60);
    }
    void renderComponentSizeAndDpr() {
        QFETCH(qreal, dpr);
        QFETCH(QSize, device);
        qt::OffscreenRenderer renderer(qttest::testFonts());
        renderer.setBuilders("solid", solidBuilders(0x00FF00FFU));
        plot::ComponentBuildInputs in;
        in.spec = spec("solid", {}, 0);
        const QImage img = renderer.renderComponent(in, QSize(40, 30), dpr);
        QCOMPARE(img.size(), device);
        QCOMPARE(img.devicePixelRatio(), dpr);
        QVERIFY(sameColour(pixel(img, device.width() - 2, device.height() - 2), 0xFF00FF00U));
        QVERIFY(renderer.renderComponent(in, QSize(0, 30), dpr).isNull());
    }

    void staticOnThePoolDynamicAndChromeOnTheCaller() {
        TaskPool pool(2);
        std::mutex mutex;
        std::vector<std::thread::id> staticThreads;
        std::vector<std::thread::id> callerThreads;
        qt::ComponentBuilders b = solidBuilders(0xFFFFFFFFU);
        const auto inner = b.buildStatic;
        b.buildStatic = [&, inner](const plot::ComponentBuildInputs& in, const CancelToken& c) {
            {
                const std::scoped_lock lock(mutex);
                staticThreads.push_back(std::this_thread::get_id());
            }
            return inner(in, c);
        };
        const auto innerDynamic = b.buildDynamic;
        b.buildDynamic = [&, innerDynamic](const plot::ComponentBuildInputs& in) {
            callerThreads.push_back(std::this_thread::get_id());
            return innerDynamic(in);
        };
        qt::OffscreenRenderer renderer(qttest::testFonts(), &pool);
        renderer.setBuilders("probe", b);
        plot::ComponentBuildInputs in;
        in.spec = spec("probe", {}, 0);
        QVERIFY(!renderer.renderComponent(in, QSize(50, 20), 1.0).isNull());
        QCOMPARE(staticThreads.size(), std::size_t{1});
        QVERIFY(staticThreads[0] != std::this_thread::get_id());
        QCOMPARE(callerThreads.size(), std::size_t{1});
        QCOMPARE(callerThreads[0], std::this_thread::get_id());
        pool.join();
    }

    void withoutAPoolTheStaticBuildRunsInline() {
        if (TaskPool::installed() != nullptr) {
            QSKIP("a TaskPool is installed in this process");
        }
        std::thread::id seen;
        qt::ComponentBuilders b = solidBuilders(0xFFFFFFFFU);
        const auto inner = b.buildStatic;
        b.buildStatic = [&seen, inner](const plot::ComponentBuildInputs& in, const CancelToken& c) {
            seen = std::this_thread::get_id();
            return inner(in, c);
        };
        qt::OffscreenRenderer renderer(qttest::testFonts());
        renderer.setBuilders("probe", b);
        plot::ComponentBuildInputs in;
        in.spec = spec("probe", {}, 0);
        QVERIFY(!renderer.renderComponent(in, QSize(10, 10), 1.0).isNull());
        QCOMPARE(seen, std::this_thread::get_id());
    }

    void equalsTheLayerCacheComposite_data() {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("dpr1") << 1.0;
        QTest::newRow("dpr2") << 2.0;
    }
    void equalsTheLayerCacheComposite() {
        QFETCH(qreal, dpr);
        const auto fonts = qttest::testFonts();
        TaskPool pool(2);
        qt::OffscreenRenderer renderer(fonts, &pool);
        const qt::ComponentBuilders b = solidBuilders(0xF5F5F5FFU);
        renderer.setBuilders("solid", b);
        plot::ComponentBuildInputs in;
        in.spec = spec("solid", {}, 0);
        in.scheme = std::make_shared<const project::Scheme>(fonts->scheme());
        const QSize logical(180, 90);
        const QImage offscreen = renderer.renderComponent(in, logical, dpr);

        // What a ComponentWidget does: the same lists through its LayerCache, painted under the exposed
        // region.
        qt::LayerCache cache(fonts);
        cache.resize(logical, dpr);
        plot::ComponentBuildInputs widgetInputs = in;
        widgetInputs.layout = {cache.deviceSize().width(), cache.deviceSize().height(),
                               static_cast<float>(dpr)};
        cache.rasteriseStatic(b.buildStatic(widgetInputs, CancelToken::none()).value().list);
        cache.setDynamic(b.buildDynamic(widgetInputs));
        cache.setChrome(b.buildChrome(widgetInputs, false));
        QImage onScreen(cache.deviceSize(), QImage::Format_ARGB32_Premultiplied);
        onScreen.setDevicePixelRatio(dpr);
        onScreen.fill(Qt::transparent);
        {
            QPainter p(&onScreen);
            cache.paint(p, QRegion(0, 0, logical.width(), logical.height()));
        }
        const qt::ImageDiff diff = qt::compareImages(offscreen, onScreen, 0);
        QVERIFY2(diff.passes(0.0), qPrintable(diff.summary()));
        qttest::saveArtefact(offscreen, QStringLiteral("offscreen_component@%1x").arg(dpr));
        pool.join();
    }

    void renderingIsDeterministic() {
        TaskPool pool(4);
        qt::OffscreenRenderer renderer(qttest::testFonts(), &pool);
        renderer.setBuilders("solid", solidBuilders(0xFFFFFFFFU));
        project::WorkbookFile wb;
        project::WorksheetDef sheet;
        for (int i = 0; i < 6; ++i) {
            const double l = i / 6.0;
            sheet.components.push_back(spec("solid", {l, 0.0, l + 1.0 / 6.0, 1.0}, i));
        }
        wb.worksheets.push_back(sheet);
        const qt::OffscreenRenderer::InputsProvider provider = [](const project::ComponentSpec& s,
                                                                  const plot::LayoutContext& layout) {
            plot::ComponentBuildInputs in;
            in.spec = s;
            in.layout = layout;
            return in;
        };
        const QImage a = renderer.render(wb, 0, provider, QSize(300, 120), 2.0);
        const QImage b = renderer.render(wb, 0, provider, QSize(300, 120), 2.0);
        QVERIFY(!a.isNull());
        QCOMPARE(a, b);
        pool.join();
    }

    void worksheetPlacesComponentsByRectAndZ_data() {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("dpr1") << 1.0;
        QTest::newRow("dpr2") << 2.0;
    }
    void worksheetPlacesComponentsByRectAndZ() {
        QFETCH(qreal, dpr);
        qt::OffscreenRenderer renderer(qttest::testFonts());
        renderer.setBuilders("red", solidBuilders(0xFF0000FFU));
        renderer.setBuilders("blue", solidBuilders(0x0000FFFFU));
        project::WorkbookFile wb;
        project::WorksheetDef sheet;
        sheet.components.push_back(spec("blue", {0.0, 0.0, 0.5, 1.0}, 2));  // on top (higher z)
        sheet.components.push_back(spec("red", {0.25, 0.0, 0.75, 1.0}, 1)); // below it
        sheet.components.push_back(spec("red", {0.0, 0.0, 0.0, 1.0}, 0));   // empty: skipped
        wb.worksheets.push_back(sheet);
        const QImage img = renderer.render(wb, 0, {}, QSize(200, 100), dpr);
        QCOMPARE(img.size(), QSize(200, 100) * dpr);
        const auto at = [&](int x, int y) {
            return pixel(img, static_cast<int>(x * dpr), static_cast<int>(y * dpr));
        };
        QVERIFY(sameColour(at(80, 90), 0xFF0000FFU));  // blue over red where they overlap
        QVERIFY(sameColour(at(120, 90), 0xFFFF0000U)); // red alone
        QVERIFY(sameColour(at(190, 90), 0xFFF0F0F0U)); // the scheme's windowBackground
        qttest::saveArtefact(img, QStringLiteral("offscreen_worksheet@%1x").arg(dpr));
        QVERIFY(renderer.render(wb, 1, {}, QSize(200, 100), dpr).isNull());
        QVERIFY(renderer.render(wb, -1, {}, QSize(200, 100), dpr).isNull());
    }

    void unknownTypesRenderThePlaceholder() {
        const auto fonts = qttest::testFonts();
        qt::OffscreenRenderer renderer(fonts);
        plot::ComponentBuildInputs in;
        in.spec = spec("future_gauge", {}, 0);
        in.spec.known = false;
        in.scheme = std::make_shared<const project::Scheme>(fonts->scheme());
        const QImage img = renderer.renderComponent(in, QSize(120, 60), 1.0);
        QVERIFY(sameColour(pixel(img, 0, 30), 0xFF3A3D41U)); // cptFrame
        QVERIFY(sameColour(pixel(img, 5, 5), 0xFFFFFFFFU));  // cptBackground
        const QRect ink = qttest::inkBounds(img.copy(2, 2, 116, 56), 0xFFFFFFFFU, 48);
        QVERIFY(!ink.isNull()); // the type name
    }

    void failedBuildsRenderTheErrorPlaceholder() {
        qt::OffscreenRenderer renderer(qttest::testFonts());
        qt::ComponentBuilders b;
        b.buildStatic = [](const plot::ComponentBuildInputs&,
                           const CancelToken&) -> Result<plot::ComponentBuild> {
            return fail(ErrorCode::Unsupported, "not today");
        };
        renderer.setBuilders("broken", b); // missing Dynamic/Chrome builders fall back to empty lists
        plot::ComponentBuildInputs in;
        in.spec = spec("broken", {}, 0);
        const QImage img = renderer.renderComponent(in, QSize(200, 60), 1.0);
        QVERIFY(!img.isNull());
        QVERIFY(sameColour(pixel(img, 5, 5), 0xFFFFFFFFU));
        QVERIFY(!qttest::inkBounds(img.copy(2, 2, 196, 56), 0xFFFFFFFFU, 48).isNull());
    }

    void builtinTimeDistanceIsWired() {
        const qt::ComponentBuilders b = qt::OffscreenRenderer::builtinBuilders("time_distance");
        QVERIFY(b.buildStatic && b.buildDynamic && b.buildChrome);
        qt::OffscreenRenderer renderer(qttest::testFonts());
        plot::ComponentBuildInputs in;
        in.spec = spec("time_distance", {}, 0);
        const QImage img = renderer.renderComponent(in, QSize(160, 90), 2.0);
        QCOMPARE(img.size(), QSize(320, 180));
    }

    // The shipped default workbook and schemes (the esses-render demo of 07 section 4.5) render without data:
    // every component lands at its rectangle over the scheme's window background (the Values window column is
    // a placeholder until a Qt-free values_list builder exists).
    void shippedWorkbookRendersWithoutData_data() {
        QTest::addColumn<QString>("scheme");
        QTest::newRow("light") << QStringLiteral("light");
        QTest::newRow("dark") << QStringLiteral("dark");
    }
    void shippedWorkbookRendersWithoutData() {
        QFETCH(QString, scheme);
        const std::filesystem::path data = std::filesystem::path(qttest::repoRoot().toStdWString()) / "data";
        std::vector<std::string> log;
        const auto workbook =
            project::WorkbookFile::load(data / "workbooks" / "circuit-default.workbook.json", log);
        QVERIFY2(workbook.ok(), workbook.ok() ? "" : workbook.error().message.c_str());
        const auto loaded =
            project::Scheme::load(data / "schemes" / (scheme.toStdString() + ".scheme.json"), log);
        QVERIFY2(loaded.ok(), loaded.ok() ? "" : loaded.error().message.c_str());
        const auto shared = std::make_shared<const project::Scheme>(loaded.value());
        const auto fonts = std::make_shared<const qt::SchemeFonts>(*shared);
        TaskPool pool(2);
        qt::OffscreenRenderer renderer(fonts, &pool);
        const qt::OffscreenRenderer::InputsProvider provider = [&](const project::ComponentSpec& s,
                                                                   const plot::LayoutContext& layout) {
            plot::ComponentBuildInputs in;
            in.spec = s;
            in.layout = layout;
            in.scheme = shared;
            return in;
        };
        for (std::size_t w = 0; w < workbook->worksheets.size(); ++w) {
            const QImage img =
                renderer.render(*workbook, static_cast<int>(w), provider, QSize(800, 450), 2.0);
            QCOMPARE(img.size(), QSize(1600, 900));
            qttest::saveArtefact(img, QStringLiteral("shipped_%1_ws%2@2x").arg(scheme).arg(w));
        }
        pool.join();
    }
};

QTEST_MAIN(TestOffscreenRenderer)
#include "tst_offscreen_renderer.moc"
