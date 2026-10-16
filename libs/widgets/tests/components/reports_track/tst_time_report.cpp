// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReport (04 section 13; 07 section 6.1; AC-M3-03 display), offscreen: created by type, laps x sections
// with the lap time and the Eclectic / Rolling minimum / Total rows in the views, the % bands as cell tints
// and the fastest time bold, ALT+n toggling band n as one spec edit (a band beyond the list a no-op), the
// untrusted laps italic when included, the time format from the body, refresh on Selection, Export Data and
// Copy as TSV. Expected times come from the hand-written section ranges of the fixture.

#include "components/reports_track/rt_support.hpp"

#include <esses/qtmodel/report_model.hpp>
#include <esses/widgets/components/time_report.hpp>

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
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

QJsonObject band(double pct, const QString& colour, bool visible = true) {
    return QJsonObject{{QStringLiteral("pct"), pct},
                       {QStringLiteral("colour"), colour},
                       {QStringLiteral("visible"), visible}};
}

QJsonObject timeBody(bool includeUntrusted = false, const QString& format = QStringLiteral("s.mmm")) {
    return QJsonObject{
        {QStringLiteral("scope"), QStringLiteral("selectedOutings")},
        {QStringLiteral("rowGroup"), QStringLiteral("Outings:Laps:Track Sections:Default")},
        {QStringLiteral("options"), QJsonObject{{QStringLiteral("includeUntrusted"), includeUntrusted}}},
        {QStringLiteral("summaries"), QJsonObject{{QStringLiteral("eclectic"), true},
                                                  {QStringLiteral("rollingMin"), true},
                                                  {QStringLiteral("total"), true}}},
        {QStringLiteral("bands"),
         QJsonArray{band(10.0, QStringLiteral("#30c030")), band(30.0, QStringLiteral("#c0c030"))}},
        {QStringLiteral("timeFormat"), format}};
}

QString text(const QTableView* view, int row, int column) {
    return view->model()->index(row, column).data().toString();
}

} // namespace

