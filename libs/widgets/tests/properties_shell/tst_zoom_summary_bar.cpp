// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ZoomSummaryBar (06 sections 2.1 and 4.5, 04 section 8.6; D-133): the summary in time and distance (the Main
// outing extent from the Main start, the lap starts, the shared zoom and cursor), the repaint on the
// worksheet's changes, the mouse (click sets the cursor, a drag moves the span, SHIFT+click extends it,
// double-click shows the whole outing); the DrawList's geometry against positions computed here by hand, the
// DrawList JSON goldens (tests/golden/drawlist/zoom_summary/, exact on every OS) and the pixel goldens
// (tests/golden/images/zoom_summary/, provisional: compared in report mode unless they were generated on
// Linux, D-127; ESSES_UPDATE_GOLDENS=1 rewrites both).

#include "components/component_test_support.hpp"

#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <cmath>
#include <filesystem>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;

bool updateGoldens() {
    return qEnvironmentVariableIntValue("ESSES_UPDATE_GOLDENS") == 1;
}

QString platformTag() {
#if defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("windows");
#endif
}

QString goldenPath(const char* kind, const QString& file) {
    return QString::fromStdWString((repoRoot() / "tests" / "golden" / kind / "zoom_summary").wstring()) +
           QLatin1Char('/') + file;
}

