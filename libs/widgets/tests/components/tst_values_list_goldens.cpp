// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Goldens of values_list (AC-M3-02: three cases at DPR 1 and 2; 04 sections 6.7 and 15; D-127, D-202):
//   * pixel goldens tests/golden/images/values_list/<case>@<dpr>x.png: the table painted by
//     QWidget::render (its Print to Clipboard, 04 section 7.2 item 9). A pixel differs when a channel
//     differs by more than 8/255;
//     an image fails above 0.5 % differing pixels, but only where the PNGs are authoritative (D-127: Linux
//     with goldens.json "generatedOn": "linux", or ESSES_PIXEL_GOLDENS_STRICT=1); elsewhere each case is a
//     render smoke test (size, DPR tag, ink, two renders identical) and the difference is reported. The PNGs
//     of this change were rendered on Windows and are provisional ("generatedOn": "windows") until they are
//     replaced from the ubuntu qt-app artefact (<build>/libs/widgets/artefacts/values_list/).
//   * exact goldens, checked on every OS: the table's headers and cell text
//     (tests/golden/tables/values_list/<case>.json), plus cells computed by hand from the synthetic source's
//     closed form (Throttle = 50 + 50 cos(2 pi u), u the fraction of the lap; D-041).
// ESSES_UPDATE_GOLDENS=1 rewrites the PNGs (and goldens.json, generatedOn = this platform) and the table
// goldens.

#include "components/component_test_support.hpp"

#include <esses/qtmodel/values_model.hpp>
#include <esses/widgets/components/values_list.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableView>
#include <QTest>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;

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

QString pathOf(const std::filesystem::path& p) {
    return QString::fromStdWString(p.lexically_normal().wstring());
}

QString imagesDir() {
    return pathOf(repoRoot() / "tests" / "golden" / "images" / "values_list");
}

QString artefactsDir() {
    const QString dir = pathOf(std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts" / "values_list");
    QDir().mkpath(dir);
    return dir;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    bytes.replace("\r\n", "\n");
    return bytes;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

struct Diff {
    int differing = 0;
    int total = 0;
    QImage image;
};

Diff compare(const QImage& a, const QImage& b) {
    Diff d;
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    d.total = x.width() * x.height();
    d.image = QImage(x.size(), QImage::Format_ARGB32);
    d.image.fill(Qt::white);
    for (int r = 0; r < x.height(); ++r) {
        for (int c = 0; c < x.width(); ++c) {
            const QRgb p = x.pixel(c, r);
            const QRgb q = y.pixel(c, r);
            if (std::abs(qRed(p) - qRed(q)) > 8 || std::abs(qGreen(p) - qGreen(q)) > 8 ||
                std::abs(qBlue(p) - qBlue(q)) > 8 || std::abs(qAlpha(p) - qAlpha(q)) > 8) {
                ++d.differing;
                d.image.setPixel(c, r, qRgb(255, 0, 0));
            }
        }
    }
    return d;
}

bool hasInk(const QImage& image) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb first = img.pixel(0, 0);
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            if (img.pixel(x, y) != first) {
                return true;
            }
        }
    }
    return false;
}

void checkPixels(const QString& name, qreal dpr, const QImage& render, const QImage& again) {
    const QString stem = name + QStringLiteral("@%1x").arg(static_cast<int>(dpr));
    render.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".png"));
    QVERIFY(!render.isNull());
    QCOMPARE(render.devicePixelRatio(), dpr);
    QVERIFY(hasInk(render));
    QCOMPARE(compare(render, again).differing, 0);
    const QString png = imagesDir() + QLatin1Char('/') + stem + QStringLiteral(".png");
    if (updating()) {
        QDir().mkpath(imagesDir());
        QVERIFY(render.save(png));
        return;
    }
    const QImage golden(png);
    QVERIFY2(!golden.isNull(),
             qPrintable(QStringLiteral("missing golden %1 (ESSES_UPDATE_GOLDENS=1)").arg(png)));
    const QString generatedOn =
        QJsonDocument::fromJson(readFile(imagesDir() + QStringLiteral("/goldens.json")))
            .object()
            .value(QStringLiteral("generatedOn"))
            .toString();
    const bool strict = qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
                        (platformTag() == QLatin1String("linux") && generatedOn == QLatin1String("linux"));
    if (golden.size() != render.size()) {
        if (strict) {
            QFAIL(qPrintable(QStringLiteral("values_list/%1: size %2x%3, golden %4x%5")
                                 .arg(stem)
                                 .arg(render.width())
                                 .arg(render.height())
                                 .arg(golden.width())
                                 .arg(golden.height())));
        }
        qInfo("values_list/%s: size differs from the provisional golden (reported only)", qPrintable(stem));
        return;
    }
    const Diff d = compare(render, golden);
    const double pct = d.total > 0 ? 100.0 * d.differing / d.total : 0.0;
    if (d.differing > 0) {
        d.image.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
    }
    if (strict) {
        QVERIFY2(pct <= 0.5,
                 qPrintable(QStringLiteral("values_list/%1: %2 % of pixels differ").arg(stem).arg(pct)));
    } else {
        qInfo("values_list/%s: %.3f %% of pixels differ from the golden made on %s (reported only, D-127)",
              qPrintable(stem), pct, qPrintable(generatedOn));
    }
}

