// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelReport (04 section 12; 07 section 6.1; AC-M3-03 display), offscreen: created by type over its
// ReportModel and the workspace StatCache, the frozen first column, the cells and the highlight and italic
// styling in the views, refresh on Selection and Data, the zoom link, the body's layout and delta columns,
// addChannel as one spec edit, Export Data (CSV) and Copy as TSV, the caption, the focus border and Print to
// Clipboard. Expected cells come from the fixture's closed-form channels (Speed sample i = i at 10 Hz).

#include "components/reports_track/rt_support.hpp"

#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/channel_report.hpp>

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::rtw;

namespace {

class BytesSink final : public ExportSink {
public:
    bool write(const QByteArray& bytes) override {
        data += bytes;
        return true;
    }
    QByteArray data;
};

QJsonObject reportBody(const QJsonArray& channels, const QString& layout = QStringLiteral("channelsInRows"),
                       const QJsonObject& options = {}, const QJsonArray& headers = {}) {
    QJsonObject report{{QStringLiteral("scope"), QStringLiteral("selectedRanges")},
                       {QStringLiteral("channels"), channels},
                       {QStringLiteral("options"), options}};
    if (!headers.isEmpty()) {
        report.insert(QStringLiteral("headers"), headers);
    }
    return QJsonObject{
        {QStringLiteral("report"), report},
        {QStringLiteral("layout"), layout},
        {QStringLiteral("highlight"),
         QJsonObject{
             {QStringLiteral("min"), QJsonObject{{QStringLiteral("on"), true},
                                                 {QStringLiteral("colour"), QStringLiteral("#2060ff")}}},
             {QStringLiteral("max"), QJsonObject{{QStringLiteral("on"), true},
                                                 {QStringLiteral("colour"), QStringLiteral("#ff3030")}}}}},
        {QStringLiteral("deltaVsOverlay0"), false},
        {QStringLiteral("timeFormat"), QStringLiteral("project")}};
}

QJsonArray speed(const QStringList& stats) {
    return QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Speed")},
                                  {QStringLiteral("stats"), QJsonArray::fromStringList(stats)}}};
}

QString text(const QTableView* view, int row, int column) {
    return view->model()->index(row, column).data().toString();
}

} // namespace

