// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// values_list at DPR 1.5 (04 section 6.6 item 5: goldens of time_distance and values_list at DPR 1.5 besides
// 1 and 2; M4 hidpi package, D-209, AC-M4-07). The time_distance goldens at 1.5 are DrawList JSON
// (tests/golden/images/tst_dpr_goldens.cpp); values_list is a table painted by Qt, so its 1.5 goldens are the
// three cases of libs/widgets/tests/components/tst_values_list_goldens.cpp rendered as Print to Clipboard
// does (renderToImage at DPR 1.5):
//   * exact, on every OS: what the table shows does not depend on the DPR: its headers, hidden columns,
//     heading and every cell equal the committed table goldens of those cases
//     (tests/golden/tables/values_list/<case>.json, computed by hand there), and the hand-worked Throttle
//     cells of main_reference hold at 1.5 too;
//   * the image is lround(logical x 1.5) device pixels, tagged 1.5, has ink, and two renders are identical;
//   * pixel goldens tests/golden/images/dpr_sweep/values_list/<case>@1.5x.png, compared as the values_list
//     goldens are (a pixel differs above 8/255 in a channel, an image fails above 0.5 %), but only where they
//     are authoritative (D-127: Linux with goldens.json "generatedOn": "linux", or
//     ESSES_PIXEL_GOLDENS_STRICT=1); elsewhere the difference is reported. The PNGs of this change were
//     rendered on Windows and are provisional until replaced from the ubuntu qt-app artefact
//     (<build>/libs/widgets/artefacts/dpr_sweep/ values_list/). ESSES_UPDATE_GOLDENS=1 rewrites the PNGs and
//     goldens.json.

#include "components/component_test_support.hpp"

#include <esses/base/read_file.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/widgets/components/values_list.hpp>

#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableView>
#include <QTest>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;
constexpr qreal kDpr = 1.5;
constexpr int kTimeoutMs = 30'000;

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

std::filesystem::path imagesDir() {
    return repoRoot() / "tests" / "golden" / "images" / "dpr_sweep" / "values_list";
}

QString artefactsDir() {
    const QString dir =
        pathOf(std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts" / "dpr_sweep" / "values_list");
    QDir().mkpath(dir);
    return dir;
}

QByteArray readBytes(const std::filesystem::path& path) {
    Result<std::string> bytes = readWholeFile(path);
    if (!bytes) {
        return {};
    }
    return QByteArray::fromStdString(*bytes);
}

bool writeBytes(const std::filesystem::path& path, const QByteArray& bytes) {
    QDir().mkpath(pathOf(path.parent_path()));
    QFile f(pathOf(path));
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

// The cases of tst_values_list_goldens.cpp (same bodies, sizes, laps and cursor times).
struct Case {
    QString name;
    QSize size;
    QJsonObject body;
    std::size_t main = 1;
    std::optional<std::size_t> reference;
    Micros cursor = 0;
};

QJsonObject row(const char* channel, int dps, const char* unit = nullptr) {
    QJsonObject o{{QStringLiteral("channel"), QString::fromLatin1(channel)}, {QStringLiteral("dps"), dps}};
    if (unit != nullptr) {
        o.insert(QStringLiteral("displayUnit"), QString::fromLatin1(unit));
    }
    return o;
}

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
    const QJsonObject fixed{
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

project::ComponentSpec withBody(project::ComponentSpec spec, const QJsonObject& body) {
    Result<model::OpaqueJson> json =
        model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    spec.body = std::move(json).value();
    return spec;
}

// Every displayed cell of the rows that have data is final (no pending U+2026).
bool settledCells(const qt::ValuesModel& m, bool reference) {
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

bool hasInk(const QImage& image) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb first = img.pixel(0, 0);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (img.pixel(x, y) != first) {
                return true;
            }
        }
    }
    return false;
}

// Pixels differing by more than 8/255 in a channel (the golden rule of 04 section 6.7 item 4); a red mark
// in `diff` for each.
int differing(const QImage& a, const QImage& b, QImage* diff) {
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    if (diff != nullptr) {
        *diff = QImage(x.size(), QImage::Format_ARGB32);
        diff->fill(Qt::white);
    }
    int n = 0;
    for (int r = 0; r < x.height(); ++r) {
        for (int c = 0; c < x.width(); ++c) {
            const QRgb p = x.pixel(c, r);
            const QRgb q = y.pixel(c, r);
            if (std::abs(qRed(p) - qRed(q)) > 8 || std::abs(qGreen(p) - qGreen(q)) > 8 ||
                std::abs(qBlue(p) - qBlue(q)) > 8 || std::abs(qAlpha(p) - qAlpha(q)) > 8) {
                ++n;
                if (diff != nullptr) {
                    diff->setPixel(c, r, qRgb(255, 0, 0));
                }
            }
        }
    }
    return n;
}

} // namespace

