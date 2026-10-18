// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Goldens of channel_report, time_report and track_position (AC-M3-02; 04 section 6.7; D-127), three cases
// each at DPR 1 and 2:
//   * pixel goldens tests/golden/images/<type>/<case>@<dpr>x.png: the report tables painted by
//   QWidget::render
//     (their Print to Clipboard, 04 section 7.2 item 9), the track map by OffscreenRenderer (the widget's
//     composite equals it, tst_track_position). A pixel differs when a channel differs by more than 8/255;
//     an image fails above 0.5 % differing pixels, but only where the PNGs are authoritative (D-127: Linux
//     with goldens.json "generatedOn": "linux", or ESSES_PIXEL_GOLDENS_STRICT=1); elsewhere each case is a
//     render smoke test (size, DPR tag, ink, two renders identical) and the difference is reported. The PNGs
//     of this change were rendered on Windows and are provisional ("generatedOn": "windows") until they are
//     replaced from the ubuntu qt-app artefact. Every render (and a diff on a mismatch) goes to
//     <build>/libs/widgets/artefacts/<type>/.
//   * exact goldens, checked on every OS: the report tables' contents
//   (tests/golden/tables/<type>/<case>.json:
//     headers, cell text, tints, italic and bold, from the ReportModel) and the track map's DrawList JSON
//     with FixedTextMetrics (tests/golden/drawlist/track_position/widget_<case>@<dpr>x.json: the layers the
//     widget's inputs give, from a hand-made rectangle and constant-speed laps, so no transcendental function
//     feeds a coordinate).
// ESSES_UPDATE_GOLDENS=1 rewrites the PNGs (and goldens.json, generatedOn = this platform), the table and the
// DrawList goldens.

#include "components/reports_track/rt_support.hpp"

#include <esses/base/cancel.hpp>
#include <esses/plotcore/text_metrics.hpp>
#include <esses/plotcore/track_geometry.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/time_report.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableView>
#include <QTest>
#include <functional>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::rtw;

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

QString imagesDir(const QString& type) {
    return pathOf(repoRoot() / "tests" / "golden" / "images" / type.toStdString());
}

