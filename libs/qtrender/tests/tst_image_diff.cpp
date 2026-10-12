// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The golden comparison rule (04 section 6.7 item 4, 08 section 6.4): a pixel differs when a straight-alpha
// channel differs by more than 8/255; an image fails above 0.5 % differing pixels or on a size mismatch.

#include <esses/qtrender/image_diff.hpp>

#include <QGuiApplication>
#include <QTest>

using namespace esses;

namespace {

QImage filled(int w, int h, QRgb c) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(c);
    return img;
}

} // namespace

class TestImageDiff : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void identicalImagesPass() {
        const QImage a = filled(10, 10, qRgb(10, 20, 30));
        const qt::ImageDiff d = qt::compareImages(a, a);
        QVERIFY(d.sizeMatches);
        QCOMPARE(d.totalPixels, std::int64_t{100});
        QCOMPARE(d.differingPixels, std::int64_t{0});
        QCOMPARE(d.maxChannelDelta, 0);
        QCOMPARE(d.fraction(), 0.0);
        QVERIFY(d.passes());
        QVERIFY(d.diff.isNull());
    }

    void channelToleranceIsEightOf255() {
        QImage a = filled(10, 10, qRgb(100, 100, 100));
        QImage b = a;
        b.setPixel(0, 0, qRgb(108, 100, 100));       // delta 8: equal
        b.setPixel(1, 0, qRgb(100, 91, 100));        // delta 9: differs
        b.setPixel(2, 0, qRgba(100, 100, 100, 246)); // alpha delta 9: differs
        const qt::ImageDiff d = qt::compareImages(b, a);
        QCOMPARE(d.differingPixels, std::int64_t{2});
        QCOMPARE(d.maxChannelDelta, 9);
        QCOMPARE(qt::compareImages(b, a, 9).differingPixels, std::int64_t{0});
    }

    void halfAPercentThreshold() {
        const QImage expected = filled(100, 100, qRgb(0, 0, 0));
        QImage actual = expected;
        for (int i = 0; i < 50; ++i) {
            actual.setPixel(i, 0, qRgb(255, 255, 255));
        }
        QVERIFY(qt::compareImages(actual, expected).passes()); // 0.5 % exactly passes
        actual.setPixel(50, 0, qRgb(255, 255, 255));
        const qt::ImageDiff d = qt::compareImages(actual, expected);
        QVERIFY(!d.passes()); // 0.51 % fails
        QVERIFY(qAbs(d.fraction() - 0.0051) < 1e-12);
        QVERIFY(d.summary().contains(QStringLiteral("51 of 10000")));
    }

    void sizeMismatchFails() {
        const qt::ImageDiff d = qt::compareImages(filled(10, 10, 0), filled(10, 11, 0));
        QVERIFY(!d.sizeMatches);
        QCOMPARE(d.fraction(), 1.0);
        QVERIFY(!d.passes());
        QVERIFY(!qt::compareImages(QImage(), QImage()).passes());
    }

    void premultipliedInputsCompareStraight() {
        QImage a(4, 4, QImage::Format_ARGB32_Premultiplied);
        a.fill(QColor(255, 0, 0, 128));
        const QImage b = filled(4, 4, qRgba(255, 0, 0, 128));
        QVERIFY(qt::compareImages(a, b, 1).passes(0.0));
    }

    void dprTagsAreIgnored() {
        QImage a = filled(8, 8, qRgb(1, 2, 3));
        QImage b = a.copy();
        b.setDevicePixelRatio(2.0);
        QVERIFY(qt::compareImages(a, b).passes(0.0));
    }

    void diffImageMarksDifferingPixels() {
        const QImage expected = filled(5, 5, qRgb(0, 0, 0));
        QImage actual = expected;
        actual.setPixel(3, 2, qRgb(200, 0, 0));
        const qt::ImageDiff d = qt::compareImages(actual, expected, qt::kGoldenChannelTolerance, true);
        QCOMPARE(d.diff.size(), QSize(5, 5));
        QCOMPARE(d.diff.pixel(3, 2), qRgb(255, 0, 0));
        QVERIFY(d.diff.pixel(0, 0) != qRgb(255, 0, 0));
    }
};

QTEST_MAIN(TestImageDiff)
#include "tst_image_diff.moc"
