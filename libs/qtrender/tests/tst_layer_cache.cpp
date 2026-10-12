// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayerCache (04 section 6.2, D-137): layers are invalidated exactly by the ChangeMask table of 04
// section 2.2 (recomposition only on the right masks), the Dynamic damage region is the old and new cursor
// strips plus the key-value column, and paint() composes Static + Dynamic + Chrome identically to
// RasterBackend::compose at DPR 1 and 2, clipped to the exposed region.

#include "qtrender_test_support.hpp"

#include <esses/plotcore/change_mask.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QGuiApplication>
#include <QTest>
#include <array>
#include <vector>

using namespace esses;
using esses::qttest::emptyList;
using esses::qttest::inkBounds;
using esses::qttest::pixel;
using esses::qttest::sameColour;

Q_DECLARE_METATYPE(esses::plot::ChangeMask)

namespace {

// A cache with every layer built (clean).
void makeClean(qt::LayerCache& cache) {
    cache.resize(QSize(100, 50), 1.0);
    plot::DrawList s = emptyList(100, 50, 1.0F);
    s.clear(0xFFFFFFFFU);
    cache.rasteriseStatic(s);
    cache.setDynamic(emptyList(100, 50, 1.0F));
    cache.setChrome(emptyList(100, 50, 1.0F));
}

plot::DrawList staticList(int w, int h, float dpr) {
    plot::DrawList l = emptyList(w, h, dpr);
    l.clear(0xFFFFFFFFU);
    l.filledRect({10.0F * dpr, 10.0F * dpr, 30.0F * dpr, 20.0F * dpr}, 0x1E88E5FFU);
    std::vector<float> trace;
    for (int i = 0; i < w; ++i) {
        trace.push_back(static_cast<float>(i) * dpr);
        trace.push_back(static_cast<float>(h / 2 + (i % 7) - 3) * dpr);
    }
    l.polyline(trace.data(), trace.size() / 2, 0xE53935FFU, dpr);
    return l;
}

// A cursor line at device x over the trace area and a key value at the left.
plot::DrawList dynamicList(int w, int h, float dpr, float cursorX, const char* value) {
    plot::DrawList l = emptyList(w, h, dpr);
    const std::vector<float> cursor = {cursorX, 0.0F, cursorX, static_cast<float>(l.heightPx)};
    l.polyline(cursor.data(), 2, 0xFF3B30FFU, dpr);
    l.textRun(value, 4.0F * dpr, 4.0F * dpr, plot::TextStyleId::KeyValue, plot::TextAnchor::TopLeft,
              0x000000FFU, 0.0F, 0, 0xFFFFFFFFU);
    return l;
}

plot::DrawList chromeList(int w, int h, float dpr) {
    plot::DrawList l = emptyList(w, h, dpr);
    const float px = dpr;
    const auto fw = static_cast<float>(l.widthPx);
    const auto fh = static_cast<float>(l.heightPx);
    l.filledRect({0.0F, 0.0F, fw, px}, 0x4C9AFFFFU);
    l.filledRect({0.0F, fh - px, fw, px}, 0x4C9AFFFFU);
    l.filledRect({0.0F, 0.0F, px, fh}, 0x4C9AFFFFU);
    l.filledRect({fw - px, 0.0F, px, fh}, 0x4C9AFFFFU);
    l.textRun("Caption", fw / 2.0F, 2.0F * dpr, plot::TextStyleId::Caption, plot::TextAnchor::TopCentre,
              0x202020FFU);
    return l;
}

} // namespace