QString artefactsDir(const QString& type) {
    const QString dir =
        pathOf(std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts" / type.toStdString());
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

// The pixel golden of one case: written when updating; otherwise compared (strict only where authoritative).
void checkPixels(const QString& type, const QString& name, qreal dpr, const QImage& render,
                 const QImage& again) {
    const QString stem = name + QStringLiteral("@%1x").arg(static_cast<int>(dpr));
    render.save(artefactsDir(type) + QLatin1Char('/') + stem + QStringLiteral(".png"));
    // The smoke part, everywhere: device size, DPR tag, ink, deterministic.
    QVERIFY(!render.isNull());
    QCOMPARE(render.devicePixelRatio(), dpr);
    QVERIFY(hasInk(render));
    QCOMPARE(compare(render, again).differing, 0);
    const QString png = imagesDir(type) + QLatin1Char('/') + stem + QStringLiteral(".png");
    if (updating()) {
        QDir().mkpath(imagesDir(type));
        QVERIFY(render.save(png));
        return;
    }
    const QImage golden(png);
    QVERIFY2(!golden.isNull(),
             qPrintable(QStringLiteral("missing golden %1 (ESSES_UPDATE_GOLDENS=1)").arg(png)));
    const QString generatedOn =
        QJsonDocument::fromJson(readFile(imagesDir(type) + QStringLiteral("/goldens.json")))
            .object()
            .value(QStringLiteral("generatedOn"))
            .toString();
    const bool strict = qEnvironmentVariableIntValue("ESSES_PIXEL_GOLDENS_STRICT") == 1 ||
                        (platformTag() == QLatin1String("linux") && generatedOn == QLatin1String("linux"));
    if (golden.size() != render.size()) {
        if (strict) {
            QFAIL(qPrintable(QStringLiteral("%1/%2: size %3x%4, golden %5x%6")
                                 .arg(type, stem)
                                 .arg(render.width())
                                 .arg(render.height())
                                 .arg(golden.width())
                                 .arg(golden.height())));
        }
        qInfo("%s/%s: size differs from the provisional golden (reported only)", qPrintable(type),
              qPrintable(stem));
        return;
    }
    const Diff d = compare(render, golden);
    const double pct = d.total > 0 ? 100.0 * d.differing / d.total : 0.0;
    if (d.differing > 0) {
        d.image.save(artefactsDir(type) + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
    }
    if (strict) {
        QVERIFY2(pct <= 0.5,
                 qPrintable(QStringLiteral("%1/%2: %3 % of pixels differ").arg(type, stem).arg(pct)));
    } else {
        qInfo("%s/%s: %.3f %% of pixels differ from the golden made on %s (reported only, D-127)",
              qPrintable(type), qPrintable(stem), pct, qPrintable(generatedOn));
    }
}

void writeManifest(const QString& type) {
    if (!updating()) {
        return;
    }
    QJsonObject m;
    m.insert(QStringLiteral("$comment"),
             QStringLiteral(
                 "Provenance of the PNGs in this directory (libs/widgets/tests/components/reports_track/"
                 "tst_reports_track_goldens.cpp). Pixel goldens are authoritative on ubuntu only "
                 "(D-127): replace them from the qt-app ubuntu artefact (<build>/libs/widgets/artefacts/"
                 "<type>) and set generatedOn to linux."));
    m.insert(QStringLiteral("generatedOn"), platformTag());
    m.insert(QStringLiteral("platform"), QStringLiteral("offscreen"));
    m.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
    m.insert(QStringLiteral("source"), QStringLiteral("rendered locally: provisional"));
    QVERIFY(writeFile(imagesDir(type) + QStringLiteral("/goldens.json"),
                      QJsonDocument(m).toJson(QJsonDocument::Indented)));
}

// The exact golden of a text document (tables, DrawLists).
void checkText(const std::filesystem::path& path, const QByteArray& actual) {
    const QString file = pathOf(path);
    if (updating()) {
        QVERIFY(writeFile(file, actual));
        return;
    }
    const QByteArray expected = readFile(file);
    if (expected != actual) {
        const QString out =
            pathOf(std::filesystem::path(ESSES_TEST_BINARY_DIR) /
                   (path.parent_path().filename().string() + "_" + path.filename().string() + ".actual"));
        writeFile(out, actual);
        QFAIL(qPrintable(QStringLiteral("golden %1 differs%2; actual written to %3")
                             .arg(file, expected.isEmpty() ? QStringLiteral(" (missing)") : QString(), out)));
    }
}

QString rgba(std::uint32_t c) {
    return QStringLiteral("#%1").arg(c, 8, 16, QLatin1Char('0'));
}

// The table golden: headers, then every cell's text and its styling roles.
QByteArray tableJson(const QString& name, const qt::ReportModel& m) {
    QJsonArray headers;
    for (int c = 0; c < m.columnCount(); ++c) {
        headers.append(m.headerData(c, Qt::Horizontal).toString());
    }
    QJsonArray rows;
    for (int r = 0; r < m.rowCount(); ++r) {
        QJsonArray row;
        for (int c = 0; c < m.columnCount(); ++c) {
            const QModelIndex i = m.index(r, c);
            QJsonObject cell{{QStringLiteral("t"), i.data().toString()}};
            const QVariant bg = i.data(qt::ReportModel::BackgroundRgbaRole);
            if (bg.isValid()) {
                cell.insert(QStringLiteral("bg"), rgba(bg.toUInt()));
            }
            if (i.data(qt::ReportModel::ItalicRole).toBool()) {
                cell.insert(QStringLiteral("i"), true);
            }
            if (i.data(qt::ReportModel::BoldRole).toBool()) {
                cell.insert(QStringLiteral("b"), true);
            }
            row.append(cell);
        }
        rows.append(row);
    }
    const QJsonObject o{
        {QStringLiteral("case"), name}, {QStringLiteral("headers"), headers}, {QStringLiteral("rows"), rows}};
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QJsonObject highlightOn() {
    return QJsonObject{
        {QStringLiteral("min"),
         QJsonObject{{QStringLiteral("on"), true}, {QStringLiteral("colour"), QStringLiteral("#2060ff")}}},
        {QStringLiteral("max"),
         QJsonObject{{QStringLiteral("on"), true}, {QStringLiteral("colour"), QStringLiteral("#ff3030")}}}};
}

QJsonArray stats(const QString& channel, const QStringList& names) {
    return QJsonArray{QJsonObject{{QStringLiteral("name"), channel},
                                  {QStringLiteral("stats"), QJsonArray::fromStringList(names)}}};
}

struct ReportCase {
    QString name;
    QSize size;
    std::function<void(World&, const std::shared_ptr<const model::DataSource>&)> select;
    QJsonObject body;
};

std::vector<ReportCase> channelCases() {
    QJsonObject rows{
        {QStringLiteral("report"),
         QJsonObject{{QStringLiteral("scope"), QStringLiteral("selectedRanges")},
                     {QStringLiteral("channels"),
                      stats(QStringLiteral("Speed"), {QStringLiteral("avg"), QStringLiteral("min"),
                                                      QStringLiteral("max"), QStringLiteral("stdDev")})}}},
        {QStringLiteral("layout"), QStringLiteral("channelsInRows")},
        {QStringLiteral("highlight"), highlightOn()}};
    QJsonArray two = stats(QStringLiteral("Speed"), {QStringLiteral("avg"), QStringLiteral("max")});
    two.append(stats(QStringLiteral("Load"), {QStringLiteral("max")}).at(0));
    QJsonObject columns{{QStringLiteral("report"),
                         QJsonObject{{QStringLiteral("scope"), QStringLiteral("selectedRanges")},
                                     {QStringLiteral("headers"), QJsonArray{QStringLiteral("Outings")}},
                                     {QStringLiteral("channels"), two}}},
                        {QStringLiteral("layout"), QStringLiteral("channelsInColumns")},
                        {QStringLiteral("highlight"), highlightOn()},
                        {QStringLiteral("deltaVsOverlay0"), true}};
    QJsonObject untrusted{
        {QStringLiteral("report"),
         QJsonObject{{QStringLiteral("scope"), QStringLiteral("selectedRanges")},
                     {QStringLiteral("channels"),
                      stats(QStringLiteral("Load"),
                            {QStringLiteral("start"), QStringLiteral("end"), QStringLiteral("change")})},
                     {QStringLiteral("options"), QJsonObject{{QStringLiteral("includeUntrusted"), true}}}}},
        {QStringLiteral("layout"), QStringLiteral("channelsInColumns")},
        {QStringLiteral("highlight"), highlightOn()}};
    const auto mainAnd = [](const char* overlay, bool zero) {
        return [overlay, zero](World& w, const std::shared_ptr<const model::DataSource>& src) {
            w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
            if (zero) {
                w.selection.toggleOverlay0(refOf(*src, kLaps, overlay));
            } else {
                w.selection.toggleOverlay(refOf(*src, kLaps, overlay));
            }
        };
    };
    return {ReportCase{QStringLiteral("rows_highlight"), QSize(420, 200), mainAnd("Lap 3", true), rows},
            ReportCase{QStringLiteral("columns_delta"), QSize(560, 120), mainAnd("Lap 3", true), columns},
            ReportCase{QStringLiteral("untrusted_included"), QSize(400, 120), mainAnd("Lap 2", false),
                       untrusted}};
}

QJsonObject timeBody(bool untrusted, const QString& format, bool summaries, const QString& scope) {
    return QJsonObject{
        {QStringLiteral("scope"), scope},
        {QStringLiteral("rowGroup"), QStringLiteral("Outings:Laps:Track Sections:Default")},
        {QStringLiteral("options"), QJsonObject{{QStringLiteral("includeUntrusted"), untrusted}}},
        {QStringLiteral("summaries"), QJsonObject{{QStringLiteral("eclectic"), summaries},
                                                  {QStringLiteral("rollingMin"), summaries},
                                                  {QStringLiteral("total"), summaries}}},
        {QStringLiteral("bands"),
         QJsonArray{QJsonObject{{QStringLiteral("pct"), 10.0},
                                {QStringLiteral("colour"), QStringLiteral("#30c030")},
                                {QStringLiteral("visible"), true}},
                    QJsonObject{{QStringLiteral("pct"), 30.0},
                                {QStringLiteral("colour"), QStringLiteral("#c0c030")},
                                {QStringLiteral("visible"), true}}}},
        {QStringLiteral("timeFormat"), format}};
}

std::vector<ReportCase> timeCases() {
    const auto main = [](World& w, const std::shared_ptr<const model::DataSource>& src) {
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
    };
    return {
        ReportCase{QStringLiteral("bands_summaries"), QSize(420, 210), main,
                   timeBody(false, QStringLiteral("s.mmm"), true, QStringLiteral("selectedOutings"))},
        ReportCase{QStringLiteral("untrusted_italic"), QSize(420, 240), main,
                   timeBody(true, QStringLiteral("s.mmm"), true, QStringLiteral("selectedOutings"))},
        ReportCase{QStringLiteral("minutes_plain"), QSize(400, 110), main,
                   timeBody(false, QStringLiteral("m:ss.mmm"), false, QStringLiteral("selectedOutings"))}};
}

// ---- track_position cases

struct TrackCase {
    QString name;
    QJsonObject body;
    plot::Domain domain = plot::Domain::Time;
    Micros cursor = 0;
    std::optional<plot::XSpan> zoom;
    bool overlay = false;
};

std::vector<TrackCase> trackCases() {
    const QJsonObject base{{QStringLiteral("showZoomSpan"), true},
                           {QStringLiteral("showSectionLabels"), false},
                           {QStringLiteral("showSectionMarks"), true},
                           {QStringLiteral("showSectionColours"), true}};
    QJsonObject manual = base;
    manual.insert(QStringLiteral("showSectionLabels"), true);
    manual.insert(QStringLiteral("scale"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("manual")},
                                                       {QStringLiteral("zoom"), 1.25},
                                                       {QStringLiteral("panX"), 60},
                                                       {QStringLiteral("panY"), 20},
                                                       {QStringLiteral("rotation"), 0}});
    manual.insert(QStringLiteral("sizes"), QJsonObject{{QStringLiteral("cursor"), 8},
                                                       {QStringLiteral("detail"), 3},
                                                       {QStringLiteral("material"), 2}});
    QJsonObject plain = base;
    plain.insert(QStringLiteral("showSectionMarks"), false);
    plain.insert(QStringLiteral("showSectionColours"), false);
    return {TrackCase{QStringLiteral("fit_main_dot"), base, plot::Domain::Time, 6 * kSec,
                      plot::XSpan{0.0, 10.0 * kSec}, false},
            TrackCase{QStringLiteral("manual_overlay_span"), manual, plot::Domain::Time, 20 * kSec,
                      plot::XSpan{10.0 * kSec, 30.0 * kSec}, true},
            TrackCase{QStringLiteral("distance_plain"), plain, plot::Domain::Distance, 30 * kSec,
                      plot::XSpan{500.0, 900.0}, true}};
}

} // namespace

class TstReportsTrackGoldens : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(registerGoldenFonts() > 0);
        registerBuiltinComponents();
    }

    void channelReport_data() {
        QTest::addColumn<int>("index");
        const auto cases = channelCases();
        for (int i = 0; i < static_cast<int>(cases.size()); ++i) {
            QTest::newRow(qPrintable(cases[static_cast<std::size_t>(i)].name)) << i;
        }
    }
    void channelReport() {
        QFETCH(int, index);
        const ReportCase rc = channelCases()[static_cast<std::size_t>(index)];
        World w;
        const auto src = threeLaps(w, "golden-src");
        rc.select(w, src);
        ChannelReport report(
            componentSpec(ChannelReport::kType, rc.body, "6d1f7c0e-6c0a-4f61-9c55-2b0e8d1a0c01"), *w.context);
        report.resize(rc.size);
        report.show();
        QVERIFY(settle(report.model(), 0));
        QTest::qWait(1);
        checkText(repoRoot() / "tests" / "golden" / "tables" / "channel_report" /
                      (rc.name.toStdString() + ".json"),
                  tableJson(rc.name, report.model()));
        for (const qreal dpr : {1.0, 2.0}) {
            checkPixels(QStringLiteral("channel_report"), rc.name, dpr, report.renderToImage(dpr),
                        report.renderToImage(dpr));
        }
        writeManifest(QStringLiteral("channel_report"));
    }

    void timeReport_data() {
        QTest::addColumn<int>("index");
        const auto cases = timeCases();
        for (int i = 0; i < static_cast<int>(cases.size()); ++i) {
            QTest::newRow(qPrintable(cases[static_cast<std::size_t>(i)].name)) << i;
        }
    }
    void timeReport() {
        QFETCH(int, index);
        const ReportCase rc = timeCases()[static_cast<std::size_t>(index)];
        World w;
        const auto src = threeLaps(w, "golden-src");
        rc.select(w, src);
        TimeReport report(componentSpec(TimeReport::kType, rc.body, "6d1f7c0e-6c0a-4f61-9c55-2b0e8d1a0c02"),
                          *w.context);
        report.resize(rc.size);
        report.show();
        QVERIFY(settle(report.model(), 0));
        checkText(repoRoot() / "tests" / "golden" / "tables" / "time_report" /
                      (rc.name.toStdString() + ".json"),
                  tableJson(rc.name, report.model()));
        for (const qreal dpr : {1.0, 2.0}) {
            checkPixels(QStringLiteral("time_report"), rc.name, dpr, report.renderToImage(dpr),
                        report.renderToImage(dpr));
        }
        writeManifest(QStringLiteral("time_report"));
    }

    void trackPosition_data() {
        QTest::addColumn<int>("index");
        const auto cases = trackCases();
        for (int i = 0; i < static_cast<int>(cases.size()); ++i) {
            QTest::newRow(qPrintable(cases[static_cast<std::size_t>(i)].name)) << i;
        }
    }
    void trackPosition() {
        QFETCH(int, index);
        const TrackCase tc = trackCases()[static_cast<std::size_t>(index)];
        World w(tc.domain == plot::Domain::Distance ? "distance" : "time");
        qt::TrackMapService maps(nullptr);
        w.render.setTrackMaps(&maps);
        QVERIFY(maps.setMap("Golden Rect", std::make_shared<const analysis::TrackMap>(rectMap("Golden Rect")))
                    .ok());
        const auto src = publish(w, linearSource("golden-track", "Golden Rect", {40.0, 50.0, 45.0, 30.0},
                                                 2000.0, *shippedUnits(), w.rt.cache()));
        w.selection.setMain(lapRef(*src, 1));
        if (tc.overlay) {
            w.selection.toggleOverlay0(lapRef(*src, 2));
        }
        QVERIFY(QTest::qWaitFor([&] { return w.distanceReady(src); }, 20000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto c = w.distance.chain(lapRef(*src, 2), Priority::Interactive);
                return c != nullptr && c->complete();
            },
            20000));
        w.view->setCursor(tc.cursor, qt::CursorSource::Programmatic);
        if (tc.zoom) {
            w.view->zoomTo(*tc.zoom);
        }
        project::ComponentSpec spec =
            componentSpec("track_position", tc.body, "6d1f7c0e-6c0a-4f61-9c55-2b0e8d1a0c03");
        const QSize logical(360, 260);
        // Exact: the DrawList JSON with FixedTextMetrics.
        for (const int dpr : {1, 2}) {
            const plot::LayoutContext layout{logical.width() * dpr, logical.height() * dpr,
                                             static_cast<float>(dpr)};
            const plot::ComponentBuildInputs in =
                w.render.prepareInputs(spec, w.view->state(), w.selection.snapshot(), layout, w.scheme,
                                       std::make_shared<const plot::FixedTextMetrics>());
            QVERIFY(in.trackMap != nullptr);
            Result<plot::ComponentBuild> built = plot::TrackGeometry::buildStatic(in, CancelToken{});
            QVERIFY(built.ok());
            const std::string name = "widget_" + tc.name.toStdString();
            std::string json = "{\"case\":\"" + name + "\",\"dpr\":" + std::to_string(dpr) + ",\n";
            json += "\"static\":" + built->list.toJson();
            json += ",\"dynamic\":" + plot::TrackGeometry::buildDynamic(in).toJson();
            json += ",\"chrome\":" + plot::TrackGeometry::buildChrome(in, false).toJson();
            json += "}\n";
            checkText(repoRoot() / "tests" / "golden" / "drawlist" / "track_position" /
                          (name + "@" + std::to_string(dpr) + "x.json"),
                      QByteArray::fromStdString(json));
        }
        // Pixels: OffscreenRenderer (the widget's composite equals it, tst_track_position).
        qt::OffscreenRenderer renderer(std::make_shared<const qt::SchemeFonts>(*w.scheme));
        const plot::ComponentBuildInputs in =
            w.render.prepareInputs(spec, w.view->state(), w.selection.snapshot(), {}, w.scheme, nullptr);
        for (const qreal dpr : {1.0, 2.0}) {
            checkPixels(QStringLiteral("track_position"), tc.name, dpr,
                        renderer.renderComponent(in, logical, dpr),
                        renderer.renderComponent(in, logical, dpr));
        }
        writeManifest(QStringLiteral("track_position"));
    }
};

QTEST_MAIN(TstReportsTrackGoldens)
#include "tst_reports_track_goldens.moc"
