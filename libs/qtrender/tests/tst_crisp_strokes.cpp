// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Crisp strokes at a fractional DPR (M4 hidpi package, D-209; 04 section 6.6 items 1 and 4;
// raster_backend.hpp item 7) and the whole-pixel origin of LayerCache::paint. Expectations are the pixel sets
// the rule names, computed here from the geometry: an axis-aligned stroke of width w > 1 at centre c covers
// the W = lround(w) pixels from floor(c + (1 - W) / 2) across, and along it the pixels whose centres lie in
// its half-open extent (both ends snapped to pixel edges); W = 1 is the aliased 1 px path (the pixel
// containing the centre, both end pixels). Each case is drawn by both paths of RasterBackend: the Static
// rasteriser (LayerCache::rasteriseStatic) and paintList onto a DPR-tagged image (Dynamic and Chrome lists),
// which must agree.
//   * DPR 1.5: a 2 px vertical line at x = 10.5 (where the builders put round(DPR) = 2 px grid lines and the
//     cursor) covers columns 10 and 11 fully and nothing else; a 1.5 px horizontal tick at y = 7.3 rows 6 and
//     7; a 3 px stroke at 20.0 columns 19-21; a rectangle outline of a fractional rectangle has no partially
//     covered pixel (the QPainter path mitres its corners); a slanted line stays antialiased.
//   * DPR 1.25: a 1.25 px line is 1 px, the pixel containing its centre.
//   * DPR 2 (and 1): drawn exactly as before (the authoritative goldens, D-025): the 2 px line at 10.5
//     covers columns 9-11 at 50/100/50 %.
//   * LayerCache::paint from a painter at a fractional device origin paints composite() pixel for pixel at
//     the rounded origin (the Dynamic and Chrome lists with the Static image).

#include "qtrender_test_support.hpp"

#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QGuiApplication>
#include <QPainter>
#include <QTest>
#include <array>
#include <vector>

using namespace esses;
using esses::qttest::emptyList;

namespace {

constexpr plot::Rgba kInk = 0x204080FFU;   // opaque
constexpr plot::Rgba kPaper = 0xFFFFFFFFU; // the Static clear
constexpr QRgb kInkRgb = 0xFF204080U;
constexpr QRgb kPaperRgb = 0xFFFFFFFFU;

// The list drawn the two ways: the Static rasteriser and paintList over a paper fill.
std::array<QImage, 2> drawBoth(const plot::DrawList& list) {
    QImage viaStatic = qt::RasterBackend::rasteriseStatic(list, qt::SchemeFonts(), kPaper);
    QImage viaPainter(list.widthPx, list.heightPx, QImage::Format_ARGB32_Premultiplied);
    viaPainter.setDevicePixelRatio(static_cast<qreal>(list.dpr));
    viaPainter.fill(QColor::fromRgba(kPaperRgb));
    {
        QPainter p(&viaPainter);
        qt::RasterBackend::paintList(p, list, qt::SchemeFonts());
    }
    return {viaStatic.convertToFormat(QImage::Format_ARGB32),
            viaPainter.convertToFormat(QImage::Format_ARGB32)};
}

// The ink pixels of `img` must be exactly `expected` and every other pixel paper.
bool exactly(const QImage& img, const QRegion& expected, QString& why) {
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb want = expected.contains(QPoint(x, y)) ? kInkRgb : kPaperRgb;
            if (img.pixel(x, y) != want) {
                why = QStringLiteral("pixel (%1, %2) is %3, expected %4")
                          .arg(x)
                          .arg(y)
                          .arg(img.pixel(x, y), 8, 16, QLatin1Char('0'))
                          .arg(want, 8, 16, QLatin1Char('0'));
                return false;
            }
        }
    }
    return true;
}

int partialPixels(const QImage& img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (c != kInkRgb && c != kPaperRgb) {
                ++n;
            }
        }
    }
    return n;
}

plot::DrawList listAt(float dpr) {
    // 32 x 32 device pixels whatever the DPR.
    plot::DrawList l;
    l.widthPx = 32;
    l.heightPx = 32;
    l.dpr = dpr;
    return l;
}

} // namespace

