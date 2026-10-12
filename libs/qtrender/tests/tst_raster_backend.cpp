// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RasterBackend (04 section 6.1): layer life cycle and free-list, every DrawList command at DPR 1 and 2
// against exact pixel expectations, text placement against QtTextMetrics, compose/readback, and the synthetic
// geometry scenes against the committed reference PNGs (DrawList -> PNG at DPR 1, 1.5 and 2).

#include "qtrender_test_support.hpp"

#include <esses/plotcore/text_metrics.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QGuiApplication>
#include <QTest>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace esses;
using esses::qttest::emptyList;
using esses::qttest::inkBounds;
using esses::qttest::pixel;
using esses::qttest::rasterise;
using esses::qttest::sameColour;

namespace {

constexpr QRgb kWhite = 0xFFFFFFFFU;
constexpr plot::Rgba kRed = 0xFF0000FFU;
constexpr QRgb kRedQ = 0xFFFF0000U;

std::size_t addCmd(plot::DrawList& l, plot::CmdKind kind, const std::vector<float>& xy, plot::Rgba colour,
                   float widthPx = 1.0F, float pointSizePx = 0.0F) {
    plot::DrawCmd cmd;
    cmd.kind = kind;
    cmd.vertexOffset = static_cast<std::uint32_t>(l.verts.size() / 2);
    cmd.vertexCount = static_cast<std::uint32_t>(xy.size() / 2);
    cmd.colour = colour;
    cmd.widthPx = widthPx;
    cmd.pointSizePx = pointSizePx;
    l.verts.insert(l.verts.end(), xy.begin(), xy.end());
    l.cmds.push_back(cmd);
    return l.cmds.size() - 1;
}

} // namespace

