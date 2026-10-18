// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Determinism of text that Qt draws as outlines (04 section 6.7 item 1: the same inputs give the same
// pixels): a run whose glyphs exceed Qt's glyph-cache limit (64 device px; the numeric gauge's dynamic font,
// TextRun::scale) renders the same whatever text the thread drew before it. Qt's FreeType engine loads such
// outlines under the face transform left by the last glyph the thread rasterised, subpixel offset included
// (raster_backend.cpp, drawText); the text drawn in between here is the same family with vertical-only
// hinting, i.e. with subpixel glyph positions, as fontconfig's default hintslight gives every font on Linux,
// so the offsets are non-zero on every FreeType platform (Linux, the offscreen platform on Windows). CoreText
// keeps no such state, so on macOS this is a plain repeatability check.

#include "qtrender_test_support.hpp"

#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTest>
#include <cmath>

using namespace esses;
using qttest::emptyList;
using qttest::rasterise;

namespace {

constexpr plot::Rgba kWhite = 0xFFFFFFFFU;

// One Gauge run at `scale` (the test scheme's gauge font is 14 pt: about 19 logical px, so a scale of 5 is
// about 93 device px at DPR 1, past the limit at every DPR), on a cleared layer unless `clear` is false.
plot::DrawList largeRun(float dpr, float scale = 5.0F, bool clear = true) {
    plot::DrawList l = emptyList(420, 160, dpr);
    if (clear) {
        l.clear(kWhite);
    }
    const std::size_t cmd = l.textRun("-48.37", 6.0F * dpr, 80.0F * dpr, plot::TextStyleId::Gauge,
                                      plot::TextAnchor::MiddleLeft, 0x202124FFU);
    l.texts[l.cmds[cmd].textIndex].scale = scale;
    return l;
}

// Rasterises digits at subpixel positions (identity transform, so the default glyph set of a new engine:
// every glyph is loaded, each with its offset), in a size no other text of the process uses, in both weights
// of the golden fonts (the state is per font file: the gauge role's 600 is NotoSans-Bold). The last glyph of
// each weight lands on the quarter pixel (round % 4, round / 4 % 4) / 4, so consecutive rounds leave
// different offsets behind.
void drawSubpixelText(int round) {
    QImage scratch(360, 60, QImage::Format_ARGB32_Premultiplied);
    scratch.fill(Qt::white);
    QPainter p(&scratch);
    p.setPen(Qt::black);
    for (const QFont::Weight weight : {QFont::Normal, QFont::DemiBold}) {
        QFont f(QStringLiteral("Noto Sans"));
        f.setPointSizeF(7.0 + 0.37 * round);
        f.setWeight(weight);
        f.setHintingPreference(QFont::PreferVerticalHinting);
        f.setStyleStrategy(QFont::NoSubpixelAntialias);
        p.setFont(f);
        for (int k = 1; k < 8; ++k) {
            p.drawText(QPointF(4.0 + 40.0 * k + k / 8.0, 20.0 + k / 8.0), QStringLiteral("8.0-4%1").arg(k));
        }
        p.drawText(QPointF(10.1 + 0.25 * (round % 4), 45.1 + 0.25 * (round / 4 % 4)), QStringLiteral("W"));
    }
}

QString differing(const QImage& a, const QImage& b) {
    if (a.size() != b.size()) {
        return QStringLiteral("size %1x%2 vs %3x%4")
            .arg(a.width())
            .arg(a.height())
            .arg(b.width())
            .arg(b.height());
    }
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            n += p[col] != q[col] ? 1 : 0;
        }
    }
    return QStringLiteral("%1 pixels differ").arg(n);
}

} // namespace

class TstTextOutlineState : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { qttest::registerGoldenFonts(); }

    void outlineTextIgnoresEarlierText_data() {
        QTest::addColumn<float>("dpr");
        QTest::newRow("1x") << 1.0F;
        QTest::newRow("1.25x") << 1.25F;
        QTest::newRow("1.5x") << 1.5F;
        QTest::newRow("2x") << 2.0F;
    }

    void outlineTextIgnoresEarlierText() {
        QFETCH(float, dpr);
        const auto fonts = qttest::testFonts();
        const plot::DrawList list = largeRun(dpr);
        const QImage first = rasterise(list, kWhite, fonts);
        QVERIFY(qttest::inkBounds(first, 0xFFFFFFFFU, 48).width() > 150); // the run is drawn, and large
        for (int round = 0; round < 6; ++round) {
            drawSubpixelText(round + static_cast<int>(std::lround(dpr * 8.0F)) * 8);
            const QImage again = rasterise(list, kWhite, fonts);
            QVERIFY2(again == first,
                     qPrintable(QStringLiteral("round %1: ").arg(round) + differing(again, first)));
        }
    }

    // The same through LayerCache's Dynamic list (the numeric gauge's value), painted onto the composite.
    void dynamicOutlineTextIgnoresEarlierText() {
        const auto fonts = qttest::testFonts();
        for (const float dpr : {1.0F, 2.0F}) {
            qt::LayerCache cache(fonts);
            cache.resize(QSize(420, 160), static_cast<qreal>(dpr));
            plot::DrawList background = emptyList(420, 160, dpr);
            background.clear(kWhite);
            cache.rasteriseStatic(background);
            cache.setDynamic(largeRun(dpr, 4.5F, false)); // the Static layer holds the background
            const QImage first = cache.composite();
            for (int round = 0; round < 4; ++round) {
                drawSubpixelText(100 + round + static_cast<int>(dpr) * 10);
                const QImage again = cache.composite();
                QVERIFY2(
                    again == first,
                    qPrintable(QStringLiteral("dpr %1 round %2: ").arg(static_cast<double>(dpr)).arg(round) +
                               differing(again, first)));
            }
        }
    }
};

QTEST_MAIN(TstTextOutlineState)
#include "tst_text_outline_state.moc"
