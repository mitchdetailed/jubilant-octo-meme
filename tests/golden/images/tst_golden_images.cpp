// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-04 pixel goldens (07 section 4.2, 04 section 6.7, 08 section 6.4, D-127): the eight M1 time_distance
// worksheet cases of libs/plotcore/tests/td_golden_cases.hpp (single trace, 20 traces, like-units group, two
// panels, distance mode with an overlay, dark scheme, time overlays, the no-distance banner) rendered by
// qt::OffscreenRenderer::renderComponent at DPR 1 and 2 with the golden fonts (tests/golden/fonts, Noto Sans)
// and QtTextMetrics, against tests/golden/images/time_distance/<case>@<dpr>x.png; <case>.json describes each
// case and is compared on every OS (a changed case definition fails until the goldens are regenerated).
// Comparison rule: qt::compareImages (a pixel differs when a channel differs by more than 8/255; fail above
// 0.5 %). Where it is strict (D-127): pixel goldens are authoritative on ubuntu only, so the comparison fails
// the test on Linux when goldens.json says the PNGs were made on Linux (from the qt-app CI artefact), and
// everywhere with ESSES_PIXEL_GOLDENS_STRICT=1. Otherwise (Windows, macOS, or provisional PNGs on Linux) each
// case is a render smoke test (size, DPR tag, ink, determinism) and the pixel difference is only reported;
// the same cases are DrawList JSON goldens on every OS (plotcore.TimeDistanceBuilder: DrawList JSON
// goldens...). ESSES_UPDATE_GOLDENS=1 rewrites the PNGs, the case JSONs and goldens.json (generatedOn = this
// platform). Every render (and its diff) is written to <build>/tests/golden/artefacts/ for the CI artefact.

#include "../../../libs/plotcore/tests/td_golden_cases.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>
#include <cmath>
#include <memory>

using namespace esses;