class TstChannelReport : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void createdByTypeWithAFrozenFirstColumn() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay0(refOf(*src, kLaps, "Lap 3"));
        const project::ComponentSpec spec = componentSpec(
            ChannelReport::kType, reportBody(speed({QStringLiteral("avg"), QStringLiteral("max")})));
        std::unique_ptr<ComponentWidget> owner(ComponentWidget::create(spec, *w.context));
        auto* report = qobject_cast<ChannelReport*>(owner.get());
        QVERIFY(report != nullptr);
        report->resize(480, 220);
        report->show();
        QVERIFY(settle(report->model(), 0));
        QCOMPARE(report->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
        QCOMPARE(report->commandContext(), qt::CommandContext::Component);
        QCOMPARE(report->channelNames(), QStringList{QStringLiteral("Speed")});
        QVERIFY(report->acceptsChannelDrop());

        QTableView* frozen = report->frozenView();
        QTableView* body = report->tableView();
        QCOMPARE(frozen->model(), body->model()); // one model, two views
        QCOMPARE(frozen->selectionModel(), body->selectionModel());
        QCOMPARE(body->model()->columnCount(), 3);
        QVERIFY(!frozen->isColumnHidden(0));
        QVERIFY(frozen->isColumnHidden(1) && frozen->isColumnHidden(2));
        QVERIFY(body->isColumnHidden(0));
        QVERIFY(!body->isColumnHidden(1) && !body->isColumnHidden(2));
        QVERIFY(!body->tabKeyNavigation());
        QCOMPARE(text(frozen, 0, 0), QStringLiteral("Speed avg [m/s]"));
        QCOMPARE(text(body, 0, 1), QStringLiteral("49.50"));
        QCOMPARE(text(body, 1, 2), QStringLiteral("299.00"));
        QCOMPARE(body->model()->headerData(2, Qt::Horizontal).toString(),
                 QStringLiteral("Lap 3 (Overlay 0)"));
        QCOMPARE(report->model().pendingCells(), 0);

        // The highlight tints the cells (the colour over the table background), not the labels.
        const QVariant minBg = body->model()->index(0, 1).data(Qt::BackgroundRole);
        const QVariant maxBg = body->model()->index(0, 2).data(Qt::BackgroundRole);
        QVERIFY(minBg.canConvert<QColor>() && maxBg.canConvert<QColor>());
        const QColor minC = minBg.value<QColor>();
        const QColor maxC = maxBg.value<QColor>();
        QVERIFY(minC.blue() > minC.red()); // blue-ish
        QVERIFY(maxC.red() > maxC.blue()); // red-ish
        QVERIFY(!body->model()->index(0, 0).data(Qt::BackgroundRole).isValid());

        // The caption (auto text) and the focus border.
        auto* caption = report->findChild<QLabel*>(QStringLiteral("reportCaption"));
        QVERIFY(caption != nullptr);
        QCOMPARE(caption->text(), QStringLiteral("Channel Report"));
        QVERIFY(caption->isVisible());
        report->setFocusedComponent(true);
        const QImage focused = report->renderToImage(1.0);
        QCOMPARE(focused.size(), QSize(480, 220));
        const QRgb border = focused.pixel(0, 100);
        const project::Rgba focus = w.scheme->role("cptFocusBorder", 0x2060FFFFU);
        QCOMPARE(qRed(border), static_cast<int>((focus >> 24) & 0xFFU));
        QCOMPARE(qBlue(border), static_cast<int>((focus >> 8) & 0xFFU));
        report->setFocusedComponent(false);
        QVERIFY(report->renderToImage(1.0).pixel(0, 100) != border);
        // Print to Clipboard at DPR 2: the same widget at twice the pixels.
        const QImage hi = report->renderToImage(2.0);
        QCOMPARE(hi.size(), QSize(960, 440));
        QCOMPARE(hi.devicePixelRatio(), 2.0);
    }

    void untrustedRowsItalicInTheView() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay(refOf(*src, kLaps, "Lap 2"));
        ChannelReport report(
            componentSpec(ChannelReport::kType,
                          reportBody(speed({QStringLiteral("avg")}), QStringLiteral("channelsInColumns"),
                                     QJsonObject{{QStringLiteral("includeUntrusted"), true}})),
            *w.context);
        QVERIFY(settle(report.model(), 0));
        QTableView* body = report.tableView();
        QCOMPARE(body->model()->rowCount(), 2);
        QVERIFY(body->model()->index(1, 1).data(Qt::FontRole).value<QFont>().italic());
        QVERIFY(report.frozenView()->model()->index(1, 0).data(Qt::FontRole).value<QFont>().italic());
        QVERIFY(!body->model()->index(0, 1).data(Qt::FontRole).value<QFont>().italic());
        QCOMPARE(body->model()->index(0, 1).data(Qt::FontRole).value<QFont>().family(),
                 QString::fromStdString(w.scheme->font("reportTable").family));
    }

    void selectionDataAndZoomRefresh() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ChannelReport report(componentSpec(ChannelReport::kType, reportBody(speed({QStringLiteral("avg")}))),
                             *w.context);
        QVERIFY(settle(report.model(), 0));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("49.50"));
        // Selection: Main moves to Lap 3.
        const int before = report.model().deliveries();
        w.selection.setMain(refOf(*src, kLaps, "Lap 3"));
        QVERIFY(settle(report.model(), before));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("249.50"));
        // Not zoom linked: a zoom change does not re-run the engine.
        const int unlinked = report.model().deliveries();
        w.view->zoomTo(plot::XSpan{0.0, 5.0e6});
        QTest::qWait(50);
        QCOMPARE(report.model().deliveries(), unlinked);
        QVERIFY(!report.model().busy());

        // Zoom linked: the cells follow the worksheet zoom (each range clipped through its own mapper).
        const QJsonObject linked =
            reportBody(speed({QStringLiteral("avg")}), QStringLiteral("channelsInRows"),
                       QJsonObject{{QStringLiteral("zoomLinked"), true}});
        project::ComponentSpec next = report.spec();
        next.body = docOf(linked);
        report.applySpec(next);
        QVERIFY(settle(report.model(), unlinked));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("224.50")); // samples 200..249
        const int zoomed = report.model().deliveries();
        w.view->zoomTo(plot::XSpan{5.0e6, 10.0e6});
        QVERIFY(settle(report.model(), zoomed));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("274.50")); // samples 250..299

        // Data: a source that is not shown does not re-run it; the shown one does.
        const int quiet = report.model().deliveries();
        report.onDataChanged(model::SourceId{}, 0);
        QTest::qWait(50);
        QCOMPARE(report.model().deliveries(), quiet);
        report.onDataChanged(src->id, 0);
        QVERIFY(settle(report.model(), quiet));
    }

    void bodyLayoutAndDeltaColumns() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        w.selection.toggleOverlay0(refOf(*src, kLaps, "Lap 3"));
        QJsonObject body = reportBody(speed({QStringLiteral("min")}), QStringLiteral("channelsInColumns"), {},
                                      QJsonArray{QStringLiteral("Outings")});
        body.insert(QStringLiteral("deltaVsOverlay0"), true);
        ChannelReport report(componentSpec(ChannelReport::kType, body), *w.context);
        QVERIFY(settle(report.model(), 0));
        QAbstractItemModel* m = report.tableView()->model();
        QCOMPARE(m->rowCount(), 2);    // Main and Overlay 0 headings
        QCOMPARE(m->columnCount(), 3); // label, min, min delta
        QCOMPARE(m->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Outings"));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("0.00"));
        QCOMPARE(text(report.tableView(), 0, 2), QStringLiteral("-200.00")); // 0 - 200
        QCOMPARE(text(report.tableView(), 1, 2), QString());
        // The highlight off from the body.
        QJsonObject off = body;
        off.insert(QStringLiteral("highlight"),
                   QJsonObject{{QStringLiteral("min"), QJsonObject{{QStringLiteral("on"), false}}},
                               {QStringLiteral("max"), QJsonObject{{QStringLiteral("on"), false}}}});
        project::ComponentSpec next = report.spec();
        next.body = docOf(off);
        report.applySpec(next);
        QVERIFY(settle(report.model(), 1));
        QVERIFY(!m->index(0, 1).data(Qt::BackgroundRole).isValid());
    }

    void addChannelIsOneSpecEdit() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ChannelReport report(componentSpec(ChannelReport::kType, reportBody(speed({QStringLiteral("avg")}))),
                             *w.context);
        QSignalSpy edits(&report, &ComponentWidget::specEditRequested);
        report.addChannel(QStringLiteral("Speed"), false); // already reported
        QCOMPARE(edits.count(), 0);
        report.addChannel(QStringLiteral("Load"), false);
        QCOMPARE(edits.count(), 1);
        const auto next = edits.at(0).at(0).value<project::ComponentSpec>();
        const QJsonObject body = objectOf(next.body);
        const QJsonArray channels =
            body.value(QStringLiteral("report")).toObject().value(QStringLiteral("channels")).toArray();
        QCOMPARE(channels.size(), 2);
        QCOMPARE(channels.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Load"));
        // The other members of the body survive.
        QCOMPARE(body.value(QStringLiteral("highlight")), reportBody({}).value(QStringLiteral("highlight")));
        report.applySpec(next);
        QCOMPARE(report.channelNames(), (QStringList{QStringLiteral("Speed"), QStringLiteral("Load")}));
        QVERIFY(settle(report.model(), 0));
        QCOMPARE(report.tableView()->model()->rowCount(), 4); // Speed avg; Load avg, min, max (the defaults)
        QCOMPARE(text(report.frozenView(), 1, 0), QStringLiteral("Load avg [%]"));
        QCOMPARE(text(report.tableView(), 1, 1), QStringLiteral("99.0")); // Load = 2 i, dps 1
        QCOMPARE(text(report.tableView(), 3, 1), QStringLiteral("198.0"));
    }

    void exportDataAndCopyAsTsv() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        ChannelReport report(
            componentSpec(ChannelReport::kType,
                          reportBody(QJsonArray{
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("Load")},
                                          {QStringLiteral("stats"), QJsonArray{QStringLiteral("max")}}}})),
            *w.context);
        QVERIFY(settle(report.model(), 0));
        BytesSink sink;
        QVERIFY(report.exportData(sink, ExportOptions{}));
        QCOMPARE(sink.data, QByteArray("Channel,Lap 1\n\"Load max [%]\",198.0\n"));
        report.copyTsv();
        QCOMPARE(QApplication::clipboard()->text(),
                 QStringLiteral("Channel\tLap 1\n\"Load max [%]\"\t198.0\n"));
        QCOMPARE(report.toTsv(), QApplication::clipboard()->text());
        QVERIFY(report.contextCommands().contains(QStringLiteral("component.export_data")));
        QVERIFY(report.contextCommands().contains(QStringLiteral("component.print_clipboard")));
    }

    void captionFromTheSpecAndSchemeFonts() {
        World w;
        project::ComponentSpec spec =
            componentSpec(ChannelReport::kType, reportBody(speed({QStringLiteral("avg")})));
        spec.caption.autoText = false;
        spec.caption.text = "Speeds";
        ChannelReport report(spec, *w.context);
        auto* caption = report.findChild<QLabel*>(QStringLiteral("reportCaption"));
        QCOMPARE(caption->text(), QStringLiteral("Speeds"));
        spec.caption.show = false;
        report.applySpec(spec);
        QVERIFY(caption->isHidden());
        QCOMPARE(report.tableView()->font().family(),
                 QString::fromStdString(w.scheme->font("reportTable").family));
        project::Scheme bigger = *w.scheme;
        bigger.fonts["reportTable"].pointSize = 14.0;
        report.onSchemeChanged(bigger);
        QCOMPARE(report.tableView()->font().pointSizeF(), 14.0);
        // No selection: an empty table, no error.
        QVERIFY(settle(report.model(), 0));
        QCOMPARE(report.tableView()->model()->columnCount(), 1);
        QCOMPARE(report.model().lastError(), QString());
    }
};

QTEST_MAIN(TstChannelReport)
#include "tst_channel_report.moc"
