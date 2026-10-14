// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M3-02 pixel goldens of the plots work (04 section 6.7, 08 section 6.4, D-127, D-198): three cases each
// of scatter, histogram, outing, gauge_numeric and gauge_bar (libs/plotcore/tests/plots_golden_cases.hpp,
// synthetic rational data) rendered by qt::OffscreenRenderer::renderComponent at DPR 1 and 2 with the golden
// fonts and QtTextMetrics, through the whole builders (the analysis binners and TrendFit included), against
// tests/golden/images/<type>/<case>@<dpr>x.png; <case>.json describes each case and is compared on every OS.
// The comparison follows tst_golden_images.cpp: strict on Linux when the directory's goldens.json says the
// PNGs were made on Linux (the qt-app CI artefact), and everywhere with ESSES_PIXEL_GOLDENS_STRICT=1;
// otherwise each case is a render smoke test (size, DPR tag, ink, determinism) and the difference is reported
// only. The PNGs committed with this work were rendered on Windows (goldens.json "generatedOn": "windows"):
// provisional until they are replaced from the ubuntu artefact. ESSES_UPDATE_GOLDENS=1 rewrites the PNGs, the
// case JSONs and the goldens.json files (generatedOn = this platform). Every render (and its diff) is written
// to <build>/tests/golden/artefacts/plots/.

#include "../../../libs/plotcore/tests/plots_golden_cases.hpp"

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
#include <map>
#include <memory>

using namespace esses;