void writeManifest() {
    if (!updating()) {
        return;
    }
    QJsonObject m;
    m.insert(QStringLiteral("$comment"),
             QStringLiteral("Provenance of the PNGs in this directory (libs/widgets/tests/components/"
                            "tst_values_list_goldens.cpp). Pixel goldens are authoritative on ubuntu only "
                            "(D-127): replace them from the qt-app ubuntu artefact (<build>/libs/widgets/"
                            "artefacts/values_list) and set generatedOn to linux."));
    m.insert(QStringLiteral("generatedOn"), platformTag());
    m.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
    m.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
    m.insert(QStringLiteral("source"), QStringLiteral("rendered locally: provisional"));
    QVERIFY(writeFile(imagesDir() + QStringLiteral("/goldens.json"),
                      QJsonDocument(m).toJson(QJsonDocument::Indented)));
}

void checkText(const std::filesystem::path& path, const QByteArray& actual) {
    const QString file = pathOf(path);
    if (updating()) {
        QVERIFY(writeFile(file, actual));
        return;
    }
    const QByteArray expected = readFile(file);
    if (expected != actual) {
        const QString out = pathOf(std::filesystem::path(ESSES_TEST_BINARY_DIR) /
                                   ("values_list_" + path.stem().string() + ".actual.json")); // CI artefact
        writeFile(out, actual);
        QFAIL(qPrintable(QStringLiteral("golden %1 differs%2; actual written to %3")
                             .arg(file, expected.isEmpty() ? QStringLiteral(" (missing)") : QString(), out)));
    }
}

// Headers, then each row's cells (the hidden columns included: the model is the same in every layout).
QByteArray tableJson(const QString& name, const qt::ValuesModel& m, const QTableView& view) {
    QJsonArray headers;
    QJsonArray hidden;
    for (int c = 0; c < m.columnCount(); ++c) {
        headers.append(m.headerData(c, Qt::Horizontal).toString());
        if (view.isColumnHidden(c)) {
            hidden.append(c);
        }
    }
    QJsonArray rows;
    for (int r = 0; r < m.rowCount(); ++r) {
        QJsonArray row;
        for (int c = 0; c < m.columnCount(); ++c) {
            row.append(m.cellText(r, c));
        }
        rows.append(row);
    }
    const QJsonObject o{{QStringLiteral("case"), name},
                        {QStringLiteral("headers"), headers},
                        {QStringLiteral("hiddenColumns"), hidden},
                        {QStringLiteral("showHeading"), view.horizontalHeader()->isVisible()},
                        {QStringLiteral("rows"), rows}};
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

project::ComponentSpec withBody(project::ComponentSpec spec, const QJsonObject& body) {
    Result<model::OpaqueJson> json =
        model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    spec.body = std::move(json).value();
    return spec;
}

QJsonObject row(const char* channel, int dps, const char* unit = nullptr) {
    QJsonObject o{{QStringLiteral("channel"), QString::fromLatin1(channel)}, {QStringLiteral("dps"), dps}};
    if (unit != nullptr) {
        o.insert(QStringLiteral("displayUnit"), QString::fromLatin1(unit));
    }
    return o;
}

struct Case {
    QString name;
    QSize size;
    QJsonObject body;
    std::size_t main = 1;                 // lap index (0 = out lap)
    std::optional<std::size_t> reference; // the reference overlay's lap
    Micros cursor = 0;
};

std::vector<Case> cases() {
    const QJsonObject defaults{
        {QStringLiteral("columns"), QJsonObject{{QStringLiteral("datum"), false},
                                                {QStringLiteral("delta"), true},
                                                {QStringLiteral("inlineUnits"), true}}},
        {QStringLiteral("showHeading"), true},
        {QStringLiteral("autoSizeColumns"), true}};
    QJsonObject reference = defaults;
    reference.insert(QStringLiteral("channels"),
                     QJsonArray{row("Throttle", 2), row("Wheel Speed FL", 2, "m/s")});
    QJsonObject fixed{
        {QStringLiteral("channels"), QJsonArray{row("Throttle", 0), row("Wheel Speed FL", 1, "km/h")}},
        {QStringLiteral("columns"), QJsonObject{{QStringLiteral("datum"), false},
                                                {QStringLiteral("delta"), false},
                                                {QStringLiteral("inlineUnits"), false}}},
        {QStringLiteral("showHeading"), false},
        {QStringLiteral("autoSizeColumns"), false},
        {QStringLiteral("widths"), QJsonObject{{QStringLiteral("name"), 130},
                                               {QStringLiteral("value"), 70},
                                               {QStringLiteral("delta"), 70},
                                               {QStringLiteral("datum"), 70}}}};
    QJsonObject derived = defaults;
    derived.insert(QStringLiteral("channels"),
                   QJsonArray{row("Corr Speed", 1, "km/h"), row("Corr Dist", 0, "m"), row("Throttle", 1),
                              row("Missing Channel", 1)});
    return {Case{QStringLiteral("main_reference"), QSize(340, 90), reference, 1, 2, 10 * kSec},
            Case{QStringLiteral("main_only_fixed"), QSize(280, 70), fixed, 2, std::nullopt, 12 * kSec},
            Case{QStringLiteral("corr_speed_delta"), QSize(380, 150), derived, 2, 3, 20 * kSec}};
}

// Every displayed cell of the rows that have data is final (no pending "…").
bool settled(const qt::ValuesModel& m, bool reference) {
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.cellText(r, qt::ValuesModel::NameCol) == QLatin1String("Missing Channel")) {
            continue;
        }
        const QString main = m.cellText(r, qt::ValuesModel::MainCol);
        const QString ref = m.cellText(r, qt::ValuesModel::ReferenceCol);
        if (main.isEmpty() || main.contains(QChar(0x2026)) ||
            (reference && (ref.isEmpty() || ref.contains(QChar(0x2026))))) {
            return false;
        }
    }
    return true;
}

} // namespace

