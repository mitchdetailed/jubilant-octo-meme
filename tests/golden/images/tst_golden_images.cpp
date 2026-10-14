// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-04 and AC-M3-02 pixel goldens of time_distance (07 sections 4.2 and 6.2, 04 section 6.7, 08
// section 6.4, D-127): the eight M1 worksheet cases of libs/plotcore/tests/td_golden_cases.hpp (single trace,
// 20 traces, like-units group, two panels, distance mode with an overlay, dark scheme, time overlays, the
// no-distance banner) and the thirteen M3 cases of libs/plotcore/tests/td_m3_cases.hpp (variance in both
// domains and its banner, the datum key, measurements, the offset axis in both domains, the bands,
// annotations and device events, the status strip, overlapped mode, scale lines with the crosshair, the dark
// full anatomy), rendered by qt::OffscreenRenderer::renderComponent at DPR 1 and 2 with the golden fonts
// (tests/golden/fonts, Noto Sans) and QtTextMetrics, against
// tests/golden/images/time_distance/<case>@<dpr>x.png; <case>.json describes each case and is compared on
// every OS (a changed case definition fails until the goldens are regenerated). Comparison rule:
// qt::compareImages (a pixel differs when a channel differs by more than 8/255; fail above 0.5 %). Where it
// is strict (D-127): pixel goldens are authoritative on ubuntu only, so the comparison fails the test on
// Linux when goldens.json says the case's PNGs were made on Linux (from the qt-app CI artefact), and
// everywhere with ESSES_PIXEL_GOLDENS_STRICT=1. goldens.json's "generatedOn" holds for every case not listed
// in its "cases" object, which names the provisional ones (rendered elsewhere, or stale until replaced from
// the artefact). Otherwise (Windows, macOS, or provisional PNGs on Linux) each case is a render smoke test
// (size, DPR tag, ink, determinism) and the pixel difference is only reported; the same cases are DrawList
// JSON goldens on every OS (plotcore.TimeDistanceBuilder: DrawList JSON goldens...). ESSES_UPDATE_GOLDENS=1
// rewrites the PNGs, the case JSONs and goldens.json (generatedOn = this platform, no provisional cases);
// ESSES_UPDATE_GOLDENS_CASES=<name,name,...> rewrites only those cases' PNGs and JSONs and lists them in
// "cases" as rendered on this platform; ESSES_UPDATE_GOLDEN_JSON=1 rewrites the case JSONs only. Every render
// (and its diff) is written to <build>/tests/golden/artefacts/ for the CI artefact.

#include "../../../libs/plotcore/tests/td_golden_cases.hpp"
#include "../../../libs/plotcore/tests/td_m3_cases.hpp"

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

// The cases ESSES_UPDATE_GOLDENS_CASES names (empty: none).
QStringList casesToUpdate() {
    return qEnvironmentVariable("ESSES_UPDATE_GOLDENS_CASES").split(QLatin1Char(','), Qt::SkipEmptyParts);
}