class TstDprValuesList : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(registerGoldenFonts() > 0);
        registerBuiltinComponents();
    }

    void valuesListAt1_5_data() {
        QTest::addColumn<int>("index");
        const std::vector<Case> all = cases();
        for (std::size_t i = 0; i < all.size(); ++i) {
            QTest::newRow(qPrintable(all[i].name)) << static_cast<int>(i);
        }
    }

    void valuesListAt1_5() {
        QFETCH(int, index);
        const Case c = cases().at(static_cast<std::size_t>(index));
        World w;
        const auto src = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*src, c.main));
        if (c.reference) {
            w.selection.toggleOverlay(lapRef(*src, *c.reference));
        }
        if (c.name == QLatin1String("corr_speed_delta")) {
            QVERIFY(QTest::qWaitFor([&] { return w.distanceReady(src); }, kTimeoutMs));
        }
        w.view->setCursor(c.cursor, qt::CursorSource::Programmatic);
        ValuesList list(withBody(valuesListSpec({}, {}, 0, "5c3f1a52-8d7e-4b8e-9a51-0c6d2f4e7a10"), c.body),
                        *w.context);
        list.resize(c.size);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        const qt::ValuesModel& m = list.model();
        QTRY_VERIFY_WITH_TIMEOUT(settledCells(m, c.reference.has_value()), kTimeoutMs);

        // What the table shows equals the committed (DPR-free) table golden of the case.
        const QByteArray tableBytes = readBytes(repoRoot() / "tests" / "golden" / "tables" / "values_list" /
                                                (c.name.toStdString() + ".json"));
        const QJsonObject golden = QJsonDocument::fromJson(tableBytes).object();
        QVERIFY2(!golden.isEmpty(), "the table golden of the case is missing");
        const QJsonArray headers = golden.value(QStringLiteral("headers")).toArray();
        QCOMPARE(m.columnCount(), static_cast<int>(headers.size()));
        for (int col = 0; col < m.columnCount(); ++col) {
            QCOMPARE(m.headerData(col, Qt::Horizontal).toString(), headers.at(col).toString());
        }
        QJsonArray hidden;
        for (int col = 0; col < m.columnCount(); ++col) {
            if (list.table().isColumnHidden(col)) {
                hidden.append(col);
            }
        }
        QCOMPARE(hidden, golden.value(QStringLiteral("hiddenColumns")).toArray());
        QCOMPARE(list.table().horizontalHeader()->isVisible(),
                 golden.value(QStringLiteral("showHeading")).toBool());
        const QJsonArray rows = golden.value(QStringLiteral("rows")).toArray();
        QCOMPARE(m.rowCount(), static_cast<int>(rows.size()));
        for (int r = 0; r < m.rowCount(); ++r) {
            const QJsonArray cells = rows.at(r).toArray();
            for (int col = 0; col < m.columnCount(); ++col) {
                QCOMPARE(m.cellText(r, col), cells.at(col).toString());
            }
        }
        if (c.name == QLatin1String("main_reference")) {
            // By hand (as in tst_values_list_goldens): Throttle = 50 + 50 cos(2 pi u) at u = 10/50 (Main) and
            // 10/48 (the overlay).
            const auto throttle = [](double u) { return 50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * u); };
            QCOMPARE(m.cellText(0, qt::ValuesModel::MainCol), QString::number(throttle(10.0 / 50.0), 'f', 2));
            QCOMPARE(m.cellText(0, qt::ValuesModel::ReferenceCol),
                     QString::number(throttle(10.0 / 48.0), 'f', 2));
        }

        // The render at DPR 1.5.
        const QImage render = list.renderToImage(kDpr);
        const QString stem = c.name + QStringLiteral("@1.5x");
        render.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".png"));
        QCOMPARE(render.size(), QSize(static_cast<int>(std::lround(c.size.width() * kDpr)),
                                      static_cast<int>(std::lround(c.size.height() * kDpr))));
        QCOMPARE(render.devicePixelRatio(), kDpr);
        QVERIFY(hasInk(render));
        QCOMPARE(differing(render, list.renderToImage(kDpr), nullptr), 0);

        const std::filesystem::path png = imagesDir() / (stem.toStdString() + ".png");
        if (updating()) {
            QDir().mkpath(pathOf(imagesDir()));
            QVERIFY(render.save(pathOf(png)));
            QJsonObject manifest;
            manifest.insert(
                QStringLiteral("$comment"),
                QStringLiteral("Provenance of the PNGs in this directory (libs/widgets/tests/m4_hidpi/"
                               "tst_dpr_values_list.cpp). Pixel goldens are authoritative on ubuntu "
                               "only (D-127): replace them from the qt-app ubuntu artefact (<build>/"
                               "libs/widgets/artefacts/dpr_sweep/values_list) and set generatedOn to "
                               "linux."));
            manifest.insert(QStringLiteral("generatedOn"), platformTag());
            manifest.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
            manifest.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
            manifest.insert(QStringLiteral("source"), QStringLiteral("rendered locally: provisional"));
            QVERIFY(writeBytes(imagesDir() / "goldens.json",
                               QJsonDocument(manifest).toJson(QJsonDocument::Indented)));
            return;
        }
        const QImage expected = QImage::fromData(readBytes(png), "PNG");
        QVERIFY2(!expected.isNull(),
                 qPrintable(QStringLiteral("missing golden %1 (ESSES_UPDATE_GOLDENS=1)").arg(pathOf(png))));
        const QString generatedOn = QJsonDocument::fromJson(readBytes(imagesDir() / "goldens.json"))
                                        .object()
                                        .value(QStringLiteral("generatedOn"))
                                        .toString();
        const bool strict =
            qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
            (platformTag() == QLatin1String("linux") && generatedOn == QLatin1String("linux"));
        if (expected.size() != render.size()) {
            if (strict) {
                QFAIL(qPrintable(QStringLiteral("values_list/%1: size %2x%3, golden %4x%5")
                                     .arg(stem)
                                     .arg(render.width())
                                     .arg(render.height())
                                     .arg(expected.width())
                                     .arg(expected.height())));
            }
            qInfo("values_list/%s: size differs from the provisional golden (reported only)",
                  qPrintable(stem));
            return;
        }
        QImage diff;
        const int n = differing(render, expected, &diff);
        const double pct = 100.0 * n / (static_cast<double>(render.width()) * render.height());
        if (n > 0) {
            diff.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
        }
        if (strict) {
            QVERIFY2(pct <= 0.5,
                     qPrintable(QStringLiteral("values_list/%1: %2 % of pixels differ").arg(stem).arg(pct)));
        } else {
            qInfo(
                "values_list/%s: %.3f %% of pixels differ from the golden made on %s (reported only, D-127)",
                qPrintable(stem), pct, qPrintable(generatedOn));
        }
    }
};

QTEST_MAIN(TstDprValuesList)
#include "tst_dpr_values_list.moc"
