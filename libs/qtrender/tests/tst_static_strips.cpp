// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RasterBackend::rasteriseStatic (M1 integration, AC-M1-02; 04 section 6.3 step 2 "worker build + raster"):
// the Static layer painted on a TaskPool worker in parallel vertical strips equals one
// LayerCache::rasteriseStatic pass pixel for pixel (tolerance 0): the geometry scene (grid, translucent band,
// 1 px and dashed 2 px traces, points, lines, quads, scissor) plus translucent and wide x-monotonic traces
// and rotated and clipped text, at DPR 1, 2 and 1.5 (one strip), and the eight time_distance golden cases
// built with QtTextMetrics at DPR 1 and 2; text at fractional positions across every strip edge with glyphs
// at subpixel positions (on every OS, see SubpixelGlyphs); the clear colour when the list does not start with
// a Clear; one strip on a NoBlock thread (never waits); and the spare buffer that LayerCache::storeStatic
// hands back is reused.

#include "../../plotcore/tests/td_golden_cases.hpp"
#include "qtrender_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QFont>
#include <QGuiApplication>
#include <QTest>
#include <cmath>
#include <memory>
#include <vector>

using namespace esses;

namespace {

// One pass on the calling thread, as the widgets did before and OffscreenRenderer still does.
QImage singlePass(const plot::DrawList& list, const std::shared_ptr<const qt::SchemeFonts>& fonts,
                  plot::Rgba clear) {
    qt::LayerCache cache(fonts);
    cache.resize(QSize(list.widthPx, list.heightPx), 1.0);
    cache.rasteriseStatic(list, clear);
    return cache.staticImage();
}

// Strips on a worker of `pool` (the widget's path).
QImage onWorker(TaskPool& pool, const plot::DrawList& list,
                const std::shared_ptr<const qt::SchemeFonts>& fonts, plot::Rgba clear) {
    auto f = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
        return qt::RasterBackend::rasteriseStatic(list, *fonts, clear);
    });
    Result<QImage> r = std::move(f).take();
    return r ? std::move(r).value() : QImage();
}

// The geometry scene plus what the strips must split exactly: a translucent and a 3 px monotonic trace
// crossing every strip, a monotonic trace with a gap, and text (rotated, elided, with a background) on strip
// edges.
plot::DrawList stripScene(float dpr) {
    plot::DrawList l = qttest::geometryScene(dpr);
    const auto d = [dpr](double logical) { return static_cast<float>(logical * static_cast<double>(dpr)); };
    std::vector<float> a;
    std::vector<float> b;
    std::vector<float> c;
    for (int i = 0; i <= 230; ++i) {
        const double x = 5.0 + i;
        a.push_back(d(x));
        a.push_back(d(100.0 + 30.0 * std::sin(i / 7.0)));
        b.push_back(d(x));
        b.push_back(d(60.0 + 25.0 * std::cos(i / 11.0)));
        c.push_back(d(x));
        c.push_back(i == 120 ? std::numeric_limits<float>::quiet_NaN() : d(130.0 + 10.0 * std::sin(i / 3.0)));
    }
    l.polyline(a.data(), a.size() / 2, 0x8E24AA80U, d(1.0)); // translucent
    l.polyline(b.data(), b.size() / 2, 0x00838FFFU, d(3.0)); // wide, antialiased coverage runs
    l.polyline(c.data(), c.size() / 2, 0xEF6C00FFU, d(1.0)); // a gap: walked whole
    l.textRun("strip edge text", d(70.0), d(40.0), plot::TextStyleId::Axis, plot::TextAnchor::Centre,
              0x202124FFU);
    l.textRun("rotated", d(80.0), d(150.0), plot::TextStyleId::Axis, plot::TextAnchor::BottomLeft,
              0x202124FFU, 0.0F, 1);
    l.textRun("elided caption over strips", d(10.0), d(2.0), plot::TextStyleId::Caption,
              plot::TextAnchor::TopLeft, 0x202124FFU, d(100.0), 0, 0xE8EAEDFFU);
    return l;
}