namespace {

std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

QString goldenDir(const std::string& type) {
    return QString::fromStdWString(
        (repoRoot() / "tests" / "golden" / "images" / type).lexically_normal().wstring());
}

QString artefactsDir() {
    const QString dir = QString::fromStdWString(
        (std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts" / "plots").lexically_normal().wstring());
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
QByteArray caseJson(const plot::test::Fixture& fx, const plot::test::PlotGoldenCase& c) {
    QJsonObject o;
    o.insert(QStringLiteral("case"), QString::fromStdString(c.name));
    o.insert(QStringLiteral("type"), QString::fromStdString(c.type));
    o.insert(QStringLiteral("data"),
             QStringLiteral("libs/plotcore/tests/td_golden_cases.hpp (synthetic rational data, sources "
                            "synthetic_a/b/c.ld); cases in libs/plotcore/tests/plots_golden_cases.hpp"));
    o.insert(QStringLiteral("sizeLogical"), QJsonArray{c.width, c.height});
    o.insert(QStringLiteral("dprs"), QJsonArray{1, 2});
    o.insert(QStringLiteral("scheme"), c.dark ? QStringLiteral("Test Dark") : QStringLiteral("Test Light"));
    o.insert(QStringLiteral("fonts"), QStringLiteral("tests/golden/fonts (Noto Sans), QtTextMetrics"));
    o.insert(QStringLiteral("domain"),
             c.domain == plot::Domain::Distance ? QStringLiteral("distance") : QStringLiteral("time"));
    o.insert(QStringLiteral("zoom"), QJsonArray{c.zoom.a, c.zoom.b});
    o.insert(QStringLiteral("cursorUs"), static_cast<qint64>(c.cursorT));
    o.insert(QStringLiteral("datumUs"),
             c.datumT ? QJsonValue(static_cast<qint64>(*c.datumT)) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("focused"), c.focused);
    o.insert(QStringLiteral("caption"), c.caption);
    o.insert(QStringLiteral("colourLegend"), c.legend);
    o.insert(QStringLiteral("pendingStripe"), false); // the offscreen render is settled (04 section 6.7)
    o.insert(QStringLiteral("activeChannel"), QString::fromStdString(c.activeChannel));
    o.insert(QStringLiteral("body"), QJsonDocument::fromJson(QByteArray::fromStdString(c.body)).object());
    QJsonArray slots;
    for (const plot::test::GoldenCase::SlotSpec& s : c.slots) {
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

QString manifestPath(const std::string& type) {
    return goldenDir(type) + QStringLiteral("/goldens.json");
}

QString goldensGeneratedOn(const std::string& type) {
    return QJsonDocument::fromJson(readFile(manifestPath(type)))
        .object()
        .value(QStringLiteral("generatedOn"))
        .toString();
}

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

const char* const kTypes[] = {"scatter", "histogram", "outing", "gauge_numeric", "gauge_bar"};

} // namespace

class TstGoldenPlots : public QObject {
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
        for (const char* type : kTypes) {
            QDir().mkpath(goldenDir(type));
            if (updating()) {
                QJsonObject m;
                m.insert(
                    QStringLiteral("$comment"),
                    QStringLiteral("Provenance of the PNGs in this directory (tests/golden/images/"
                                   "tst_golden_plots.cpp). Pixel goldens are authoritative on ubuntu only "
                                   "(D-127): replace them from the qt-app ubuntu artefact "
                                   "(<build>/tests/golden/artefacts/plots) and set generatedOn to linux."));
                m.insert(QStringLiteral("generatedOn"), platformTag());
                m.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
                m.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
                QVERIFY(writeFile(manifestPath(type), QJsonDocument(m).toJson(QJsonDocument::Indented)));
            }
            const QString on = goldensGeneratedOn(type);
            generatedOn_[type] = on;
            const bool strict = qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
                                (platformTag() == QLatin1String("linux") && on == QLatin1String("linux"));
            strict_[type] = strict;
            qInfo("%s pixel goldens generated on %s; comparison %s on %s", type, qPrintable(on),
                  strict ? "strict" : "reported only (render smoke test, D-127)", qPrintable(platformTag()));
        }
    }

    void plotCases_data() {
        QTest::addColumn<int>("index");
        QTest::addColumn<double>("dpr");
        const plot::test::Fixture fx;
        const auto cases = plot::test::plotGoldenCases(fx);
        QCOMPARE(cases.size(), std::size_t{15});
        std::map<std::string, int> perType;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            ++perType[cases[i].type];
            for (const double dpr : {1.0, 2.0}) {
                const QByteArray row =
                    QStringLiteral("%1/%2@%3x")
                        .arg(QString::fromStdString(cases[i].type), QString::fromStdString(cases[i].name))
                        .arg(dpr)
                        .toUtf8();
                QTest::newRow(row.constData()) << static_cast<int>(i) << dpr;
            }
        }
        for (const char* type : kTypes) {
            QVERIFY2(perType[type] >= 3, type); // AC-M3-02: at least three cases per component
        }
    }

    void plotCases() {
        QFETCH(int, index);
        QFETCH(double, dpr);
        const plot::test::Fixture fx;
        QVERIFY(plot::test::makeResident(fx.a));
        QVERIFY(plot::test::makeResident(fx.b));
        const auto cases = plot::test::plotGoldenCases(fx);
        const plot::test::PlotGoldenCase& c = cases[static_cast<std::size_t>(index)];
        const QString stem = QStringLiteral("%1@%2x").arg(QString::fromStdString(c.name)).arg(dpr);

        plot::ComponentBuildInputs in = plot::test::plotGoldenInputs(fx, c, static_cast<float>(dpr));
        in.metrics = nullptr; // QtTextMetrics of the case's scheme with the golden fonts
        TaskPool pool(4);
        qt::OffscreenRenderer renderer(std::make_shared<const qt::SchemeFonts>(*in.scheme, 1), &pool);
        const QSize logical(c.width, c.height);
        const QImage image = renderer.renderComponent(in, logical, dpr);
        const QImage again = renderer.renderComponent(in, logical, dpr);
        image.save(artefactsDir() + QLatin1Char('/') + QString::fromStdString(c.type) + QLatin1Char('_') +
                   stem + QStringLiteral(".png"));

        // Render smoke test (every OS).
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(static_cast<int>(std::lround(logical.width() * dpr)),
                                     static_cast<int>(std::lround(logical.height() * dpr))));
        QCOMPARE(image.devicePixelRatio(), dpr);
        QVERIFY2(distinctColours(image) >= 4, "background, frame, text and data");
        QCOMPARE(qt::compareImages(image, again, 0).differingPixels, std::int64_t{0}); // deterministic

        // The case description (every OS).
        const QString dir = goldenDir(c.type);
        const QString jsonPath =
            dir + QLatin1Char('/') + QString::fromStdString(c.name) + QStringLiteral(".json");
        const QByteArray json = caseJson(fx, c);
        const QString pngPath = dir + QLatin1Char('/') + stem + QStringLiteral(".png");
        if (updating()) {
            QVERIFY(writeFile(jsonPath, json));
            QVERIFY(image.save(pngPath, "PNG"));
            return;
        }
        QVERIFY2(readFile(jsonPath) == json,
                 qPrintable(jsonPath + QStringLiteral(" differs from the case definition; regenerate the "
                                                      "goldens (ESSES_UPDATE_GOLDENS=1, then from the ubuntu "
                                                      "artefact)")));

        // The pixel comparison.
        const QImage golden(pngPath);
        QVERIFY2(!golden.isNull(), qPrintable(pngPath + QStringLiteral(" is missing")));
        const qt::ImageDiff d = qt::compareImages(image, golden, qt::kGoldenChannelTolerance, true);
        if (!d.passes()) {
            d.diff.save(artefactsDir() + QLatin1Char('/') + QString::fromStdString(c.type) +
                        QLatin1Char('_') + stem + QStringLiteral(".diff.png"));
        }
        qInfo("%s/%s: %s", c.type.c_str(), qPrintable(stem), qPrintable(d.summary()));
        if (strict_[c.type]) {
            QVERIFY2(d.passes(), qPrintable(stem + QStringLiteral(": ") + d.summary()));
        } else if (!d.passes()) {
            qWarning("%s differs from the %s-generated golden (reported only, D-127): %s", qPrintable(stem),
                     qPrintable(generatedOn_[c.type]), qPrintable(d.summary()));
        }
    }

private:
    std::map<std::string, QString> generatedOn_;
    std::map<std::string, bool> strict_;
};

QTEST_MAIN(TstGoldenPlots)
#include "tst_golden_plots.moc"