class TstCrispStrokes : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(qttest::registerGoldenFonts() > 0); }

    void twoPixelLineAtAPixelCentre_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("1.5") << 1.5F;
        QTest::newRow("1.25") << 1.25F;
    }
    // A 2 px vertical line at x = 10.5, y in [2, 20): columns 10-11, rows 2-19, at any fractional DPR.
    void twoPixelLineAtAPixelCentre() {
        QFETCH(float, dpr);
        plot::DrawList l = listAt(dpr);
        const std::array<float, 4> v{10.5F, 2.0F, 10.5F, 20.0F};
        l.lines(v.data(), 2, kInk, 2.0F);
        for (const QImage& img : drawBoth(l)) {
            QString why;
            QVERIFY2(exactly(img, QRegion(10, 2, 2, 18), why), qPrintable(why));
        }
    }

    // The builders' tick: width DPR x 1 = 1.5 at a data position y = 7.3, x in [3, 9): rows 6-7, columns 3-8.
    void fractionalWidthTick() {
        plot::DrawList l = listAt(1.5F);
        const std::array<float, 4> v{3.0F, 7.3F, 9.0F, 7.3F};
        l.lines(v.data(), 2, kInk, 1.5F);
        for (const QImage& img : drawBoth(l)) {
            QString why;
            QVERIFY2(exactly(img, QRegion(3, 6, 6, 2), why), qPrintable(why));
        }
    }

    // A 3 px vertical stroke centred on a pixel edge (20.0): floor(20 - 1) = 19, columns 19-21.
    void oddWidthOnAnEdge() {
        plot::DrawList l = listAt(1.5F);
        const std::array<float, 4> v{20.0F, 4.5F, 20.0F, 12.5F};
        l.lines(v.data(), 2, kInk, 3.0F);
        for (const QImage& img : drawBoth(l)) {
            QString why;
            // Along: ends at 4.5 and 12.5 go to ceil(4) = 4 and ceil(12) = 12: rows 4-11.
            QVERIFY2(exactly(img, QRegion(19, 4, 3, 8), why), qPrintable(why));
        }
    }

    // DPR 1.25: a 1.25 px line is drawn 1 px, in the pixel containing its centre, both end pixels included
    // (the aliased path).
    void quarterWidthIsOnePixel() {
        plot::DrawList l = listAt(1.25F);
        const std::array<float, 4> v{4.6F, 2.0F, 4.6F, 9.0F};
        l.lines(v.data(), 2, kInk, 1.25F);
        for (const QImage& img : drawBoth(l)) {
            QString why;
            QVERIFY2(exactly(img, QRegion(4, 2, 1, 8), why), qPrintable(why));
        }
    }

    // A rectangle outline of a fractional rectangle (rectOutline, 2 px) covers whole pixels only; on the
    // QPainter path its three joined corners are filled (a mitre), not rounded.
    void fractionalRectangleOutline() {
        plot::DrawList l = listAt(1.5F);
        l.rectOutline(plot::RectPx{3.25F, 4.25F, 20.5F, 12.5F}, kInk, 2.0F);
        const std::array<QImage, 2> both = drawBoth(l);
        for (const QImage& img : both) {
            QCOMPARE(partialPixels(img), 0);
            QVERIFY(img.pixel(15, 4) == kInkRgb || img.pixel(15, 5) == kInkRgb); // the top edge is drawn
        }
        const QImage& viaPainter = both[1];
        // The outline's vertices: (4.25, 5.25) ... (22.75, 15.75) snapped to whole-pixel 2 px blocks: columns
        // 3-4 and 22-23 (floor(4.25 - 0.5) = 3, floor(22.75 - 0.5) = 22), rows 4-5 and 15-16.
        for (const QPoint corner : {QPoint(23, 4), QPoint(23, 16), QPoint(3, 16)}) {
            QVERIFY2(viaPainter.pixel(corner) == kInkRgb,
                     qPrintable(QStringLiteral("corner (%1, %2)").arg(corner.x()).arg(corner.y())));
        }
    }

    // A slanted stroke is drawn as listed: antialiased.
    void slantedStaysAntialiased() {
        plot::DrawList l = listAt(1.5F);
        const std::array<float, 4> v{2.0F, 2.0F, 28.0F, 19.0F};
        l.lines(v.data(), 2, kInk, 2.0F);
        for (const QImage& img : drawBoth(l)) {
            QVERIFY(partialPixels(img) > 0);
        }
    }

    void integerDprUnchanged_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("2") << 2.0F;
        QTest::newRow("1") << 1.0F;
    }
    // DPR 1 and 2 are drawn as their authoritative goldens show: the 2 px line at 10.5 covers columns 9-11,
    // the outer two half.
    void integerDprUnchanged() {
        QFETCH(float, dpr);
        plot::DrawList l = listAt(dpr);
        const std::array<float, 4> v{10.5F, 2.0F, 10.5F, 20.0F};
        l.lines(v.data(), 2, kInk, 2.0F);
        for (const QImage& img : drawBoth(l)) {
            QCOMPARE(img.pixel(10, 10), kInkRgb);
            QVERIFY(img.pixel(9, 10) != kInkRgb && img.pixel(9, 10) != kPaperRgb);
            QVERIFY(img.pixel(11, 10) != kInkRgb && img.pixel(11, 10) != kPaperRgb);
            QCOMPARE(img.pixel(8, 10), kPaperRgb);
            QCOMPARE(img.pixel(12, 10), kPaperRgb);
        }
    }

    void paintFromAFractionalOrigin_data() {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<qreal>("offset");         // logical
        QTest::newRow("1.25 at 3") << 1.25 << 3.0; // device 3.75 -> 4
        QTest::newRow("1.25 at 1") << 1.25 << 1.0; // device 1.25 -> 1
        QTest::newRow("1.5 at 7") << 1.5 << 7.0;   // device 10.5 -> 11
        QTest::newRow("2 at 3") << 2.0 << 3.0;     // device 6, whole already
    }
    // A widget at a fractional device position: every layer from the same whole device pixel, so the painted
    // pixels are composite()'s, shifted by the rounded origin.
    void paintFromAFractionalOrigin() {
        QFETCH(qreal, dpr);
        QFETCH(qreal, offset);
        const QSize logical(40, 24);
        qt::LayerCache cache;
        cache.resize(logical, dpr);
        const QSize device = cache.deviceSize();
        plot::DrawList s;
        s.widthPx = device.width();
        s.heightPx = device.height();
        s.dpr = static_cast<float>(dpr);
        s.clear(0xF0F0F0FFU);
        const std::array<float, 4> grid{12.5F, 0.0F, 12.5F, static_cast<float>(device.height())};
        s.lines(grid.data(), 2, 0x808080FFU, 1.0F);
        cache.rasteriseStatic(s);
        plot::DrawList d;
        d.widthPx = device.width();
        d.heightPx = device.height();
        d.dpr = static_cast<float>(dpr);
        const std::array<float, 4> cursor{20.5F, 0.0F, 20.5F, static_cast<float>(device.height())};
        d.lines(cursor.data(), 2, 0xD02020FFU, 1.0F);
        d.textRun("1.25", 2.0F, 2.0F, plot::TextStyleId::KeyValue, plot::TextAnchor::TopLeft, 0x000000FFU);
        cache.setDynamic(d);
        cache.setChrome(emptyList(logical.width(), logical.height(), static_cast<float>(dpr)));
        const QImage composite = cache.composite().convertToFormat(QImage::Format_ARGB32);

        QImage target(QSize(64, 48) * dpr, QImage::Format_ARGB32_Premultiplied);
        target.setDevicePixelRatio(dpr);
        target.fill(Qt::white);
        {
            QPainter p(&target);
            p.translate(offset, offset);
            cache.paint(p, QRegion(QRect(QPoint(0, 0), logical)));
        }
        const QImage out = target.convertToFormat(QImage::Format_ARGB32);
        const int origin = static_cast<int>(std::floor(offset * dpr + 0.5));
        int differing = 0;
        for (int y = 0; y < composite.height(); ++y) {
            for (int x = 0; x < composite.width(); ++x) {
                if (out.pixel(origin + x, origin + y) != composite.pixel(x, y)) {
                    ++differing;
                }
            }
        }
        QCOMPARE(differing, 0);
    }
};

QTEST_MAIN(TstCrispStrokes)
#include "tst_crisp_strokes.moc"
