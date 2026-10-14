// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// plot::TextRun::scale (D-198 (b); the numeric gauge's dynamic font, 04 section 16 item 1) through
// RasterBackend and LayerCache: a scaled run's ink grows with its scale, elision applies at the scaled size,
// a background box follows the scaled metrics, the Dynamic damage covers the scaled ink, and the DrawList
// JSON names the scale only when it is not 1. Text is checked by geometry (ink bounds), as the other qtrender
// tests.

#include "qtrender_test_support.hpp"

#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>

#include <QTest>
#include <cmath>
#include <string>

using namespace esses;
using qttest::emptyList;
using qttest::inkBounds;
using qttest::rasterise;

namespace {

constexpr QRgb kWhite = 0xFFFFFFFFU;

// A list with one Gauge run at `scale`.
plot::DrawList gaugeRun(const char* text, float scale, float maxWidthPx = 0.0F, plot::Rgba background = 0) {
    plot::DrawList l = emptyList(600, 200, 1.0F);
    const std::size_t cmd = l.textRun(text, 10.0F, 10.0F, plot::TextStyleId::Gauge, plot::TextAnchor::TopLeft,
                                      0x000000FFU, maxWidthPx, 0, background);
    l.texts[l.cmds[cmd].textIndex].scale = scale;
    return l;
}

} // namespace

class TstTextScale : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { qttest::registerGoldenFonts(); }

    void inkGrowsWithTheScale() {
        const auto fonts = qttest::testFonts();
        const QRect one = inkBounds(rasterise(gaugeRun("88.8", 1.0F), kWhite, fonts), kWhite, 48);
        const QRect three = inkBounds(rasterise(gaugeRun("88.8", 3.0F), kWhite, fonts), kWhite, 48);
        QVERIFY(!one.isNull());
        QVERIFY(!three.isNull());
        const double wRatio = static_cast<double>(three.width()) / one.width();
        const double hRatio = static_cast<double>(three.height()) / one.height();
        QVERIFY2(std::abs(wRatio - 3.0) < 0.3, qPrintable(QString::number(wRatio)));
        QVERIFY2(std::abs(hRatio - 3.0) < 0.3, qPrintable(QString::number(hRatio)));
        // A scale of 1 is the unscaled style (the M1 path).
        const plot::DrawList plain = [] {
            plot::DrawList l = emptyList(600, 200, 1.0F);
            l.textRun("88.8", 10.0F, 10.0F, plot::TextStyleId::Gauge, plot::TextAnchor::TopLeft, 0x000000FFU);
            return l;
        }();
        QCOMPARE(rasterise(plain, kWhite, fonts), rasterise(gaugeRun("88.8", 1.0F), kWhite, fonts));
    }

    void elisionAndBackgroundFollowTheScaledFont() {
        const auto fonts = qttest::testFonts();
        const QRect elided =
            inkBounds(rasterise(gaugeRun("1234567890.123", 2.0F, 150.0F), kWhite, fonts), kWhite, 48);
        QVERIFY(!elided.isNull());
        QVERIFY2(elided.right() <= 10 + 150 + 1, qPrintable(QString::number(elided.right())));
        const qt::QtTextMetrics metrics(fonts->scheme(), 1);
        const QRect box =
            inkBounds(rasterise(gaugeRun("12.5", 2.0F, 0.0F, 0x00FF00FFU), kWhite, fonts), kWhite, 0);
        const float w = 2.0F * metrics.advancePx(plot::TextStyleId::Gauge, "12.5", 1.0F);
        const float h = 2.0F * metrics.lineHeightPx(plot::TextStyleId::Gauge, 1.0F);
        QVERIFY2(std::abs(box.width() - w) <= 0.06F * w, qPrintable(QString::number(box.width())));
        QVERIFY2(std::abs(box.height() - h) <= 0.06F * h, qPrintable(QString::number(box.height())));
    }

    void dynamicDamageCoversTheScaledInk() {
        const auto fonts = qttest::testFonts();
        qt::LayerCache cache(fonts);
        cache.resize(QSize(600, 200), 1.0);
        const plot::DrawList list = gaugeRun("88.8", 4.0F);
        const QRegion damage = cache.dynamicDamage(emptyList(600, 200, 1.0F), list);
        const QRect ink = inkBounds(rasterise(list, kWhite, fonts), kWhite, 48);
        QVERIFY(!ink.isNull());
        QVERIFY2(damage.boundingRect().contains(ink),
                 qPrintable(QStringLiteral("%1 %2").arg(damage.boundingRect().width()).arg(ink.width())));
    }

    void jsonNamesTheScaleOnlyWhenSet() {
        const std::string scaled = gaugeRun("88.8", 2.5F).toJson();
        QVERIFY(scaled.find("\"scale\":2.5") != std::string::npos);
        const std::string plain = gaugeRun("88.8", 1.0F).toJson();
        QVERIFY(plain.find("\"scale\"") == std::string::npos);
    }
};

QTEST_MAIN(TstTextScale)
#include "tst_text_scale.moc"
