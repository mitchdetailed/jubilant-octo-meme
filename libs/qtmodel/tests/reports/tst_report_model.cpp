// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportModel (04 sections 12-13; AC-M3-03 display part; D-199): the channel report's scope, headings,
// statistics, min/max highlight, delta vs Overlay 0, both layouts, untrusted rows, zoom link, pending maths
// cells and export quoting; the time report's section times, summaries, bands, time formats and missing
// sections. Every expected cell is computed by hand from the fixture's closed-form channels
// (report_track_fixtures.hpp: Speed sample i = i at 10 Hz, so a lap [a, b) s averages 10 (a + b) / 2 - 0.5).

#include "../qtmodel_test_support.hpp"
#include "report_track_fixtures.hpp"

#include <esses/analysis/report_spec.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/report_model.hpp>

#include <QSignalSpy>
#include <QTest>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::rttest;

namespace {

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

struct World {
    AppRuntime rt{runtimeOptions()};
    MathsService maths{rt.pool(), rt.dispatcher(), test::shippedUnits()};
    DataSelectionModel selection;

    std::shared_ptr<const model::DataSource> publish(std::shared_ptr<const model::DataSource> src) {
        maths.addSource(src);
        selection.sourcePublished(src);
        return src;
    }
    // Laps 1-3 of 10 s each, Lap 2 untrusted; sections Turn 1 / Straight 1 per lap.
    std::shared_ptr<const model::DataSource> threeLaps(const std::string& tag = "rm-a",
                                                       std::vector<model::Detail> details = {}) {
        const std::vector<RangeSpec> laps{{"Lap 1", 0, 10}, {"Lap 2", 10, 20, false}, {"Lap 3", 20, 30}};
        const std::vector<RangeSpec> sections{{"Turn 1", 0, 3},     {"Straight 1", 3, 10},
                                              {"Turn 1", 10, 12},   {"Straight 1", 12, 20},
                                              {"Turn 1", 20, 22.5}, {"Straight 1", 22.5, 30}};
        return publish(
            reportSource(tag, 30.0, laps, sections, std::move(details), *test::shippedUnits(), rt.cache()));
    }
};

bool settle(const ReportModel& m, int before) {
    return QTest::qWaitFor([&] { return !m.busy() && m.deliveries() > before; }, 20000);
}

QString cell(const ReportModel& m, int row, int column) {
    return m.data(m.index(row, column)).toString();
}

QString header(const ReportModel& m, int column) {
    return m.headerData(column, Qt::Horizontal).toString();
}

analysis::ReportSpec speedSpec(std::vector<analysis::ReportStat> stats) {
    analysis::ReportSpec spec;
    spec.channels.push_back(analysis::ReportChannel{"Speed", std::move(stats)});
    return spec;
}

} // namespace

class TstReportModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void channelsInRowsWithTheMinMaxHighlight() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay0(refOf(*src, kLaps, "Lap 3"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        m.setHighlight(ReportModel::Highlight{0x2060FFFFU, 0xFF3030FFU});
        QSignalSpy computed(&m, &ReportModel::computed);
        m.setChannelReport(speedSpec({analysis::ReportStat::Avg, analysis::ReportStat::Min,
                                      analysis::ReportStat::Max, analysis::ReportStat::StdDev}),
                           QStringLiteral("cr-1"));
        QVERIFY(settle(m, 0));
        QCOMPARE(computed.count(), 1);
        QCOMPARE(m.lastError(), QString());
        // channelsInRows: a row per statistic, a column per heading (Main first, Overlay 0 marked).
        QCOMPARE(m.rowCount(), 4);
        QCOMPARE(m.columnCount(), 3);
        QCOMPARE(header(m, 0), QStringLiteral("Channel"));
        QCOMPARE(header(m, 1), QStringLiteral("Lap 1"));
        QCOMPARE(header(m, 2), QStringLiteral("Lap 3 (Overlay 0)"));
        QCOMPARE(cell(m, 0, 0), QStringLiteral("Speed avg [m/s]"));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("49.50"));  // samples 0..99
        QCOMPARE(cell(m, 0, 2), QStringLiteral("249.50")); // samples 200..299
        QCOMPARE(cell(m, 1, 1), QStringLiteral("0.00"));
        QCOMPARE(cell(m, 2, 2), QStringLiteral("299.00"));
        // Population standard deviation of 100 consecutive integers: sqrt((100^2 - 1) / 12) = 28.866.
        QCOMPARE(cell(m, 3, 1), QStringLiteral("28.87"));
        QCOMPARE(cell(m, 3, 2), QStringLiteral("28.87"));
        // The highlight: Lap 1 holds every column minimum, Lap 3 every maximum; equal values are not marked.
        QCOMPARE(m.data(m.index(0, 1), ReportModel::BackgroundRgbaRole).toUInt(), 0x2060FFFFU);
        QCOMPARE(m.data(m.index(0, 2), ReportModel::BackgroundRgbaRole).toUInt(), 0xFF3030FFU);
        QVERIFY(!m.data(m.index(3, 1), ReportModel::BackgroundRgbaRole).isValid());
        QVERIFY(!m.data(m.index(0, 0), ReportModel::BackgroundRgbaRole).isValid());
        // The highlight off: no background.
        m.setHighlight({});
        QVERIFY(!m.data(m.index(0, 1), ReportModel::BackgroundRgbaRole).isValid());
        QCOMPARE(m.pendingCells(), 0);
    }

    void channelsInColumnsAndTheDeltaVsOverlay0() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay0(refOf(*src, kLaps, "Lap 3"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::ReportSpec spec = speedSpec({analysis::ReportStat::Avg, analysis::ReportStat::Max});
        spec.layout = analysis::ReportLayout::ChannelsInColumns;
        spec.deltaVsOverlay0 = true;
        spec.headers = {"Outings", "Outings:Laps"};
        m.setChannelReport(spec, QStringLiteral("cr-2"));
        QVERIFY(settle(m, 0));
        // A row per heading; columns: label, avg, avg delta, max, max delta.
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.columnCount(), 5);
        QCOMPARE(header(m, 0), QStringLiteral("Outings / Laps"));
        QCOMPARE(header(m, 1), QStringLiteral("Speed avg [m/s]"));
        QVERIFY(header(m, 2).startsWith(QStringLiteral("Speed avg ")));
        QVERIFY(header(m, 2).contains(QStringLiteral("vs Overlay 0")));
        QCOMPARE(cell(m, 0, 0), QStringLiteral("test.ld / Lap 1"));
        QCOMPARE(cell(m, 1, 0), QStringLiteral("test.ld / Lap 3 (Overlay 0)"));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("49.50"));
        // Main - Overlay 0 is paired by heading path: the paths differ ("Lap 1" vs "Lap 3"), so it is blank.
        QCOMPARE(cell(m, 0, 2), QString());
        QCOMPARE(cell(m, 1, 2), QString()); // never on the Overlay 0 row

        // Headings that pair (the outing level only): Main - Overlay 0 = 49.5 - 249.5 = -200.
        spec.headers = {"Outings"};
        m.setChannelReport(spec, QStringLiteral("cr-2"));
        QVERIFY(settle(m, 1));
        // Both ranges lie in one outing: one Main row and one Overlay 0 row under the same name.
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(cell(m, 0, 0), QStringLiteral("test.ld"));
        QCOMPARE(cell(m, 0, 2), QStringLiteral("-200.00"));
        QCOMPARE(cell(m, 0, 4), QStringLiteral("-200.00"));
        QCOMPARE(m.data(m.index(0, 2), ReportModel::CellStateRole).toInt(),
                 static_cast<int>(analysis::ReportCellState::Ok));

        // Overlays hidden (F4): the delta columns are blank (04 section 12 item 5).
        w.selection.toggleOverlaysHidden();
        m.refresh();
        QVERIFY(settle(m, 2));
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(cell(m, 0, 2), QString());
    }

    void untrustedRowsAreExcludedUnlessIncluded() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay(refOf(*src, kLaps, "Lap 2")); // untrusted
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::ReportSpec spec = speedSpec({analysis::ReportStat::Avg});
        spec.layout = analysis::ReportLayout::ChannelsInColumns;
        m.setChannelReport(spec, QStringLiteral("cr-3"));
        QVERIFY(settle(m, 0));
        QCOMPARE(m.rowCount(), 1); // Lap 2 is untrusted: excluded
        QCOMPARE(cell(m, 0, 0), QStringLiteral("Lap 1"));

        spec.options.includeUntrusted = true;
        m.setChannelReport(spec, QStringLiteral("cr-3"));
        QVERIFY(settle(m, 1));
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(cell(m, 1, 0), QStringLiteral("Lap 2 (Overlay 1)"));
        QCOMPARE(cell(m, 1, 1), QStringLiteral("149.50"));
        QVERIFY(m.data(m.index(1, 0), ReportModel::ItalicRole).toBool()); // untrusted rows are italic
        QVERIFY(m.data(m.index(1, 1), ReportModel::ItalicRole).toBool());
        QVERIFY(!m.data(m.index(0, 1), ReportModel::ItalicRole).toBool());

        // excludeOverlays narrows the policy to Main.
        spec.options.excludeOverlays = true;
        m.setChannelReport(spec, QStringLiteral("cr-3"));
        QVERIFY(settle(m, 2));
        QCOMPARE(m.rowCount(), 1);
    }

    void scopesSelectedOutingsAndAllOutings() {
        World w;
        const auto a =
            w.threeLaps("rm-a", {textDetail("Log Date", "2026-10-01"), textDetail("Log Time", "10:00:00")});
        const auto b =
            w.publish(reportSource("rm-b", 20.0, {{"Lap 1", 0, 10}, {"Lap 2", 10, 20}}, {},
                                   {textDetail("Log Date", "2026-10-01"), textDetail("Log Time", "11:00:00")},
                                   *test::shippedUnits(), w.rt.cache(), 10, "second.ld"));
        w.selection.setMain(refOf(*a, kLaps, "Lap 3"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::ReportSpec spec = speedSpec({analysis::ReportStat::Max});
        spec.layout = analysis::ReportLayout::ChannelsInColumns;
        spec.headers = {"Outings"};
        spec.scope = analysis::ReportScope::SelectedOutings;
        m.setChannelReport(spec, QStringLiteral("cr-4"));
        QVERIFY(settle(m, 0));
        QCOMPARE(m.rowCount(), 1); // the outing around Main
        QCOMPARE(cell(m, 0, 0), QStringLiteral("test.ld"));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("299.00"));

        spec.scope = analysis::ReportScope::AllOutings;
        m.setChannelReport(spec, QStringLiteral("cr-4"));
        QVERIFY(settle(m, 1));
        QCOMPARE(m.rowCount(), 2); // every outing of every open source, in Log Date / Log Time order
        QCOMPARE(cell(m, 0, 0), QStringLiteral("test.ld"));
        QCOMPARE(cell(m, 1, 0), QStringLiteral("second.ld"));
        QCOMPARE(cell(m, 1, 1), QStringLiteral("199.00"));
        static_cast<void>(b);
    }

    void zoomLinkedClipsEveryRangeThroughItsMapper() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay0(refOf(*src, kLaps, "Lap 3"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::ReportSpec spec = speedSpec({analysis::ReportStat::Avg, analysis::ReportStat::Max});
        spec.options.zoomLinked = true;
        plot::ViewState view;
        view.zoom = plot::XSpan{0.0, 5.0e6}; // the first 5 s of each range
        m.setZoom(view);
        m.setChannelReport(spec, QStringLiteral("cr-5"));
        QVERIFY(settle(m, 0));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("24.50"));  // samples 0..49
        QCOMPARE(cell(m, 0, 2), QStringLiteral("224.50")); // samples 200..249
        QCOMPARE(cell(m, 1, 1), QStringLiteral("49.00"));
        // Not linked: the whole ranges again.
        m.setZoom(std::nullopt);
        m.refresh();
        QVERIFY(settle(m, 1));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("49.50"));
    }

    void pendingMathsCellsFillInOnRefresh() {
        World w;
        const auto src = w.publish(test::makeSource(test::SyntheticOptions{}, w.rt.cache()));
        w.selection.setMain(test::rangeRef(*src, 1));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        // The widget refreshes on Data; here the test does.
        connect(&w.maths, &MathsService::channelReady, &m, &ReportModel::refresh);
        analysis::ReportSpec spec;
        spec.channels.push_back(analysis::ReportChannel{"Corr Speed", {analysis::ReportStat::Max}});
        spec.channels.push_back(analysis::ReportChannel{"Throttle", {analysis::ReportStat::Max}});
        spec.channels.push_back(analysis::ReportChannel{"No Such Channel", {analysis::ReportStat::Max}});
        m.setChannelReport(spec, QStringLiteral("cr-6"));
        QVERIFY(settle(m, 0));
        QVERIFY(QTest::qWaitFor([&] { return !m.busy() && m.pendingCells() == 0; }, 20000));
        QCOMPARE(m.rowCount(), 3);
        bool ok = false;
        const double corrMax = cell(m, 0, 1).toDouble(&ok);
        QVERIFY(ok);
        // The synthetic lap's wheel speed peaks at 1.3 x the mean (2000 m / 50 s = 40 m/s): 52 m/s.
        QVERIFY(std::abs(corrMax - 52.0) < 0.5);
        QCOMPARE(cell(m, 1, 1), QStringLiteral("100.00")); // Throttle peaks at 100 %
        QCOMPARE(cell(m, 2, 1), QStringLiteral("(missing)"));
        QVERIFY(m.data(m.index(2, 1), ReportModel::ItalicRole).toBool());
        QCOMPARE(m.data(m.index(2, 1), ReportModel::CellStateRole).toInt(),
                 static_cast<int>(analysis::ReportCellState::Missing));
    }

    void exportQuotesFieldsLikeTheHelpSays() {
        QCOMPARE(ReportModel::quoteField(QStringLiteral("plain"), QLatin1Char(',')), QStringLiteral("plain"));
        QCOMPARE(ReportModel::quoteField(QStringLiteral("a,b"), QLatin1Char(',')), QStringLiteral("\"a,b\""));
        QCOMPARE(ReportModel::quoteField(QStringLiteral("50%"), QLatin1Char('\t')),
                 QStringLiteral("\"50%\""));
        QCOMPARE(ReportModel::quoteField(QStringLiteral("say \"hi\""), QLatin1Char('\t')),
                 QStringLiteral("\"say \"\"hi\"\"\""));
        QCOMPARE(ReportModel::quoteField(QStringLiteral("two\nlines"), QLatin1Char('\t')),
                 QStringLiteral("\"two\nlines\""));
        QCOMPARE(ReportModel::quoteField(QStringLiteral("a\tb"), QLatin1Char('\t')),
                 QStringLiteral("\"a\tb\""));

        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::ReportSpec spec;
        spec.channels.push_back(analysis::ReportChannel{"Load", {analysis::ReportStat::Avg}});
        m.setChannelReport(spec, QStringLiteral("cr-7"));
        QVERIFY(settle(m, 0));
        // Load is in %, so the unit makes the label a quoted field.
        QCOMPARE(m.toTsv(), QStringLiteral("Channel\tLap 1\n\"Load avg [%]\"\t99.0\n"));
        QCOMPARE(m.toCsv(), QStringLiteral("Channel,Lap 1\n\"Load avg [%]\",99.0\n"));
    }

    void timeReportSectionsSummariesAndBands() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::TimeReportSpec spec; // selectedOutings, Default sections, untrusted excluded
        spec.bands = {analysis::TimeReportBand{10.0, 0x30C030FFU, true},
                      analysis::TimeReportBand{30.0, 0xC0C030FFU, true}};
        m.setTimeReport(spec, QStringLiteral("tr-1"));
        m.setTimeFormat(QStringLiteral("s.mmm"));
        QVERIFY(settle(m, 0));
        QCOMPARE(m.kind(), ReportModel::Kind::Time);
        // Laps 1 and 3 (Lap 2 is untrusted), then Eclectic, Rolling minimum, Total.
        QCOMPARE(m.columnCount(), 4);
        QCOMPARE(header(m, 0), QStringLiteral("Lap"));
        QCOMPARE(header(m, 1), QStringLiteral("Turn 1"));
        QCOMPARE(header(m, 2), QStringLiteral("Straight 1"));
        QCOMPARE(header(m, 3), QStringLiteral("Lap Time"));
        QCOMPARE(m.rowCount(), 5);
        QCOMPARE(cell(m, 0, 0), QStringLiteral("Lap 1"));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("3.000"));
        QCOMPARE(cell(m, 0, 2), QStringLiteral("7.000"));
        QCOMPARE(cell(m, 0, 3), QStringLiteral("10.000"));
        QCOMPARE(cell(m, 1, 0), QStringLiteral("Lap 3"));
        QCOMPARE(cell(m, 1, 1), QStringLiteral("2.500"));
        QCOMPARE(cell(m, 1, 2), QStringLiteral("7.500"));
        // Eclectic: 2.5 + 7.0 = 9.5; the rolling minimum over Lap 1 then Lap 3 (not contiguous: Lap 2 is not
        // a row, so no window joins them): min(3.0 + 7.0, 2.5 + 7.5) = 10.0, the earliest on a tie (Lap 1).
        QCOMPARE(m.data(m.index(2, 0), ReportModel::RowKindRole).toInt(), 1);
        QCOMPARE(cell(m, 2, 1), QStringLiteral("2.500"));
        QCOMPARE(cell(m, 2, 2), QStringLiteral("7.000"));
        QCOMPARE(cell(m, 2, 3), QStringLiteral("9.500"));
        QCOMPARE(m.data(m.index(3, 0), ReportModel::RowKindRole).toInt(), 2);
        QCOMPARE(cell(m, 3, 3), QStringLiteral("10.000"));
        QCOMPARE(m.data(m.index(4, 0), ReportModel::RowKindRole).toInt(), 3);
        QCOMPARE(cell(m, 4, 1), QStringLiteral("5.500"));
        QCOMPARE(cell(m, 4, 3), QStringLiteral("20.000"));
        // Bands: Turn 1's fastest is 2.5 s, so 3.0 s (20 % slower) takes band 1 (<= 30 %), not band 0 (10 %);
        // the fastest cells take band 0 and are bold.
        QCOMPARE(m.data(m.index(1, 1), ReportModel::BackgroundRgbaRole).toUInt(), 0x30C030FFU);
        QVERIFY(m.data(m.index(1, 1), ReportModel::BoldRole).toBool());
        QCOMPARE(m.data(m.index(0, 1), ReportModel::BackgroundRgbaRole).toUInt(), 0xC0C030FFU);
        QVERIFY(!m.data(m.index(0, 1), ReportModel::BoldRole).toBool());
        // Summary rows are not banded.
        QVERIFY(!m.data(m.index(2, 1), ReportModel::BackgroundRgbaRole).isValid());

        // Untrusted laps included: Lap 2 becomes a row (italic) and feeds the summaries (Turn 1 2.0 s).
        spec.includeUntrusted = true;
        m.setTimeReport(spec, QStringLiteral("tr-1"));
        QVERIFY(settle(m, 1));
        QCOMPARE(m.rowCount(), 6);
        QCOMPARE(cell(m, 1, 0), QStringLiteral("Lap 2"));
        QVERIFY(m.data(m.index(1, 0), ReportModel::ItalicRole).toBool());
        QCOMPARE(m.data(m.index(3, 0), ReportModel::RowKindRole).toInt(), 1);
        QCOMPARE(cell(m, 3, 1), QStringLiteral("2.000"));
        QCOMPARE(cell(m, 3, 3), QStringLiteral("9.000")); // eclectic 2.0 (Lap 2) + 7.0 (Lap 1's straight)
    }

    void timeReportFormatsAndMissingSections() {
        World w;
        // Lap 2 has no Straight 1 section: an em dash.
        const auto src =
            w.publish(reportSource("rm-c", 20.0, {{"Lap 1", 0, 10}, {"Lap 2", 10, 20}},
                                   {{"Turn 1", 0, 3}, {"Straight 1", 3, 10}, {"Turn 1", 10, 12.25}}, {},
                                   *test::shippedUnits(), w.rt.cache()));
        w.selection.setMain(refOf(*src, kLaps, "Lap 2"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        analysis::TimeReportSpec spec; // selectedOutings: both laps
        spec.summaries = analysis::TimeReportSummaries{false, false, false};
        m.setTimeReport(spec, QStringLiteral("tr-2"));
        QVERIFY(settle(m, 0));
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.columnCount(), 4);
        QCOMPARE(cell(m, 1, 0), QStringLiteral("Lap 2"));
        QCOMPARE(cell(m, 1, 1), QStringLiteral("0:02.250")); // project format: m:ss.fff by default
        QCOMPARE(cell(m, 1, 2), QString(QChar(0x2014)));
        QCOMPARE(m.data(m.index(1, 2), ReportModel::CellStateRole).toInt(),
                 static_cast<int>(analysis::ReportCellState::Blank));
        m.setProjectTimeFormat(QStringLiteral("ss.fff"));
        QCOMPARE(cell(m, 1, 1), QStringLiteral("2.250"));
        m.setTimeFormat(QStringLiteral("m:ss.mmm"));
        QCOMPARE(cell(m, 1, 3), QStringLiteral("0:10.000"));
        QCOMPARE(m.toTsv(),
                 QStringLiteral("Lap\tTurn 1\tStraight 1\tLap Time\nLap 1\t0:03.000\t0:07.000\t0:10.000\n"
                                "Lap 2\t0:02.250\t") +
                     QChar(0x2014) + QStringLiteral("\t0:10.000\n"));
        // Only the Main lap (selectedRanges): Straight 1 starts in no row's lap, so it is no column.
        spec.scope = analysis::ReportScope::SelectedRanges;
        m.setTimeReport(spec, QStringLiteral("tr-2"));
        QVERIFY(settle(m, 1));
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.columnCount(), 3);
    }

    void newerRunsReplaceOlderOnes() {
        World w;
        const auto src = w.threeLaps();
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ReportModel m(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, nullptr);
        m.setChannelReport(speedSpec({analysis::ReportStat::Avg}), QStringLiteral("cr-8"));
        // Many refreshes in one turn coalesce; selection changes in a row end on the last one.
        for (int i = 0; i < 10; ++i) {
            m.refresh();
        }
        w.selection.setMain(refOf(*src, kLaps, "Lap 3"));
        QVERIFY(QTest::qWaitFor([&] { return !m.busy() && m.deliveries() >= 1; }, 20000));
        m.refresh();
        QVERIFY(settle(m, m.deliveries()));
        QCOMPARE(header(m, 1), QStringLiteral("Lap 3"));
        QCOMPARE(cell(m, 0, 1), QStringLiteral("249.50"));
        // The same cells again come from the shared StatCache.
        const auto cache = std::make_shared<analysis::StatCache>();
        ReportModel a(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, cache);
        a.setChannelReport(speedSpec({analysis::ReportStat::Avg}), QStringLiteral("cr-9"));
        QVERIFY(settle(a, 0));
        const std::size_t held = cache->size();
        QVERIFY(held > 0);
        ReportModel b(w.rt.pool(), w.rt.dispatcher(), w.selection, w.maths, cache);
        b.setChannelReport(speedSpec({analysis::ReportStat::Avg}), QStringLiteral("cr-10"));
        QVERIFY(settle(b, 0));
        QCOMPARE(cache->size(), held);
        QCOMPARE(cell(b, 0, 1), cell(a, 0, 1));
    }
};

QTEST_GUILESS_MAIN(TstReportModel)
#include "tst_report_model.moc"