class TestLayerCache : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { qttest::registerGoldenFonts(); }

    void freshCacheIsDirty() {
        qt::LayerCache cache;
        QVERIFY(cache.dirty(plot::LayerKind::Static));
        QVERIFY(cache.dirty(plot::LayerKind::Dynamic));
        QVERIFY(cache.dirty(plot::LayerKind::Chrome));
        QCOMPARE(cache.bytes(), std::size_t{0});
    }

    void invalidationFollowsTheChangeMaskTable_data() {
        QTest::addColumn<plot::ChangeMask>("mask");
        QTest::addColumn<bool>("staticDirty");
        QTest::addColumn<bool>("dynamicDirty");
        QTest::addColumn<bool>("chromeDirty");
        // 04 section 2.2: Cursor, Datum -> Dynamic; Zoom, Offsets, Scale, Size -> Static + Dynamic;
        // Domain, Selection, Data, Scheme -> all three.
        QTest::newRow("Cursor") << plot::mask(plot::Change::Cursor) << false << true << false;
        QTest::newRow("Datum") << plot::mask(plot::Change::Datum) << false << true << false;
        QTest::newRow("Zoom") << plot::mask(plot::Change::Zoom) << true << true << false;
        QTest::newRow("Offsets") << plot::mask(plot::Change::Offsets) << true << true << false;
        QTest::newRow("Scale") << plot::mask(plot::Change::Scale) << true << true << false;
        QTest::newRow("Size") << plot::mask(plot::Change::Size) << true << true << false;
        QTest::newRow("Domain") << plot::mask(plot::Change::Domain) << true << true << true;
        QTest::newRow("Selection") << plot::mask(plot::Change::Selection) << true << true << true;
        QTest::newRow("Data") << plot::mask(plot::Change::Data) << true << true << true;
        QTest::newRow("Scheme") << plot::mask(plot::Change::Scheme) << true << true << true;
        QTest::newRow("Cursor|Zoom") << (plot::Change::Cursor | plot::Change::Zoom) << true << true << false;
        QTest::newRow("Datum|Scheme") << (plot::Change::Datum | plot::Change::Scheme) << true << true << true;
        QTest::newRow("none") << plot::ChangeMask{0} << false << false << false;
    }
    void invalidationFollowsTheChangeMaskTable() {
        QFETCH(plot::ChangeMask, mask);
        QFETCH(bool, staticDirty);
        QFETCH(bool, dynamicDirty);
        QFETCH(bool, chromeDirty);
        qt::LayerCache cache;
        makeClean(cache);
        QVERIFY(!cache.dirty(plot::LayerKind::Static));
        QVERIFY(!cache.dirty(plot::LayerKind::Dynamic));
        QVERIFY(!cache.dirty(plot::LayerKind::Chrome));
        const QImage before = cache.staticImage();
        cache.invalidate(mask);
        QCOMPARE(cache.dirty(plot::LayerKind::Static), staticDirty);
        QCOMPARE(cache.dirty(plot::LayerKind::Dynamic), dynamicDirty);
        QCOMPARE(cache.dirty(plot::LayerKind::Chrome), chromeDirty);
        QCOMPARE(cache.staticImage().constBits(), before.constBits()); // kept until the rebuild lands
    }

    void everyBitIsCoveredByTheTable() {
        plot::ChangeMask all = 0;
        for (int bit = 0; bit < 10; ++bit) {
            all = static_cast<plot::ChangeMask>(all | (1U << bit));
        }
        QCOMPARE(
            static_cast<plot::ChangeMask>(plot::kDynamicOnly | plot::kStaticRebuild | plot::kFullRebuild),
            all);
        QCOMPARE(static_cast<int>(plot::kDynamicOnly & plot::kStaticRebuild), 0);
        QCOMPARE(static_cast<int>(plot::kStaticRebuild & plot::kFullRebuild), 0);
    }

    void resizeMarksEveryLayerDirty() {
        qt::LayerCache cache;
        makeClean(cache);
        cache.resize(QSize(100, 50), 1.0); // unchanged
        QVERIFY(!cache.dirty(plot::LayerKind::Static));
        cache.resize(QSize(120, 50), 1.0);
        QVERIFY(cache.dirty(plot::LayerKind::Static));
        QVERIFY(cache.dirty(plot::LayerKind::Dynamic));
        QVERIFY(cache.dirty(plot::LayerKind::Chrome));
        QVERIFY(!cache.staticImage().isNull()); // the previous image stays on screen until the rebuild
        makeClean(cache);
        cache.resize(QSize(100, 50), 2.0);
        QVERIFY(cache.dirty(plot::LayerKind::Static));
        QCOMPARE(cache.deviceSize(), QSize(200, 100));
        cache.resize(QSize(101, 33), 1.5);
        QCOMPARE(cache.deviceSize(), QSize(152, 50));
    }

    void rasteriseStaticReusesTheBuffer() {
        qt::LayerCache cache(qttest::testFonts());
        cache.resize(QSize(80, 40), 2.0);
        cache.rasteriseStatic(staticList(80, 40, 2.0F));
        const QImage& img = cache.staticImage();
        QCOMPARE(img.size(), QSize(160, 80));
        QCOMPARE(img.devicePixelRatio(), 2.0);
        QCOMPARE(cache.bytes(), std::size_t{160 * 80 * 4});
        const uchar* bits = img.constBits();
        cache.invalidate(plot::mask(plot::Change::Zoom));
        cache.rasteriseStatic(staticList(80, 40, 2.0F));
        QCOMPARE(cache.staticImage().constBits(), bits);
        QVERIFY(!cache.dirty(plot::LayerKind::Static));
        // A held copy is never overwritten (the cache detaches).
        const QImage held = cache.staticImage();
        plot::DrawList red = emptyList(80, 40, 2.0F);
        red.clear(0xFF0000FFU);
        cache.rasteriseStatic(red);
        QVERIFY(sameColour(pixel(held, 1, 1), 0xFFFFFFFFU));
        QVERIFY(sameColour(pixel(cache.staticImage(), 1, 1), 0xFFFF0000U));
    }

    void releaseStatic() {
        qt::LayerCache cache;
        makeClean(cache);
        QVERIFY(cache.bytes() > 0);
        cache.releaseStatic();
        QVERIFY(cache.staticImage().isNull());
        QCOMPARE(cache.bytes(), std::size_t{0});
        QVERIFY(cache.dirty(plot::LayerKind::Static));
    }

    void dynamicDamageCoversCursorStripsAndKeyColumn_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void dynamicDamageCoversCursorStripsAndKeyColumn() {
        QFETCH(float, dpr);
        qt::LayerCache cache(qttest::testFonts());
        cache.resize(QSize(400, 200), static_cast<qreal>(dpr));
        const plot::DrawList before = dynamicList(400, 200, dpr, 200.5F * dpr, "12.3");
        const plot::DrawList after = dynamicList(400, 200, dpr, 300.5F * dpr, "45.6");
        const QRegion damage = cache.dynamicDamage(before, after);
        // Old and new cursor strips over the full height ...
        for (const int x : {200, 300}) {
            QVERIFY(damage.contains(QPoint(x, 0)));
            QVERIFY(damage.contains(QPoint(x, 199)));
            QVERIFY(damage.contains(QPoint(x - 1, 100)));
            QVERIFY(damage.contains(QPoint(x + 1, 100)));
        }
        // ... the key-value column ...
        QVERIFY(damage.contains(QPoint(6, 8)));
        // ... and nothing else.
        QVERIFY(!damage.contains(QPoint(250, 100)));
        QVERIFY(!damage.contains(QPoint(100, 150)));
        const QRect strip = damage.intersected(QRect(150, 100, 100, 1)).boundingRect();
        QVERIFY2(strip.width() <= static_cast<int>(2 * (1 + 2 + 1)),
                 qPrintable(QString::number(strip.width()))); // x +- (w + 2) device px, in logical px
        QVERIFY(QRect(0, 0, 400, 200).contains(damage.boundingRect()));
    }

    void dynamicDamageOfEqualListsIsEmpty() {
        qt::LayerCache cache(qttest::testFonts());
        cache.resize(QSize(400, 200), 1.0);
        const plot::DrawList l = dynamicList(400, 200, 1.0F, 100.5F, "1");
        QVERIFY(cache.dynamicDamage(l, l).isEmpty());
        QVERIFY(cache.dynamicDamage(plot::DrawList{}, plot::DrawList{}).isEmpty());
    }

    void dynamicDamageOfAClearIsTheWholeComponent() {
        qt::LayerCache cache;
        cache.resize(QSize(400, 200), 1.0);
        plot::DrawList cleared = emptyList(400, 200, 1.0F);
        cleared.clear(0);
        QCOMPARE(cache.dynamicDamage(plot::DrawList{}, cleared), QRegion(0, 0, 400, 200));
    }

    void paintComposesLikeRasterBackend_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("dpr1") << 1.0F;
        QTest::newRow("dpr2") << 2.0F;
    }
    void paintComposesLikeRasterBackend() {
        QFETCH(float, dpr);
        const auto fonts = qttest::testFonts();
        const int w = 160;
        const int h = 90;
        qt::LayerCache cache(fonts);
        cache.resize(QSize(w, h), static_cast<qreal>(dpr));
        cache.rasteriseStatic(staticList(w, h, dpr));
        cache.setDynamic(dynamicList(w, h, dpr, 80.5F * dpr, "7.25"));
        cache.setChrome(chromeList(w, h, dpr));

        QImage painted(cache.deviceSize(), QImage::Format_ARGB32_Premultiplied);
        painted.setDevicePixelRatio(static_cast<qreal>(dpr));
        painted.fill(Qt::transparent);
        {
            QPainter p(&painted);
            cache.paint(p, QRegion(0, 0, w, h));
        }

        qt::RasterBackend backend(fonts, nullptr);
        const int dw = cache.deviceSize().width();
        const int dh = cache.deviceSize().height();
        std::array<plot::LayerHandle, 3> layers{};
        backend.beginLayer(plot::LayerKind::Static, dw, dh, dpr, 0);
        backend.draw(staticList(w, h, dpr));
        layers[0] = backend.endLayer();
        backend.beginLayer(plot::LayerKind::Dynamic, dw, dh, dpr, 0);
        backend.draw(dynamicList(w, h, dpr, 80.5F * dpr, "7.25"));
        layers[1] = backend.endLayer();
        backend.beginLayer(plot::LayerKind::Chrome, dw, dh, dpr, 0);
        backend.draw(chromeList(w, h, dpr));
        layers[2] = backend.endLayer();
        backend.beginLayer(plot::LayerKind::Static, dw, dh, dpr, 0);
        const plot::LayerHandle target = backend.endLayer();
        backend.compose(layers, target);
        const qt::ImageDiff diff = qt::compareImages(painted, backend.image(target), 0);
        qttest::saveArtefact(painted, QStringLiteral("layer_cache_paint@%1x").arg(static_cast<double>(dpr)));
        // Direct painting of an antialiased list versus compositing a layer differs only by rounding.
        QVERIFY2(diff.passes(0.0) || diff.maxChannelDelta <= 2, qPrintable(diff.summary()));
    }

    void compositeIsTheFullPaint() {
        qt::LayerCache cache(qttest::testFonts());
        QVERIFY(cache.composite().isNull()); // not laid out yet
        cache.resize(QSize(70, 30), 2.0);
        cache.rasteriseStatic(staticList(70, 30, 2.0F));
        cache.setDynamic(dynamicList(70, 30, 2.0F, 40.5F, "1.5"));
        cache.setChrome(chromeList(70, 30, 2.0F));
        const QImage c = cache.composite();
        QCOMPARE(c.size(), QSize(140, 60));
        QCOMPARE(c.devicePixelRatio(), 2.0);
        QImage painted(140, 60, QImage::Format_ARGB32_Premultiplied);
        painted.setDevicePixelRatio(2.0);
        painted.fill(Qt::transparent);
        {
            QPainter p(&painted);
            cache.paint(p, QRegion(0, 0, 70, 30));
        }
        QCOMPARE(c, painted);
    }

    void paintIsClippedToTheExposedRegion() {
        qt::LayerCache cache(qttest::testFonts());
        cache.resize(QSize(100, 50), 1.0);
        plot::DrawList s = emptyList(100, 50, 1.0F);
        s.clear(0xFF0000FFU);
        cache.rasteriseStatic(s);
        plot::DrawList d = emptyList(100, 50, 1.0F);
        d.filledRect({0.0F, 0.0F, 100.0F, 50.0F}, 0x0000FFFFU);
        cache.setDynamic(d);
        QImage img(100, 50, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            cache.paint(p, QRegion(0, 0, 30, 50));
            cache.paint(p, QRegion()); // nothing exposed: nothing painted
        }
        QCOMPARE(inkBounds(img, 0xFFFFFFFFU, 0), QRect(0, 0, 30, 50));
        QVERIFY(sameColour(pixel(img, 10, 10), 0xFF0000FFU)); // Dynamic over Static
    }

    void paintWithoutStaticImagePaintsTheLists() {
        qt::LayerCache cache;
        cache.resize(QSize(40, 20), 1.0);
        plot::DrawList d = emptyList(40, 20, 1.0F);
        d.filledRect({5.0F, 5.0F, 10.0F, 10.0F}, 0x000000FFU);
        cache.setDynamic(d);
        QImage img(40, 20, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            cache.paint(p, QRegion(0, 0, 40, 20));
        }
        QCOMPARE(inkBounds(img, 0xFFFFFFFFU, 0), QRect(5, 5, 10, 10));
    }
};

QTEST_MAIN(TestLayerCache)
#include "tst_layer_cache.moc"