class TstTimeReport : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void lapsSectionsAndSummaries() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        std::unique_ptr<ComponentWidget> owner(
            ComponentWidget::create(componentSpec(TimeReport::kType, timeBody()), *w.context));
        auto* report = qobject_cast<TimeReport*>(owner.get());
        QVERIFY(report != nullptr);
        report->resize(420, 200);
        report->show();
        QVERIFY(settle(report->model(), 0));
        QCOMPARE(report->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
        QCOMPARE(report->commandContext(), qt::CommandContext::Component);
        QTableView* body = report->tableView();
        QTableView* frozen = report->frozenView();
        QAbstractItemModel* m = body->model();
        QCOMPARE(m->columnCount(), 4);
        QCOMPARE(m->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Lap Time"));
        QCOMPARE(m->rowCount(), 5); // Lap 1, Lap 3, Eclectic, Rolling minimum, Total
        QCOMPARE(text(frozen, 0, 0), QStringLiteral("Lap 1"));
        QCOMPARE(text(frozen, 2, 0), QStringLiteral("Eclectic"));
        QCOMPARE(text(frozen, 3, 0), QStringLiteral("Rolling minimum"));
        QCOMPARE(text(frozen, 4, 0), QStringLiteral("Total"));
        QCOMPARE(text(body, 0, 1), QStringLiteral("3.000"));
        QCOMPARE(text(body, 1, 1), QStringLiteral("2.500"));
        QCOMPARE(text(body, 2, 3), QStringLiteral("9.500"));
        QCOMPARE(text(body, 3, 3), QStringLiteral("10.000"));
        QCOMPARE(text(body, 4, 3), QStringLiteral("20.000"));
        // Bands tint the cells; the fastest of a column is bold, summary rows are bold too.
        const QColor green = m->index(1, 1).data(Qt::BackgroundRole).value<QColor>();
        const QColor yellow = m->index(0, 1).data(Qt::BackgroundRole).value<QColor>();
        QVERIFY(green.isValid() && yellow.isValid());
        QVERIFY(green.green() > green.red());
        QVERIFY(yellow.red() > yellow.blue() && yellow.green() > yellow.blue());
        QVERIFY(m->index(1, 1).data(Qt::FontRole).value<QFont>().bold());
        QVERIFY(!m->index(0, 1).data(Qt::FontRole).value<QFont>().bold());
        QVERIFY(m->index(2, 1).data(Qt::FontRole).value<QFont>().bold());
        QVERIFY(!m->index(2, 1).data(Qt::BackgroundRole).isValid());
    }

    void altNTogglesBandNAsOneEdit() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        TimeReport report(componentSpec(TimeReport::kType, timeBody()), *w.context);
        report.resize(420, 200);
        report.show();
        QVERIFY(settle(report.model(), 0));
        QSignalSpy edits(&report, &ComponentWidget::specEditRequested);
        // ALT+1 in the table (the shell's binder has no handler for component.colour_band here: the key
        // reaches the component).
        report.tableView()->setFocus();
        QTest::keyClick(report.tableView(), Qt::Key_1, Qt::AltModifier);
        QCOMPARE(edits.count(), 1);
        auto next = edits.at(0).at(0).value<project::ComponentSpec>();
        QCOMPARE(edits.at(0).at(1).toString(), QStringLiteral("Hide band 1"));
        const QJsonArray bands = objectOf(next.body).value(QStringLiteral("bands")).toArray();
        QVERIFY(bands.at(0).toObject().value(QStringLiteral("visible")).toBool());
        QVERIFY(!bands.at(1).toObject().value(QStringLiteral("visible")).toBool());
        report.applySpec(next);
        QVERIFY(settle(report.model(), 1));
        // Band 1 hidden: Lap 1's Turn 1 (20 % slower) is no longer tinted.
        QVERIFY(!report.tableView()->model()->index(0, 1).data(Qt::BackgroundRole).isValid());
        QVERIFY(report.tableView()->model()->index(1, 1).data(Qt::BackgroundRole).isValid());
        // ALT+9: no band 9, no edit; toggleBand reports it.
        QTest::keyClick(report.tableView(), Qt::Key_9, Qt::AltModifier);
        QCOMPARE(edits.count(), 1);
        QVERIFY(!report.toggleBand(9));
        QVERIFY(report.toggleBand(1));
        QCOMPARE(edits.count(), 2);
        QCOMPARE(edits.at(1).at(1).toString(), QStringLiteral("Show band 1"));
        // The shell's route: component.colour_band [n] offered to the focused component.
        QVERIFY(report.handleCommand(QStringLiteral("component.colour_band"), {QVariant(0)}));
        QCOMPARE(edits.count(), 3);
        QCOMPARE(edits.at(2).at(1).toString(), QStringLiteral("Hide band 0"));
        QVERIFY(
            report.handleCommand(QStringLiteral("component.colour_band"), {QVariant(7)})); // handled no-op
        QCOMPARE(edits.count(), 3);
        QVERIFY(!report.handleCommand(QStringLiteral("component.cycle_style"), {}));
    }

    void untrustedLapsItalicWhenIncluded() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        TimeReport report(componentSpec(TimeReport::kType, timeBody(true)), *w.context);
        QVERIFY(settle(report.model(), 0));
        QAbstractItemModel* m = report.tableView()->model();
        QCOMPARE(m->rowCount(), 6);
        QCOMPARE(text(report.frozenView(), 1, 0), QStringLiteral("Lap 2"));
        QVERIFY(m->index(1, 0).data(Qt::FontRole).value<QFont>().italic());
        QVERIFY(m->index(1, 2).data(Qt::FontRole).value<QFont>().italic());
        QVERIFY(!m->index(0, 2).data(Qt::FontRole).value<QFont>().italic());
        QCOMPARE(text(report.tableView(), 3, 3), QStringLiteral("9.000"));
    }

    void timeFormatSelectionAndExport() {
        World w;
        const auto src = threeLaps(w);
        w.selection.setMain(refOf(*src, kLaps, "Lap 1"));
        QJsonObject body = timeBody(false, QStringLiteral("m:ss.mmm"));
        body.insert(QStringLiteral("scope"), QStringLiteral("selectedRanges"));
        body.insert(QStringLiteral("summaries"), QJsonObject{{QStringLiteral("eclectic"), false},
                                                             {QStringLiteral("rollingMin"), false},
                                                             {QStringLiteral("total"), false}});
        TimeReport report(componentSpec(TimeReport::kType, body), *w.context);
        QVERIFY(settle(report.model(), 0));
        QCOMPARE(report.tableView()->model()->rowCount(), 1);
        QCOMPARE(text(report.tableView(), 0, 3), QStringLiteral("0:10.000"));
        // Selection: Lap 3 becomes Main.
        w.selection.setMain(refOf(*src, kLaps, "Lap 3"));
        QVERIFY(settle(report.model(), 1));
        QCOMPARE(text(report.frozenView(), 0, 0), QStringLiteral("Lap 3"));
        QCOMPARE(text(report.tableView(), 0, 1), QStringLiteral("0:02.500"));
        BytesSink sink;
        QVERIFY(report.exportData(sink, ExportOptions{}));
        QCOMPARE(sink.data, QByteArray("Lap,Turn 1,Straight 1,Lap Time\nLap 3,0:02.500,0:07.500,0:10.000\n"));
        report.copyTsv();
        QCOMPARE(QApplication::clipboard()->text(),
                 QStringLiteral("Lap\tTurn 1\tStraight 1\tLap Time\nLap 3\t0:02.500\t0:07.500\t0:10.000\n"));
        const QImage img = report.renderToImage(1.0);
        QVERIFY(!img.isNull());
    }
};

QTEST_MAIN(TstTimeReport)
#include "tst_time_report.moc"