namespace {

std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

QString goldenDir() {
    return QString::fromStdWString(
        (repoRoot() / "tests" / "golden" / "images" / "time_distance").lexically_normal().wstring());
}

QString artefactsDir() {
    const QString dir = QString::fromStdWString(
        (std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts").lexically_normal().wstring());
    QDir().mkpath(dir);
    return dir;
}

QString platformTag() {
#if defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("other");
#endif
}

bool updating() {
    return qEnvironmentVariableIntValue("ESSES_UPDATE_GOLDENS") == 1;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

const char* slotKindName(plot::SlotKind k) {
    switch (k) {
    case plot::SlotKind::Main:
        return "Main";
    case plot::SlotKind::Overlay0:
        return "Overlay0";
    case plot::SlotKind::Overlay:
        return "Overlay";
    }
    return "?";
}

// The case description committed next to its PNGs (what the image shows, for review and drift detection).
QByteArray caseJson(const plot::test::Fixture& fx, const plot::test::GoldenCase& gc) {
    QJsonObject o;
    o.insert(QStringLiteral("case"), QString::fromStdString(gc.name));
    o.insert(QStringLiteral("type"), QStringLiteral("time_distance"));
    o.insert(QStringLiteral("data"),
             QStringLiteral("libs/plotcore/tests/td_golden_cases.hpp (synthetic rational "
                            "data, sources synthetic_a/b/c.ld)"));
    o.insert(QStringLiteral("sizeLogical"), QJsonArray{gc.widthLogical, gc.heightLogical});
    o.insert(QStringLiteral("dprs"), QJsonArray{1, 2});
    o.insert(QStringLiteral("scheme"), gc.dark ? QStringLiteral("Test Dark") : QStringLiteral("Test Light"));
    o.insert(QStringLiteral("fonts"), QStringLiteral("tests/golden/fonts (Noto Sans), QtTextMetrics"));
    o.insert(QStringLiteral("domain"),
             gc.domain == plot::Domain::Distance ? QStringLiteral("distance") : QStringLiteral("time"));
    o.insert(QStringLiteral("zoom"), QJsonArray{gc.zoom.a, gc.zoom.b});
    o.insert(QStringLiteral("cursorUs"), static_cast<qint64>(gc.cursorT));
    o.insert(QStringLiteral("datumUs"),
             gc.datumT ? QJsonValue(static_cast<qint64>(*gc.datumT)) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("focused"), gc.focused);
    o.insert(QStringLiteral("pendingStripe"), false); // the offscreen render is settled (04 section 6.7)
    o.insert(QStringLiteral("singleColourMain"), gc.singleColourMain);
    o.insert(QStringLiteral("activeChannel"), QString::fromStdString(gc.activeChannel));
    o.insert(QStringLiteral("body"), QJsonDocument::fromJson(QByteArray::fromStdString(gc.body)).object());
    QJsonArray slots;
    for (const plot::test::GoldenCase::SlotSpec& s : gc.slots) {
        const plot::test::SyntheticSource& src = plot::test::sourceOf(fx, s.source);
        QJsonObject j;
        j.insert(QStringLiteral("source"), QString::fromStdString(src.source->prov.path.string()));
        j.insert(QStringLiteral("lap"), static_cast<int>(s.lap));
        j.insert(QStringLiteral("kind"), QString::fromLatin1(slotKindName(s.kind)));
        j.insert(QStringLiteral("number"), s.number);
        j.insert(QStringLiteral("distanceData"), !src.pieces.empty());
        slots.append(j);
    }
    o.insert(QStringLiteral("slots"), slots);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QString manifestPath() {
    return goldenDir() + QStringLiteral("/goldens.json");
}

QString goldensGeneratedOn() {
    return QJsonDocument::fromJson(readFile(manifestPath()))
        .object()
        .value(QStringLiteral("generatedOn"))
        .toString();
}

// The background colour's share of the image and the number of distinct colours (ink check).
int distinctColours(const QImage& image) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    QSet<QRgb> seen;
    for (int y = 0; y < img.height(); y += 2) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 2) {
            seen.insert(line[x]);
            if (seen.size() > 64) {
                return static_cast<int>(seen.size());
            }
        }
    }
    return static_cast<int>(seen.size());
}

} // namespace

class TstGoldenImages : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
            qputenv(
                "QT_QPA_FONTDIR",
                QString::fromStdWString((repoRoot() / "tests" / "golden" / "fonts").wstring()).toLocal8Bit());
        }
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(qt::SchemeFonts::registerApplicationFonts(repoRoot() / "tests" / "golden" / "fonts") > 0);
        QDir().mkpath(goldenDir());
        if (updating()) {
            QJsonObject m;
            m.insert(QStringLiteral("$comment"),
                     QStringLiteral("Provenance of the PNGs in this directory (tests/golden/images/"
                                    "tst_golden_images.cpp). Pixel goldens are authoritative on ubuntu only "
                                    "(D-127): replace them from the qt-app ubuntu artefact "
                                    "(<build>/tests/golden/artefacts) and set generatedOn to linux."));
            m.insert(QStringLiteral("generatedOn"), platformTag());
            m.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
            m.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
            QVERIFY(writeFile(manifestPath(), QJsonDocument(m).toJson(QJsonDocument::Indented)));
        }
        generatedOn_ = goldensGeneratedOn();
        strict_ = qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
                  (platformTag() == QLatin1String("linux") && generatedOn_ == QLatin1String("linux"));
        qInfo("pixel goldens generated on %s; comparison %s on %s", qPrintable(generatedOn_),
              strict_ ? "strict" : "reported only (render smoke test, D-127)", qPrintable(platformTag()));
    }

    void timeDistanceCases_data() {
        QTest::addColumn<int>("index");
        QTest::addColumn<double>("dpr");
        const plot::test::Fixture fx;
        const auto cases = plot::test::goldenCases(fx);
        QCOMPARE(cases.size(), std::size_t{8});
        for (std::size_t i = 0; i < cases.size(); ++i) {
            for (const double dpr : {1.0, 2.0}) {
                const QByteArray row =
                    QStringLiteral("%1@%2x").arg(QString::fromStdString(cases[i].name)).arg(dpr).toUtf8();
                QTest::newRow(row.constData()) << static_cast<int>(i) << dpr;
            }
        }
    }

    void timeDistanceCases() {
        QFETCH(int, index);
        QFETCH(double, dpr);
        const plot::test::Fixture fx;
        const auto cases = plot::test::goldenCases(fx);
        const plot::test::GoldenCase& gc = cases[static_cast<std::size_t>(index)];
        const QString stem = QStringLiteral("%1@%2x").arg(QString::fromStdString(gc.name)).arg(dpr);

        plot::ComponentBuildInputs in = plot::test::inputsOf(fx, gc, static_cast<float>(dpr));
        in.metrics = nullptr; // QtTextMetrics of the case's scheme with the golden fonts
        TaskPool pool(4);
        qt::OffscreenRenderer renderer(std::make_shared<const qt::SchemeFonts>(*in.scheme, 1), &pool);
        const QSize logical(gc.widthLogical, gc.heightLogical);
        const QImage image = renderer.renderComponent(in, logical, dpr);
        const QImage again = renderer.renderComponent(in, logical, dpr);
        image.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".png"));

        // Render smoke test (every OS).
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(static_cast<int>(std::lround(logical.width() * dpr)),
                                     static_cast<int>(std::lround(logical.height() * dpr))));
        QCOMPARE(image.devicePixelRatio(), dpr);
        QVERIFY2(distinctColours(image) >= 6, "background, frame, grid, text and traces");
        QCOMPARE(qt::compareImages(image, again, 0).differingPixels, std::int64_t{0}); // deterministic

        // The case description (every OS).
        const QString jsonPath =
            goldenDir() + QLatin1Char('/') + QString::fromStdString(gc.name) + QStringLiteral(".json");
        const QByteArray json = caseJson(fx, gc);
        const QString pngPath = goldenDir() + QLatin1Char('/') + stem + QStringLiteral(".png");
        if (updating()) {
            QVERIFY(writeFile(jsonPath, json));
            QVERIFY(image.save(pngPath, "PNG"));
            return;
        }
        QVERIFY2(
            readFile(jsonPath) == json,
            qPrintable(jsonPath + QStringLiteral(" differs from the case definition; regenerate the goldens "
                                                 "(ESSES_UPDATE_GOLDENS=1, then from the ubuntu artefact)")));

        // The pixel comparison.
        const QImage golden(pngPath);
        QVERIFY2(!golden.isNull(), qPrintable(pngPath + QStringLiteral(" is missing")));
        const qt::ImageDiff d = qt::compareImages(image, golden, qt::kGoldenChannelTolerance, true);
        if (!d.passes()) {
            d.diff.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
        }
        qInfo("%s: %s", qPrintable(stem), qPrintable(d.summary()));
        if (strict_) {
            QVERIFY2(d.passes(), qPrintable(stem + QStringLiteral(": ") + d.summary()));
        } else if (!d.passes()) {
            qWarning("%s differs from the %s-generated golden (reported only, D-127): %s", qPrintable(stem),
                     qPrintable(generatedOn_), qPrintable(d.summary()));
        }
    }

private:
    QString generatedOn_;
    bool strict_ = false;
};

QTEST_MAIN(TstGoldenImages)
#include "tst_golden_images.moc"