// Glyphs at subpixel positions on every OS while it lives. SchemeFonts never sets a hinting preference, so
// QPainter resolves it from the application font: PreferNoHinting makes the FreeType engine (Linux, and the
// offscreen platform on Windows) place glyphs at quarter pixels, as fontconfig's default slight hinting does
// on the Linux CI; CoreText (macOS) uses subpixel offsets anyway. Windows' FreeType default is full hinting
// with glyphs at whole pixels, which hid a 1/64 px strip/one-pass difference in where a run starting left of
// a strip edge lands (the strips painted a translated view of their columns; QPainter truncates the device
// position to 26.6 fixed point toward zero, so a negative x moved right by 1/64 px and could pick the next
// quarter-pixel glyph image: ubuntu showed 26-118 pixels with a channel delta of 56, a quarter of the text's
// contrast).
class SubpixelGlyphs {
public:
    SubpixelGlyphs() : saved_(QGuiApplication::font()) {
        QFont f = saved_;
        f.setHintingPreference(QFont::PreferNoHinting);
        QGuiApplication::setFont(f);
    }
    ~SubpixelGlyphs() { QGuiApplication::setFont(saved_); }
    SubpixelGlyphs(const SubpixelGlyphs&) = delete;
    SubpixelGlyphs& operator=(const SubpixelGlyphs&) = delete;

private:
    QFont saved_;
};

// Text across every strip edge whatever the strip count: rows of runs whose anchors step through fractions of
// a device pixel, in four styles and anchors, so runs start left of each edge at many subpixel offsets;
// rotated runs every few pixels; and single glyphs whose ink reaches past their advance box (a j to the left,
// an f and a slash to the right) swept across the width in steps under one pixel, so that some box edge lies
// just beside each strip edge and only the glyph's overhang crosses it.
plot::DrawList subpixelTextScene(float dpr) {
    plot::DrawList l = qttest::emptyList(1024, 640, dpr);
    l.clear(0xFFFFFFFFU);
    const auto d = [dpr](double logical) { return static_cast<float>(logical * static_cast<double>(dpr)); };
    const plot::TextStyleId styles[] = {plot::TextStyleId::Axis, plot::TextStyleId::Caption,
                                        plot::TextStyleId::Key, plot::TextStyleId::KeyValue};
    const plot::TextAnchor anchors[] = {plot::TextAnchor::TopLeft, plot::TextAnchor::TopCentre,
                                        plot::TextAnchor::TopRight, plot::TextAnchor::MiddleLeft};
    const char* const texts[] = {"Corr Speed 123.4 km/h", "Lap 2  -0:01.375  Throttle %", "jounce Diff"};
    for (int row = 0; row < 16; ++row) {
        const double y = 4.0 + 13.0 * row;
        for (int col = 0; col < 9; ++col) {
            // 1/7 of a device pixel more per row, and 0.37 logical px more per column.
            const double x = 6.0 + 117.37 * col + row / (7.0 * static_cast<double>(dpr));
            l.textRun(texts[(row + col) % 3], d(x), d(y), styles[(row + col) % 4],
                      anchors[(row / 4 + col) % 4], 0x202124FFU);
        }
    }
    for (int k = 0; k < 110; ++k) {
        l.textRun("Brake Pres 12.5 bar", d(3.0 + 9.3 * k + 0.13 * (k % 7)), d(255.0), plot::TextStyleId::Axis,
                  plot::TextAnchor::BottomLeft, 0x1565C0FFU, 0.0F, 1);
    }
    for (int k = 0; k < 1100; ++k) { // 0.93 logical px apart, eight rows so neighbours do not overlap
        const double x = 0.05 + 0.93 * k;
        const double y = 262.0 + 16.0 * (k % 8);
        l.textRun("j", d(x), d(y), plot::TextStyleId::Caption, plot::TextAnchor::TopLeft, 0x202124FFU);
        l.textRun("f", d(x), d(y + 128.0), plot::TextStyleId::Caption, plot::TextAnchor::TopRight,
                  0x202124FFU);
        l.textRun("/", d(x), d(y + 256.0), plot::TextStyleId::Caption, plot::TextAnchor::TopRight,
                  0x202124FFU);
    }
    return l;
}

void expectEqual(const QImage& actual, const QImage& expected, const QString& name) {
    QVERIFY(!actual.isNull());
    QCOMPARE(actual.size(), expected.size());
    QCOMPARE(actual.devicePixelRatio(), expected.devicePixelRatio());
    const qt::ImageDiff d = qt::compareImages(actual, expected, 0, true);
    if (d.differingPixels != 0) {
        qttest::saveArtefact(actual, name + QStringLiteral(".strips"));
        qttest::saveArtefact(expected, name + QStringLiteral(".single"));
        qttest::saveArtefact(d.diff, name + QStringLiteral(".diff"));
    }
    QVERIFY2(d.differingPixels == 0, qPrintable(name + QStringLiteral(": ") + d.summary()));
}

} // namespace