class TstValuesListGoldens : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(registerGoldenFonts() > 0);
        registerBuiltinComponents();
    }

    void valuesList_data() {
        QTest::addColumn<int>("index");
        const auto all = cases();
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            QTest::newRow(qPrintable(all[static_cast<std::size_t>(i)].name)) << i;
        }
    }
    void valuesList() {
        QFETCH(int, index);
        const Case c = cases()[static_cast<std::size_t>(index)];
        World w;
        const auto src = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*src, c.main));
        if (c.reference) {
            w.selection.toggleOverlay(lapRef(*src, *c.reference));
        }
        if (c.name == QLatin1String("corr_speed_delta")) {
            QVERIFY(QTest::qWaitFor([&] { return w.distanceReady(src); }, 30000));
        }
        w.view->setCursor(c.cursor, qt::CursorSource::Programmatic);
        // Placed as a worksheet places it (showPlaced): as a window of its own, main_only_fixed (280 x 70)
        // was held at its table's minimum on macOS (280 x 90).
        QWidget host;
        auto* list = new ValuesList(
            withBody(valuesListSpec({}, {}, 0, "5c3f1a52-8d7e-4b8e-9a51-0c6d2f4e7a10"), c.body), *w.context,
            &host);
        QVERIFY(showPlaced(host, *list, c.size));
        QCOMPARE(list->size(), c.size);
        qt::ValuesModel& m = list->model();
        QTRY_VERIFY_WITH_TIMEOUT(settled(m, c.reference.has_value()), 30000);
        QTest::qWait(1);

        if (c.name == QLatin1String("main_reference")) {
            // By hand: Throttle = 50 + 50 cos(2 pi u); Main is the 50 s lap, the overlay the 48 s lap, and
            // the overlay's time is the cursor's lap-relative time (time domain): u = 10/50 and 10/48.
            const auto throttle = [](double u) { return 50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * u); };
            QCOMPARE(m.cellText(0, qt::ValuesModel::MainCol), QString::number(throttle(10.0 / 50.0), 'f', 2));
            QCOMPARE(m.cellText(0, qt::ValuesModel::ReferenceCol),
                     QString::number(throttle(10.0 / 48.0), 'f', 2));
        }
        if (c.name == QLatin1String("main_only_fixed")) {
            // By hand: at 12 s of the 48 s lap u = 1/4, so Throttle = 50 and Wheel Speed FL = 2000 m / 48 s *
            // (1 + 0.3 sin(pi / 2)) = 54.1667 m/s = 195.0 km/h.
            QCOMPARE(m.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("50"));
            QCOMPARE(m.cellText(1, qt::ValuesModel::MainCol), QStringLiteral("195.0"));
            QVERIFY(list->table().horizontalHeader()->isHidden());
            QVERIFY(list->table().isColumnHidden(qt::ValuesModel::DeltaCol));
            QVERIFY(list->table().isColumnHidden(qt::ValuesModel::UnitCol));
        }
        if (c.name == QLatin1String("corr_speed_delta")) {
            // By hand: Corr Speed follows the faster wheel (FL); at 20 s of the 48 s lap u = 5/12, so 2000 m
            // / 48 s * (1 + 0.3 sin(5 pi / 6)) = 47.9167 m/s = 172.5 km/h. A missing channel shows empty
            // cells.
            QCOMPARE(m.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("172.5"));
            QCOMPARE(m.cellText(3, qt::ValuesModel::MainCol), QString());
        }
        checkText(repoRoot() / "tests" / "golden" / "tables" / "values_list" /
                      (c.name.toStdString() + ".json"),
                  tableJson(c.name, m, list->table()));
        for (const qreal dpr : {1.0, 2.0}) {
            checkPixels(c.name, dpr, list->renderToImage(dpr), list->renderToImage(dpr));
        }
        writeManifest();
    }
};

QTEST_MAIN(TstValuesListGoldens)
#include "tst_values_list_goldens.moc"