QString artefactsDir() {
    const QString dir = QString::fromStdWString(
        (std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts").lexically_normal().wstring());
    QDir().mkpath(dir);
    return dir;
}

// The fixed summary of the golden cases: an outing [-30 s, 130 s) around a 50 s Main lap, lap starts every
// 40 s or so, a zoom of [10 s, 35 s), the cursor at 20 s and the datum at 12 s.
ZoomSummaryBar::Summary goldenSummary() {
    ZoomSummaryBar::Summary s;
    s.valid = true;
    s.extent = plot::XSpan{-30.0 * kSec, 130.0 * kSec};
    s.main = plot::XSpan{0.0, 50.0 * kSec};
    s.laps = {-30.0 * kSec, 0.0, 50.0 * kSec, 98.0 * kSec};
    s.zoom = plot::XSpan{10.0 * kSec, 35.0 * kSec};
    s.cursor = 20.0 * kSec;
    s.datum = 12.0 * kSec;
    return s;
}

QImage render(const plot::DrawList& list) {
    QImage img(list.widthPx, list.heightPx, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(static_cast<qreal>(list.dpr));
    img.fill(Qt::transparent);
    QPainter p(&img);
    const qt::SchemeFonts fonts;
    qt::RasterBackend::paintList(p, list, fonts);
    p.end();
    return img;
}

} // namespace

class TstZoomSummaryBar : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void summaryFollowsTheWorksheet() {
        World w;
        const auto src = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*src, 2));
        w.view->zoomToMainRange();
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        ZoomSummaryBar bar;
        bar.setSelection(&w.selection);
        bar.setWorksheet(w.view.get());
        bar.setScheme(w.scheme);
        const ZoomSummaryBar::Summary& s = bar.summary();
        QVERIFY(s.valid);
        // Lap 2 of the default source starts after the 40 s out lap and the 50 s lap 1: the outing extent
        // runs from -90 s to the end of the 30 s in lap.
        const model::Range& lap = lapAt(*src, 2);
        QCOMPARE(lap.end - lap.start, 48 * kSec);
        QCOMPARE(s.extent.a, -90.0 * kSec);
        QCOMPARE(s.extent.b, (48.0 + 49.0 + 30.0) * kSec);
        QCOMPARE(s.main.a, 0.0);
        QCOMPARE(s.main.b, 48.0 * kSec);
        QCOMPARE(s.laps.size(), std::size_t{5});
        QCOMPARE(s.laps[2], 0.0); // the Main lap starts at 0
        QCOMPARE(s.zoom, w.view->state().zoom);
        QVERIFY(s.cursor.has_value());
        QCOMPARE(*s.cursor, 10.0 * kSec);
        QVERIFY(!s.datum.has_value());
        // Every change of the worksheet repaints it.
        bar.resize(400, 18);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        const int before = bar.repaints();
        w.view->setCursor(20 * kSec, qt::CursorSource::Programmatic);
        QTRY_VERIFY(bar.repaints() > before);
        QCOMPARE(*bar.summary().cursor, 20.0 * kSec);
        w.view->placeDatum();
        QVERIFY(bar.summary().datum.has_value());
        // Distance: through the Main lap's chain once it is built.
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(src), 30000);
        w.view->setDomain(plot::Domain::Distance);
        QTRY_VERIFY(bar.summary().main.b > 1900.0 && bar.summary().main.b < 2100.0); // metres of a 2 km lap
        QVERIFY(bar.summary().extent.a < 0.0);
        // Main an outing: the ticks are still the laps' starts (D-203 (q)), not the outings'.
        w.view->setDomain(plot::Domain::Time);
        const model::RangeGroup* outings = src->layers.front().ranges.find("Outings");
        QVERIFY(outings != nullptr && !outings->ranges.empty());
        model::RangeRef outing = lapRef(*src, 0);
        outing.groupPath = "Outings";
        outing.range = outings->ranges.front().id;
        outing.name = outings->ranges.front().name;
        w.selection.setMain(outing);
        QTRY_VERIFY(bar.summary().valid && bar.summary().laps.size() == 5);
        // Without a worksheet: empty.
        bar.setWorksheet(nullptr);
        QVERIFY(!bar.summary().valid);
    }

    void mouse() {
        World w;
        const auto src = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*src, 2));
        w.view->setSize(1000);
        w.view->zoomTo(plot::XSpan{10.0 * kSec, 20.0 * kSec});
        w.view->setCursor(15 * kSec, qt::CursorSource::Programmatic);
        ZoomSummaryBar bar;
        bar.setSelection(&w.selection);
        bar.setWorksheet(w.view.get());
        bar.resize(800, 18);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        const auto px = [&](double x) { return QPoint(static_cast<int>(std::lround(bar.pxOf(x))), 9); };
        // Click: the cursor (one pixel's worth of time around the target).
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, px(30.0 * kSec));
        const double pixel = bar.summary().extent.length() / (800 - 8);
        QVERIFY(std::abs(static_cast<double>(w.view->state().cursorT) - 30.0 * kSec) <= pixel);
        // Drag: the span moves by the pointer's travel, its length kept.
        const plot::XSpan before = w.view->state().zoom;
        QTest::mousePress(&bar, Qt::LeftButton, Qt::NoModifier, px(15.0 * kSec));
        QTest::mouseMove(&bar, px(25.0 * kSec));
        QTest::mouseRelease(&bar, Qt::LeftButton, Qt::NoModifier, px(25.0 * kSec));
        const plot::XSpan after = w.view->state().zoom;
        QVERIFY(std::abs(after.length() - before.length()) < 1.0);
        QVERIFY(std::abs((after.a - before.a) - 10.0 * kSec) <= 2.0 * pixel);
        // SHIFT+click: extended to the clicked position.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::ShiftModifier, px(60.0 * kSec));
        QVERIFY(std::abs(w.view->state().zoom.b - 60.0 * kSec) <= pixel);
        QCOMPARE(w.view->state().zoom.a, after.a);
        // Double-click: the whole outing.
        QTest::mouseDClick(&bar, Qt::LeftButton, Qt::NoModifier, px(0.0));
        QCOMPARE(w.view->state().zoom.a, -90.0 * kSec);
    }

    // Positions computed by hand: a 400 px bar at DPR 1 has 4 px margins, so the bar is x 4..396 (392 px)
    // and x = extent.a + (px - 4) / 392 * |extent|.
    void geometryByHand() {
        const project::Scheme scheme = *shippedScheme("light");
        plot::LayoutContext ctx;
        ctx.widthPx = 400;
        ctx.heightPx = 18;
        ctx.dpr = 1.0F;
        const plot::DrawList list = ZoomSummaryBar::buildDrawList(goldenSummary(), ctx, scheme);
        const auto pxOf = [](double s) { return 4.0 + (s + 30.0) / 160.0 * 392.0; };
        const plot::Rgba zoom = scheme.role("cursorZoom");
        bool spanFound = false;
        for (const plot::DrawCmd& c : list.cmds) {
            if (c.kind == plot::CmdKind::FilledRect && c.colour == ((zoom & 0xFFFFFF00U) | 0x50U)) {
                spanFound = true;
                QVERIFY(std::abs(static_cast<double>(c.rect.x) - pxOf(10.0)) < 0.01);
                QVERIFY(std::abs(static_cast<double>(c.rect.w) - (pxOf(35.0) - pxOf(10.0))) < 0.01);
                QCOMPARE(c.rect.y, 3.0F);
                QCOMPARE(c.rect.h, 12.0F);
            }
        }
        QVERIFY(spanFound);
        // The cursor line at round(px) + 0.5, the full height.
        bool cursorFound = false;
        for (const plot::DrawCmd& c : list.cmds) {
            if (c.kind == plot::CmdKind::Lines && c.colour == scheme.role("cursorMain")) {
                cursorFound = true;
                QCOMPARE(list.verts[c.vertexOffset * 2], static_cast<float>(std::round(pxOf(20.0)) + 0.5));
                QCOMPARE(list.verts[c.vertexOffset * 2 + 3], 18.0F);
            }
        }
        QVERIFY(cursorFound);
    }

    void goldens_data() {
        QTest::addColumn<bool>("valid");
        QTest::addColumn<int>("dpr");
        QTest::newRow("time@1x") << true << 1;
        QTest::newRow("time@2x") << true << 2;
        QTest::newRow("empty@1x") << false << 1;
        QTest::newRow("empty@2x") << false << 2;
    }
    void goldens() {
        QFETCH(bool, valid);
        QFETCH(int, dpr);
        const project::Scheme scheme = *shippedScheme("light");
        plot::LayoutContext ctx;
        ctx.dpr = static_cast<float>(dpr);
        ctx.widthPx = 400 * dpr;
        ctx.heightPx = 18 * dpr;
        const plot::DrawList list =
            ZoomSummaryBar::buildDrawList(valid ? goldenSummary() : ZoomSummaryBar::Summary{}, ctx, scheme);
        const QString stem =
            QStringLiteral("%1@%2x").arg(valid ? QStringLiteral("time") : QStringLiteral("empty")).arg(dpr);
        // DrawList JSON: exact everywhere.
        const QByteArray json = QByteArray::fromStdString(list.toJson());
        const QString jsonPath = goldenPath("drawlist", stem + QStringLiteral(".json"));
        if (updateGoldens()) {
            QDir().mkpath(QFileInfo(jsonPath).absolutePath());
            QFile out(jsonPath);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(json);
        } else {
            QFile in(jsonPath);
            QVERIFY2(in.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("missing golden ") + jsonPath));
            const QByteArray expected = in.readAll();
            if (expected != json) {
                QFile actual(artefactsDir() + QStringLiteral("/zoom_summary_") + stem +
                             QStringLiteral(".actual.json"));
                if (actual.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    actual.write(json);
                }
            }
            QVERIFY2(expected == json, qPrintable(QStringLiteral("DrawList golden differs: ") + jsonPath));
        }
        // Pixels: provisional until generated on Linux (D-127).
        const QImage image = render(list);
        QCOMPARE(image.width(), 400 * dpr);
        QCOMPARE(qt::compareImages(image, render(list), 0).differingPixels, std::int64_t{0}); // deterministic
        image.save(artefactsDir() + QStringLiteral("/zoom_summary_") + stem + QStringLiteral(".png"));
        const QString pngPath = goldenPath("images", stem + QStringLiteral(".png"));
        const QString metaPath = goldenPath("images", QStringLiteral("goldens.json"));
        if (updateGoldens()) {
            QDir().mkpath(QFileInfo(pngPath).absolutePath());
            QVERIFY(image.save(pngPath));
            QFile meta(metaPath);
            QVERIFY(meta.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QJsonObject m{
                {QStringLiteral("$comment"),
                 QStringLiteral(
                     "Provenance of the PNGs in this directory (libs/widgets/tests/properties_shell/"
                     "tst_zoom_summary_bar.cpp). Pixel goldens are authoritative on ubuntu only (D-127): "
                     "replace them from the qt-app ubuntu artefact and set generatedOn to linux.")},
                {QStringLiteral("generatedOn"), platformTag()},
                {QStringLiteral("platform"), QStringLiteral("offscreen")},
                {QStringLiteral("qt"), QString::fromLatin1(qVersion())}};
            meta.write(QJsonDocument(m).toJson());
            return;
        }
        QFile meta(metaPath);
        QVERIFY(meta.open(QIODevice::ReadOnly));
        const QString generatedOn =
            QJsonDocument::fromJson(meta.readAll()).object().value(QStringLiteral("generatedOn")).toString();
        const bool strict =
            qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
            (platformTag() == QLatin1String("linux") && generatedOn == QLatin1String("linux"));
        QImage golden(pngPath);
        QVERIFY2(!golden.isNull(), qPrintable(QStringLiteral("missing golden ") + pngPath));
        golden.setDevicePixelRatio(dpr);
        const qt::ImageDiff d = qt::compareImages(image, golden, qt::kGoldenChannelTolerance, true);
        if (!d.passes()) {
            d.diff.save(artefactsDir() + QStringLiteral("/zoom_summary_") + stem +
                        QStringLiteral(".diff.png"));
        }
        if (strict) {
            QVERIFY2(d.passes(), qPrintable(d.summary()));
        } else {
            qInfo("zoom_summary %s: %s (report only: generated on %s)", qPrintable(stem),
                  qPrintable(d.summary()), qPrintable(generatedOn));
        }
    }
};

QTEST_MAIN(TstZoomSummaryBar)
#include "tst_zoom_summary_bar.moc"