bool updatingJson() {
    return qEnvironmentVariableIntValue("ESSES_UPDATE_GOLDEN_JSON") == 1;
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

QByteArray m3CaseJson(const plot::test::M3Fixture& m, const plot::test::M3Case& mc) {
    QJsonObject o;
    o.insert(QStringLiteral("case"), QString::fromStdString(mc.name));
    o.insert(QStringLiteral("type"), QStringLiteral("time_distance"));
    o.insert(QStringLiteral("shows"), QString::fromStdString(mc.shows));
    o.insert(
        QStringLiteral("data"),
        QStringLiteral("libs/plotcore/tests/td_m3_cases.hpp (the synthetic sources of td_golden_cases.hpp "
                       "with laps, track sections, markers, device events, status channels, annotations "
                       "and lap distance indexes)"));
    o.insert(QStringLiteral("sizeLogical"), QJsonArray{mc.widthLogical, mc.heightLogical});
    o.insert(QStringLiteral("dprs"), QJsonArray{1, 2});
    o.insert(QStringLiteral("scheme"), mc.dark ? QStringLiteral("Test Dark") : QStringLiteral("Test Light"));
    o.insert(QStringLiteral("fonts"), QStringLiteral("tests/golden/fonts (Noto Sans), QtTextMetrics"));
    o.insert(QStringLiteral("domain"),
             mc.domain == plot::Domain::Distance ? QStringLiteral("distance") : QStringLiteral("time"));
    o.insert(QStringLiteral("zoom"), QJsonArray{mc.zoom.a, mc.zoom.b});
    o.insert(QStringLiteral("cursorUs"), static_cast<qint64>(mc.cursorT));
    o.insert(QStringLiteral("datumUs"),
             mc.datumT ? QJsonValue(static_cast<qint64>(*mc.datumT)) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("offsetAxis"), mc.offsetAxis);
    o.insert(QStringLiteral("focused"), mc.focused);
    o.insert(QStringLiteral("pendingStripe"), false);
    o.insert(QStringLiteral("activeChannel"), QString::fromStdString(mc.activeChannel));
    QJsonObject limits;
    for (const auto& [name, range] : mc.limits) {
        limits.insert(QString::fromStdString(name), QJsonArray{range.first, range.second});
    }
    o.insert(QStringLiteral("overrideLimitsBase"), limits);
    o.insert(QStringLiteral("body"), QJsonDocument::fromJson(QByteArray::fromStdString(mc.body)).object());
    QJsonArray slots;
    for (const plot::test::M3Case::SlotSpec& s : mc.slots) {
        const plot::test::SyntheticSource& src = plot::test::sourceOf(m.fx, s.source);
        QJsonObject j;
        j.insert(QStringLiteral("source"), QString::fromStdString(src.source->prov.path.string()));
        j.insert(QStringLiteral("lap"), static_cast<int>(s.lap));
        j.insert(QStringLiteral("kind"), QString::fromLatin1(slotKindName(s.kind)));
        j.insert(QStringLiteral("number"), s.number);
        j.insert(QStringLiteral("offsetUs"), static_cast<qint64>(s.offsetUs));
        j.insert(QStringLiteral("offsetM"), s.offsetM);
        j.insert(QStringLiteral("distanceData"), !src.pieces.empty());
        slots.append(j);
    }
    o.insert(QStringLiteral("slots"), slots);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QString manifestPath() {
    return goldenDir() + QStringLiteral("/goldens.json");
}

QJsonObject manifest() {
    return QJsonDocument::fromJson(readFile(manifestPath())).object();
}

// Where a case's PNGs were made: its "cases" entry, else the manifest's generatedOn.
QString generatedOnOf(const QJsonObject& m, const QString& name) {
    const QJsonValue entry = m.value(QStringLiteral("cases")).toObject().value(name);
    if (entry.isString()) {
        return entry.toString();
    }
    return m.value(QStringLiteral("generatedOn")).toString();
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
            m.insert(
                QStringLiteral("$comment"),
                QStringLiteral("Provenance of the PNGs in this directory (tests/golden/images/"
                               "tst_golden_images.cpp). Pixel goldens are authoritative on ubuntu only "
                               "(D-127): replace them from the qt-app ubuntu artefact "
                               "(<build>/tests/golden/artefacts) and set generatedOn to linux; \"cases\" "
                               "lists the provisional cases with where their PNGs came from."));
            m.insert(QStringLiteral("generatedOn"), platformTag());
            m.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
            m.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
            QVERIFY(writeFile(manifestPath(), QJsonDocument(m).toJson(QJsonDocument::Indented)));
        } else if (!casesToUpdate().isEmpty()) {
            QJsonObject m = manifest();
            QJsonObject cases = m.value(QStringLiteral("cases")).toObject();
            for (const QString& name : casesToUpdate()) {
                cases.insert(name, platformTag());
            }
            m.insert(QStringLiteral("cases"), cases);
            QVERIFY(writeFile(manifestPath(), QJsonDocument(m).toJson(QJsonDocument::Indented)));
        }
        manifest_ = manifest();
        qInfo("pixel goldens generated on %s (%d provisional cases) on %s",
              qPrintable(manifest_.value(QStringLiteral("generatedOn")).toString()),
              static_cast<int>(manifest_.value(QStringLiteral("cases")).toObject().size()),
              qPrintable(platformTag()));
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
        plot::ComponentBuildInputs in = plot::test::inputsOf(fx, gc, static_cast<float>(dpr));
        check(QString::fromStdString(gc.name), dpr, std::move(in), QSize(gc.widthLogical, gc.heightLogical),
              caseJson(fx, gc));
    }

    void timeDistanceM3Cases_data() {
        QTest::addColumn<int>("index");
        QTest::addColumn<double>("dpr");
        const plot::test::M3Fixture m;
        const auto cases = plot::test::m3Cases(m);
        QVERIFY(cases.size() >= 12); // AC-M3-02: with the eight M1 cases, well over 12 time_distance cases
        for (std::size_t i = 0; i < cases.size(); ++i) {
            for (const double dpr : {1.0, 2.0}) {
                const QByteArray row =
                    QStringLiteral("%1@%2x").arg(QString::fromStdString(cases[i].name)).arg(dpr).toUtf8();
                QTest::newRow(row.constData()) << static_cast<int>(i) << dpr;
            }
        }
    }

    void timeDistanceM3Cases() {
        QFETCH(int, index);
        QFETCH(double, dpr);
        const plot::test::M3Fixture m;
        const auto cases = plot::test::m3Cases(m);
        const plot::test::M3Case& mc = cases[static_cast<std::size_t>(index)];
        plot::ComponentBuildInputs in = plot::test::m3InputsOf(m, mc, static_cast<float>(dpr));
        check(QString::fromStdString(mc.name), dpr, std::move(in), QSize(mc.widthLogical, mc.heightLogical),
              m3CaseJson(m, mc));
    }

private:
    void check(const QString& name, double dpr, plot::ComponentBuildInputs in, QSize logical,
               const QByteArray& json) {
        const QString stem = QStringLiteral("%1@%2x").arg(name).arg(dpr);
        in.metrics = nullptr; // QtTextMetrics of the case's scheme with the golden fonts
        TaskPool pool(4);
        qt::OffscreenRenderer renderer(std::make_shared<const qt::SchemeFonts>(*in.scheme, 1), &pool);
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
        const QString jsonPath = goldenDir() + QLatin1Char('/') + name + QStringLiteral(".json");
        const QString pngPath = goldenDir() + QLatin1Char('/') + stem + QStringLiteral(".png");
        if (updating() || casesToUpdate().contains(name)) {
            QVERIFY(writeFile(jsonPath, json));
            QVERIFY(image.save(pngPath, "PNG"));
            return;
        }
        if (updatingJson()) {
            QVERIFY(writeFile(jsonPath, json));
        }
        QVERIFY2(
            readFile(jsonPath) == json,
            qPrintable(jsonPath + QStringLiteral(" differs from the case definition; regenerate the goldens "
                                                 "(ESSES_UPDATE_GOLDENS=1, then from the ubuntu artefact)")));

        // The pixel comparison.
        const QString generatedOn = generatedOnOf(manifest_, name);
        const bool strict =
            qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
            (platformTag() == QLatin1String("linux") && generatedOn == QLatin1String("linux"));
        const QImage golden(pngPath);
        QVERIFY2(!golden.isNull(), qPrintable(pngPath + QStringLiteral(" is missing")));
        const qt::ImageDiff d = qt::compareImages(image, golden, qt::kGoldenChannelTolerance, true);
        if (!d.passes()) {
            d.diff.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
        }
        qInfo("%s: %s (golden from %s, %s)", qPrintable(stem), qPrintable(d.summary()),
              qPrintable(generatedOn), strict ? "strict" : "reported only");
        if (strict) {
            QVERIFY2(d.passes(), qPrintable(stem + QStringLiteral(": ") + d.summary()));
        } else if (!d.passes()) {
            qWarning("%s differs from the %s golden (reported only, D-127): %s", qPrintable(stem),
                     qPrintable(generatedOn), qPrintable(d.summary()));
        }
    }

    QJsonObject manifest_;
};

QTEST_MAIN(TstGoldenImages)
#include "tst_golden_images.moc"