class TestRasterBackend : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { qttest::registerGoldenFonts(); }

    void layerLifecycle() {
        qt::RasterBackend backend(qttest::testFonts(), std::make_shared<const plot::FixedTextMetrics>());
        backend.beginLayer(plot::LayerKind::Static, 64, 32, 2.0F, 0xFFFFFFFFU);
        const plot::LayerHandle h = backend.endLayer();
        QVERIFY(h.id != 0);
        const QImage img = backend.image(h);
        QCOMPARE(img.size(), QSize(64, 32));
        QCOMPARE(img.devicePixelRatio(), 2.0);
        QCOMPARE(img.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(backend.liveLayers(), std::size_t{1});
        QVERIFY(backend.textMetrics() != nullptr);
        backend.release(h);
        QVERIFY(backend.image(h).isNull());
        QCOMPARE(backend.liveLayers(), std::size_t{0});
        QCOMPARE(backend.freeLayers(), std::size_t{1});
    }

    void freeListReusesBuffersOfTheSameSize() {
        qt::RasterBackend backend(qttest::testFonts(), nullptr);
        backend.beginLayer(plot::LayerKind::Static, 100, 50, 1.0F, 0);
        const plot::LayerHandle a = backend.endLayer();
        const uchar* bits = backend.image(a).constBits();
        backend.release(a);
        backend.beginLayer(plot::LayerKind::Static, 100, 50, 1.0F, 0xFF0000FFU);
        const plot::LayerHandle b = backend.endLayer();
        QCOMPARE(backend.image(b).constBits(), bits); // no allocation at constant size
        QCOMPARE(backend.freeLayers(), std::size_t{0});
        QVERIFY(sameColour(pixel(backend.image(b), 5, 5), kRedQ)); // cleared to the new colour
        backend.release(b);
        // A different size allocates; the free-list is capped.
        std::vector<plot::LayerHandle> handles;
        for (int i = 0; i < 8; ++i) {
            backend.beginLayer(plot::LayerKind::Static, 10 + i, 10, 1.0F, 0);
            handles.push_back(backend.endLayer());
        }
        for (const auto& h : handles) {
            backend.release(h);
        }
        QCOMPARE(backend.freeLayers(), qt::RasterBackend::kMaxFreeLayers);
    }

    void beginWhileOpenReleasesTheOpenLayer() {
        qt::RasterBackend backend(nullptr, nullptr);
        backend.beginLayer(plot::LayerKind::Static, 8, 8, 1.0F, 0);
        backend.beginLayer(plot::LayerKind::Dynamic, 8, 8, 1.0F, 0);
        const plot::LayerHandle h = backend.endLayer();
        QCOMPARE(backend.liveLayers(), std::size_t{1});
        QVERIFY(!backend.image(h).isNull());
        backend.draw(plot::DrawList{}); // outside a layer: ignored
    }

    void clearUsesSourceComposition() {
        plot::DrawList l = emptyList(20, 10, 1.0F);
        l.clear(0x00000000U);
        const QImage img = rasterise(l, 0xFF0000FFU);
        QCOMPARE(qAlpha(pixel(img, 3, 3)), 0); // replaced, not blended
        plot::DrawList m = emptyList(20, 10, 1.0F);
        m.clear(0x00FF0080U);
        const QImage half = rasterise(m, 0xFFFFFFFFU);
        QVERIFY2(sameColour(pixel(half, 19, 9), qRgba(0, 255, 0, 128), 1),
                 qPrintable(QString::number(pixel(half, 19, 9), 16)));
    }

    void filledRectSnapsToDevicePixels_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void filledRectSnapsToDevicePixels() {
        QFETCH(float, dpr);
        plot::DrawList l = emptyList(60, 40, dpr);
        l.filledRect({10.4F * dpr, 5.6F * dpr, 20.2F * dpr, 10.0F * dpr}, kRed);
        const QImage img = rasterise(l);
        const auto scale = static_cast<double>(dpr);
        const int x0 = static_cast<int>(std::lround(10.4 * scale));
        const int x1 = static_cast<int>(std::lround(30.6 * scale));
        const int y0 = static_cast<int>(std::lround(5.6 * scale));
        const int y1 = static_cast<int>(std::lround(15.6 * scale));
        QCOMPARE(inkBounds(img, kWhite, 0), QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1)));
        QVERIFY(sameColour(pixel(img, x0, y0), kRedQ));
        QVERIFY(sameColour(pixel(img, x1 - 1, y1 - 1), kRedQ));
    }

    void onePixelPolylineIsAliasedAndSnapped_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void onePixelPolylineIsAliasedAndSnapped() {
        QFETCH(float, dpr);
        plot::DrawList l = emptyList(80, 40, dpr);
        // Device-pixel coordinates: a 1 device px line through the centre of row 10 and of column 30.
        const std::vector<float> h = {2.0F, 10.5F, 50.0F, 10.5F};
        const std::vector<float> v = {30.5F, 15.0F, 30.5F, 35.0F};
        l.polyline(h.data(), 2, kRed, 1.0F);
        l.polyline(v.data(), 2, kRed, 1.0F);
        const QImage img = rasterise(l);
        for (int x = 2; x <= 50; ++x) {
            QVERIFY2(sameColour(pixel(img, x, 10), kRedQ), qPrintable(QString::number(x)));
            QVERIFY(sameColour(pixel(img, x, 9), kWhite));
            QVERIFY(sameColour(pixel(img, x, 11), kWhite));
        }
        for (int y = 15; y <= 35; ++y) {
            QVERIFY(sameColour(pixel(img, 30, y), kRedQ));
            QVERIFY(sameColour(pixel(img, 29, y), kWhite));
            QVERIFY(sameColour(pixel(img, 31, y), kWhite));
        }
    }

    void widePolylineIsAntialiased() {
        plot::DrawList l = emptyList(80, 80, 1.0F);
        const std::vector<float> diag = {5.0F, 5.0F, 70.0F, 60.0F};
        l.polyline(diag.data(), 2, kRed, 3.0F);
        const QImage img = rasterise(l);
        int partial = 0;
        int full = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QRgb p = pixel(img, x, y);
                if (sameColour(p, kRedQ)) {
                    ++full;
                } else if (!sameColour(p, kWhite)) {
                    ++partial;
                }
            }
        }
        QVERIFY(full > 100);
        QVERIFY(partial > 50); // coverage-blended edges
    }

    void polylineSlicesAreContinuous() {
        plot::DrawList l = emptyList(120, 40, 1.0F);
        std::vector<float> xy;
        for (int i = 0; i < 10000; ++i) { // > two 4,096-point slices
            xy.push_back(1.0F + static_cast<float>(i) * 0.01F);
            xy.push_back(20.5F);
        }
        l.polyline(xy.data(), xy.size() / 2, kRed, 1.0F);
        const QImage img = rasterise(l);
        for (int x = 1; x <= 100; ++x) {
            QVERIFY2(sameColour(pixel(img, x, 20), kRedQ), qPrintable(QString::number(x)));
        }
    }

    void nonFiniteVertexBreaksThePolyline() {
        plot::DrawList l = emptyList(80, 20, 1.0F);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<float> xy = {0.0F, 10.5F, 20.0F, 10.5F, nan, nan, 40.0F, 10.5F, 60.0F, 10.5F};
        l.polyline(xy.data(), xy.size() / 2, kRed, 1.0F);
        const QImage img = rasterise(l);
        QVERIFY(sameColour(pixel(img, 5, 10), kRedQ));
        QVERIFY(sameColour(pixel(img, 50, 10), kRedQ));
        for (int x = 22; x < 39; ++x) {
            QVERIFY(sameColour(pixel(img, x, 10), kWhite));
        }
    }

    void dashedLineHasGaps() {
        plot::DrawList l = emptyList(100, 20, 1.0F);
        const std::vector<float> xy = {2.0F, 10.5F, 96.0F, 10.5F};
        l.polyline(xy.data(), 2, kRed, 1.0F, plot::DashId::Dash);
        const QImage img = rasterise(l);
        int on = 0;
        int off = 0;
        for (int x = 2; x <= 96; ++x) {
            (sameColour(pixel(img, x, 10), kRedQ) ? on : off) += 1;
        }
        QVERIFY(on > 30);
        QVERIFY(off > 10);
    }

    void outOfRangeCommandsAreIgnored() {
        plot::DrawList l = emptyList(20, 20, 1.0F);
        plot::DrawCmd bad;
        bad.kind = plot::CmdKind::Polyline;
        bad.vertexOffset = 10;
        bad.vertexCount = 4;
        l.cmds.push_back(bad);
        plot::DrawCmd badText;
        badText.kind = plot::CmdKind::TextRun;
        badText.textIndex = 3;
        l.cmds.push_back(badText);
        const QImage img = rasterise(l);
        QVERIFY(inkBounds(img, kWhite, 0).isNull());
    }

    void pointsAndLines() {
        plot::DrawList l = emptyList(60, 40, 1.0F);
        addCmd(l, plot::CmdKind::Points, {5.5F, 5.5F, 7.2F, 9.9F}, kRed, 1.0F, 1.0F);
        addCmd(l, plot::CmdKind::Points, {30.0F, 10.0F}, kRed, 1.0F, 5.0F);
        addCmd(l, plot::CmdKind::Lines,
               {40.5F, 20.0F, 40.5F, 30.0F, 50.5F, 20.0F, 50.5F, 30.0F, 55.5F, 20.0F}, kRed,
               1.0F); // the fifth (unpaired) vertex is ignored
        const QImage img = rasterise(l);
        QVERIFY(sameColour(pixel(img, 5, 5), kRedQ));
        QVERIFY(sameColour(pixel(img, 7, 9), kRedQ));
        QVERIFY(sameColour(pixel(img, 6, 6), kWhite));
        const QImage dot = img.copy(25, 5, 11, 11);
        const QRect ink = inkBounds(dot, kWhite);
        QVERIFY(ink.width() >= 4 && ink.width() <= 7);
        for (int y = 20; y <= 30; ++y) {
            QVERIFY(sameColour(pixel(img, 40, y), kRedQ));
            QVERIFY(sameColour(pixel(img, 50, y), kRedQ));
        }
        QVERIFY(sameColour(pixel(img, 45, 25), kWhite));
        QVERIFY(sameColour(pixel(img, 55, 20), kWhite));
    }

    void quads() {
        plot::DrawList l = emptyList(80, 60, 1.0F);
        addCmd(l, plot::CmdKind::Quads, {10.0F, 10.0F, 30.0F, 10.0F, 30.0F, 20.0F, 10.0F, 20.0F}, kRed);
        addCmd(l, plot::CmdKind::Quads, {60.0F, 20.0F, 75.0F, 35.0F, 60.0F, 50.0F, 45.0F, 35.0F}, kRed);
        const QImage img = rasterise(l);
        QCOMPARE(inkBounds(img.copy(0, 0, 40, 30), kWhite, 0), QRect(10, 10, 20, 10)); // exact fill
        QVERIFY(sameColour(pixel(img, 60, 35), kRedQ));                                // diamond centre
        QVERIFY(sameColour(pixel(img, 46, 21), kWhite));                               // outside the diamond
        int partial = 0;
        for (int x = 44; x < 77; ++x) {
            const QRgb p = pixel(img, x, 27);
            partial += !sameColour(p, kRedQ) && !sameColour(p, kWhite) ? 1 : 0;
        }
        QVERIFY(partial >= 2); // antialiased slanted edges
    }

    void scissorClipsAndRestores_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void scissorClipsAndRestores() {
        QFETCH(float, dpr);
        plot::DrawList l = emptyList(60, 40, dpr);
        l.scissor({10.0F * dpr, 10.0F * dpr, 20.0F * dpr, 15.0F * dpr});
        l.filledRect({0.0F, 0.0F, 60.0F * dpr, 40.0F * dpr}, kRed);
        l.scissor({0.0F, 0.0F, 0.0F, 0.0F});
        l.filledRect({50.0F * dpr, 30.0F * dpr, 5.0F * dpr, 5.0F * dpr}, 0x0000FFFFU);
        const QImage img = rasterise(l);
        const auto s = [dpr](float v) { return static_cast<int>(v * dpr); };
        QCOMPARE(inkBounds(img.copy(0, 0, s(40.0F), s(30.0F)), kWhite, 0),
                 QRect(s(10.0F), s(10.0F), s(20.0F), s(15.0F)));
        QVERIFY(sameColour(pixel(img, s(52.0F), s(32.0F)), 0xFF0000FFU)); // after the restore
    }

    void scissorNeverEscapesThePainterClip() {
        QImage img(40, 40, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setClipRect(QRect(0, 0, 20, 40));
        plot::DrawList l = emptyList(40, 40, 1.0F);
        l.scissor({10.0F, 0.0F, 30.0F, 40.0F});
        l.filledRect({0.0F, 0.0F, 40.0F, 40.0F}, kRed);
        l.scissor({0.0F, 0.0F, 0.0F, 0.0F});
        l.filledRect({0.0F, 30.0F, 40.0F, 10.0F}, kRed);
        qt::RasterBackend::paintList(p, l, *qttest::testFonts());
        QVERIFY(p.hasClipping()); // the painter's own state is restored
        p.end();
        QCOMPARE(inkBounds(img.copy(0, 0, 40, 30), kWhite, 0), QRect(10, 0, 10, 30));
        QCOMPARE(inkBounds(img.copy(0, 30, 40, 10), kWhite, 0), QRect(0, 0, 20, 10)); // base clip restored
    }

    void textAnchorsMatchTheMetrics_data() {
        QTest::addColumn<int>("anchor");
        QTest::addColumn<float>("dpr");
        for (int a = 0; a < 9; ++a) {
            for (const float dpr : {1.0F, 2.0F}) {
                QTest::addRow("anchor%d@%gx", a, static_cast<double>(dpr)) << a << dpr;
            }
        }
    }
    void textAnchorsMatchTheMetrics() {
        QFETCH(int, anchor);
        QFETCH(float, dpr);
        const auto fonts = qttest::testFonts();
        const qt::QtTextMetrics metrics(fonts->scheme(), 1);
        const std::string text = "Speed 123.4";
        const float x = 120.0F * dpr;
        const float y = 40.0F * dpr;
        plot::DrawList l = emptyList(240, 80, dpr);
        l.textRun(text, x, y, plot::TextStyleId::Key, static_cast<plot::TextAnchor>(anchor), 0x000000FFU);
        const QImage img = rasterise(l, 0xFFFFFFFFU, fonts);
        const float w = metrics.advancePx(plot::TextStyleId::Key, text, dpr);
        const float h = metrics.lineHeightPx(plot::TextStyleId::Key, dpr);
        const float left = x - (anchor % 3 == 0 ? 0.0F : anchor % 3 == 1 ? w / 2.0F : w);
        const float top = y - (anchor / 3 == 0 ? 0.0F : anchor / 3 == 1 ? h / 2.0F : h);
        const QRect box = QRectF(static_cast<qreal>(left), static_cast<qreal>(top), static_cast<qreal>(w),
                                 static_cast<qreal>(h))
                              .toAlignedRect()
                              .adjusted(-2, -2, 2, 2);
        const QRect ink = inkBounds(img, kWhite, 48);
        QVERIFY2(!ink.isNull(), "no text was drawn");
        QVERIFY2(box.contains(ink), qPrintable(QStringLiteral("ink %1,%2 %3x%4 outside box %5,%6 %7x%8")
                                                   .arg(ink.x())
                                                   .arg(ink.y())
                                                   .arg(ink.width())
                                                   .arg(ink.height())
                                                   .arg(box.x())
                                                   .arg(box.y())
                                                   .arg(box.width())
                                                   .arg(box.height())));
        QVERIFY(ink.width() > static_cast<int>(w * 0.7F)); // the run is not clipped or shrunk
        qttest::saveArtefact(img,
                             QStringLiteral("text_anchor%1@%2x").arg(anchor).arg(static_cast<double>(dpr)));
    }

    void textScalesWithTheDpr() {
        const auto fonts = qttest::testFonts();
        QRect ink[2];
        for (int i = 0; i < 2; ++i) {
            const float dpr = i == 0 ? 1.0F : 2.0F;
            plot::DrawList l = emptyList(200, 40, dpr);
            l.textRun("Throttle Pos", 10.0F * dpr, 10.0F * dpr, plot::TextStyleId::Axis,
                      plot::TextAnchor::TopLeft, 0x000000FFU);
            ink[i] = inkBounds(rasterise(l, 0xFFFFFFFFU, fonts), kWhite, 48);
        }
        QVERIFY(std::abs(ink[1].width() - 2 * ink[0].width()) <= 4);
        QVERIFY(std::abs(ink[1].left() - 2 * ink[0].left()) <= 3);
    }

    void textElidesAtMaxWidth() {
        plot::DrawList l = emptyList(300, 40, 1.0F);
        l.textRun("A very long channel name that cannot fit", 10.0F, 10.0F, plot::TextStyleId::Key,
                  plot::TextAnchor::TopLeft, 0x000000FFU, 60.0F);
        const QRect ink = inkBounds(rasterise(l), kWhite, 48);
        QVERIFY(!ink.isNull());
        QVERIFY2(ink.right() <= 10 + 60 + 1, qPrintable(QString::number(ink.right())));
    }

    void textBackgroundFillsTheBox() {
        const auto fonts = qttest::testFonts();
        const qt::QtTextMetrics metrics(fonts->scheme(), 1);
        plot::DrawList l = emptyList(200, 40, 1.0F);
        l.textRun("12.5", 100.0F, 20.0F, plot::TextStyleId::KeyValue, plot::TextAnchor::Centre, 0x000000FFU,
                  0.0F, 0, 0x00FF00FFU);
        const QImage img = rasterise(l, 0xFFFFFFFFU, fonts);
        const float w = metrics.advancePx(plot::TextStyleId::KeyValue, "12.5", 1.0F);
        const float h = metrics.lineHeightPx(plot::TextStyleId::KeyValue, 1.0F);
        const QRect fill = inkBounds(img, kWhite, 0);
        QVERIFY(std::abs(fill.width() - static_cast<int>(std::lround(w))) <= 1);
        QVERIFY(std::abs(fill.height() - static_cast<int>(std::lround(h))) <= 1);
        QVERIFY(sameColour(pixel(img, fill.left(), fill.top()), 0xFF00FF00U));
    }

    void rotatedTextReadsUpwards() {
        plot::DrawList l = emptyList(60, 200, 1.0F);
        l.textRun("Vertical title", 30.0F, 100.0F, plot::TextStyleId::Axis, plot::TextAnchor::Centre,
                  0x000000FFU, 0.0F, 1);
        const QRect ink = inkBounds(rasterise(l), kWhite, 48);
        QVERIFY(!ink.isNull());
        QVERIFY(ink.height() > 2 * ink.width());
        QVERIFY(std::abs(ink.center().y() - 100) <= 4);
        QVERIFY(std::abs(ink.center().x() - 30) <= 4);
    }

    void staticTextCacheServesRepeatedRuns() {
        plot::DrawList l = emptyList(100, 30, 1.0F);
        l.textRun("same", 5.0F, 5.0F, plot::TextStyleId::Axis, plot::TextAnchor::TopLeft, 0x000000FFU);
        const auto fonts = qttest::testFonts();
        const QImage a = rasterise(l, 0xFFFFFFFFU, fonts);
        const QImage b = rasterise(l, 0xFFFFFFFFU, fonts);
        QCOMPARE(a, b);
    }

    void composeBlendsSourceOverInOrder() {
        qt::RasterBackend backend(qttest::testFonts(), nullptr);
        backend.beginLayer(plot::LayerKind::Static, 10, 10, 1.0F, 0xFF0000FFU);
        const plot::LayerHandle bottom = backend.endLayer();
        backend.beginLayer(plot::LayerKind::Dynamic, 10, 10, 1.0F, 0x0000FF80U);
        const plot::LayerHandle top = backend.endLayer();
        backend.beginLayer(plot::LayerKind::Chrome, 10, 10, 1.0F, 0);
        const plot::LayerHandle target = backend.endLayer();
        const std::array<plot::LayerHandle, 2> order{bottom, top};
        backend.compose(order, target);
        const QRgb p = pixel(backend.image(target), 4, 4);
        QVERIFY2(sameColour(p, qRgba(127, 0, 128, 255), 2), qPrintable(QString::number(p, 16)));
        backend.beginLayer(plot::LayerKind::Chrome, 10, 10, 1.0F, 0);
        const plot::LayerHandle reversed = backend.endLayer();
        const std::array<plot::LayerHandle, 2> other{top, bottom};
        backend.compose(other, reversed);
        QVERIFY(sameColour(pixel(backend.image(reversed), 4, 4), kRedQ));
    }

    void readbackIsStraightAlpha() {
        qt::RasterBackend backend(qttest::testFonts(), nullptr);
        backend.beginLayer(plot::LayerKind::Static, 3, 2, 1.0F, 0xFF000080U);
        const plot::LayerHandle h = backend.endLayer();
        const plot::Bitmap b = backend.readback(h);
        QCOMPARE(b.w, 3);
        QCOMPARE(b.h, 2);
        QCOMPARE(b.rgba.size(), std::size_t{24});
        QVERIFY(std::abs(static_cast<int>(b.rgba[0]) - 255) <= 1);
        QCOMPARE(static_cast<int>(b.rgba[1]), 0);
        QCOMPARE(static_cast<int>(b.rgba[3]), 128);
        QVERIFY(backend.readback(plot::LayerHandle{999}).rgba.empty());
    }

    // Static layers draw solid strokes with the direct rasteriser; widgets paint the same commands through
    // QPainter (paintList). Both must show the same picture.
    void staticStrokesMatchQPainter_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void staticStrokesMatchQPainter() {
        QFETCH(float, dpr);
        plot::DrawList l = emptyList(200, 100, dpr);
        l.clear(0xFFFFFFFFU);
        std::vector<float> sine;
        std::vector<float> m4;
        for (int i = 0; i <= 180; ++i) {
            sine.push_back((10.0F + static_cast<float>(i)) * dpr);
            sine.push_back((50.0F + 35.0F * std::sin(static_cast<float>(i) / 12.0F)) * dpr);
        }
        for (int c = 0; c < static_cast<int>(180 * dpr); ++c) { // an M4-shaped noisy trace
            const auto x = static_cast<float>(c) + 10.0F * dpr;
            const float lo = (70.0F + static_cast<float>((c * 7) % 13)) * dpr / 2.0F + 20.0F * dpr;
            const float hi = lo + static_cast<float>((c * 5) % 9) * dpr;
            m4.insert(m4.end(), {x, lo + 1.0F, x + 0.5F, lo, x + 0.5F, hi, x + 1.0F, hi - 1.0F});
        }
        l.polyline(sine.data(), sine.size() / 2, 0xE53935FFU, dpr);
        l.polyline(m4.data(), m4.size() / 2, 0x1E88E5FFU, dpr);
        const QImage fast = rasterise(l);

        QImage slow(fast.size(), QImage::Format_ARGB32_Premultiplied);
        slow.setDevicePixelRatio(static_cast<qreal>(dpr));
        slow.fill(Qt::transparent);
        {
            QPainter p(&slow);
            qt::RasterBackend::paintList(p, l, *qttest::testFonts());
        }
        const qt::ImageDiff diff = qt::compareImages(fast, slow, 48);
        qttest::saveArtefact(fast, QStringLiteral("strokes_fast@%1x").arg(static_cast<double>(dpr)));
        qttest::saveArtefact(slow, QStringLiteral("strokes_qpainter@%1x").arg(static_cast<double>(dpr)));
        // Different rasterisers: allow a few pixels at diagonal steps and stroke edges, not a different
        // picture.
        QVERIFY2(diff.fraction() < 0.02, qPrintable(diff.summary()));
        QCOMPARE(inkBounds(fast, kWhite, 48), inkBounds(slow, kWhite, 48));
    }

    void translucentStrokesBlendOncePerPixel() {
        plot::DrawList l = emptyList(60, 20, 1.0F);
        const std::vector<float> xy = {2.0F, 10.5F, 20.0F, 10.5F, 40.0F, 10.5F, 40.0F, 15.5F, 41.0F, 15.5F};
        l.polyline(xy.data(), xy.size() / 2, 0xFF000080U, 1.0F);
        const QImage img = rasterise(l);
        const QRgb once = pixel(img, 10, 10);
        QVERIFY(sameColour(once, qRgb(255, 127, 127), 2));
        QCOMPARE(pixel(img, 20, 10), once); // a joint
        QCOMPARE(pixel(img, 40, 10), once); // a corner
        QCOMPARE(pixel(img, 40, 13), once); // the vertical run
        QCOMPARE(pixel(img, 41, 15), once); // the last pixel
    }

    void scissorClipsStaticStrokes() {
        plot::DrawList l = emptyList(60, 40, 1.0F);
        l.scissor({10.0F, 0.0F, 20.0F, 40.0F});
        const std::vector<float> h = {-1.0e6F, 20.5F, 1.0e6F, 20.5F};
        const std::vector<float> d = {-5.0e4F, -5.0e4F, 5.0e4F, 5.0e4F};
        l.polyline(h.data(), 2, kRed, 1.0F);
        l.polyline(d.data(), 2, kRed, 3.0F);
        const QImage img = rasterise(l);
        const QRect ink = inkBounds(img, kWhite, 0);
        QVERIFY(QRect(10, 0, 20, 40).contains(ink));
        QVERIFY(sameColour(pixel(img, 10, 20), kRedQ));
        QVERIFY(sameColour(pixel(img, 29, 20), kRedQ));
    }

    void geometrySceneMatchesTheReference_data() {
        QTest::addColumn<float>("dpr");
        QTest::addColumn<QString>("name");
        QTest::newRow("dpr1") << 1.0F << QStringLiteral("geometry_scene@1x");
        QTest::newRow("dpr1.5") << 1.5F << QStringLiteral("geometry_scene@1.5x");
        QTest::newRow("dpr2") << 2.0F << QStringLiteral("geometry_scene@2x");
    }
    void geometrySceneMatchesTheReference() {
        QFETCH(float, dpr);
        QFETCH(QString, name);
        const plot::DrawList scene = qttest::geometryScene(dpr);
        const QImage img = rasterise(scene, 0);
        QCOMPARE(img.size(), QSize(scene.widthPx, scene.heightPx));
        QCOMPARE(img.devicePixelRatio(), static_cast<qreal>(dpr));
        QString message;
        QVERIFY2(qttest::matchesReference(img, name, message), qPrintable(message));
    }
};

QTEST_MAIN(TestRasterBackend)
#include "tst_raster_backend.moc"