class TstStaticStrips : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(qttest::registerGoldenFonts() > 0); }

    void sceneEqualsOnePass_data() {
        QTest::addColumn<float>("dpr");
        QTest::addColumn<bool>("clearsFirst");
        QTest::newRow("dpr1") << 1.0F << true;
        QTest::newRow("dpr2") << 2.0F << true;
        QTest::newRow("dpr1.5 (one strip)") << 1.5F << true;
        QTest::newRow("dpr2, no leading Clear") << 2.0F << false;
    }
    void sceneEqualsOnePass() {
        QFETCH(float, dpr);
        QFETCH(bool, clearsFirst);
        TaskPool pool(4);
        const auto fonts = qttest::testFonts();
        plot::DrawList list = stripScene(dpr);
        if (!clearsFirst) {
            list.cmds.erase(list.cmds.begin()); // the scene starts with its Clear
        }
        const plot::Rgba clear = 0x10203040U;
        expectEqual(onWorker(pool, list, fonts, clear), singlePass(list, fonts, clear),
                    QStringLiteral("strips_scene@%1x").arg(static_cast<double>(dpr)));
    }

    // Regression (pull request #1, ubuntu qt-app): text crossing strip edges at subpixel positions.
    void subpixelTextEqualsOnePass_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void subpixelTextEqualsOnePass() {
        QFETCH(float, dpr);
        const SubpixelGlyphs subpixel;
        TaskPool pool(4);
        const auto fonts = qttest::testFonts();
        const plot::DrawList list = subpixelTextScene(dpr);
        expectEqual(onWorker(pool, list, fonts, 0), singlePass(list, fonts, 0),
                    QStringLiteral("strips_subpixel_text@%1x").arg(static_cast<double>(dpr)));
    }

    void goldenCasesEqualOnePass() {
        TaskPool pool(4);
        const plot::test::Fixture fx;
        for (const plot::test::GoldenCase& gc : plot::test::goldenCases(fx)) {
            for (const float dpr : {1.0F, 2.0F}) {
                plot::ComponentBuildInputs in = plot::test::inputsOf(fx, gc, dpr);
                in.metrics = qt::QtTextMetrics::create(*in.scheme, 1);
                const auto fonts = std::make_shared<const qt::SchemeFonts>(*in.scheme, 1);
                Result<plot::ComponentBuild> built =
                    plot::TimeDistanceBuilder::buildStatic(in, CancelToken{});
                QVERIFY(built.ok());
                expectEqual(onWorker(pool, built->list, fonts, 0), singlePass(built->list, fonts, 0),
                            QStringLiteral("strips_%1@%2x")
                                .arg(QString::fromStdString(gc.name))
                                .arg(static_cast<double>(dpr)));
            }
        }
    }

    // On a NoBlock (GUI) thread the strips are not used: no Future is waited for.
    void noBlockThreadPaintsOneStrip() {
        TaskPool pool(4);
        const auto fonts = qttest::testFonts();
        const plot::DrawList list = stripScene(2.0F);
        ThreadRole::set(ThreadRole::NoBlock);
        const QImage img = qt::RasterBackend::rasteriseStatic(list, *fonts, 0, &pool);
        ThreadRole::set(ThreadRole::Normal);
        expectEqual(img, singlePass(list, fonts, 0), QStringLiteral("strips_noblock"));
    }

    // LayerCache::storeStatic hands the replaced image of the same size back; the next worker raster reuses
    // it.
    void storedBuffersAreReused() {
        TaskPool pool(4);
        const auto fonts = qttest::testFonts();
        const plot::DrawList list = stripScene(1.0F);
        QImage first = onWorker(pool, list, fonts, 0);
        QImage second = onWorker(pool, list, fonts, 0);
        QVERIFY(!first.isNull() && !second.isNull());
        const uchar* firstBits = first.constBits();
        qt::LayerCache cache(fonts);
        cache.resize(QSize(list.widthPx, list.heightPx), 1.0);
        cache.storeStatic(std::move(first));
        cache.storeStatic(std::move(second)); // `first` goes to the spare buffers
        const QImage third = onWorker(pool, list, fonts, 0);
        QCOMPARE(third.constBits(), firstBits);
        expectEqual(third, singlePass(list, fonts, 0), QStringLiteral("strips_reused"));
        QVERIFY(qt::RasterBackend::rasteriseStatic(plot::DrawList{}, *fonts).isNull());
    }
};

QTEST_MAIN(TstStaticStrips)
#include "tst_static_strips.moc"
