// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelStatusWindow (06 section 10, AC-M2-12): every ResolutionTrace kind (mapping, alias, case-fold,
// logged name, missing) produced by a project and rendered in the window's detail pane; missing workbook
// references shown as "(missing)" and never dropped; the filter box (word prefix on the channel, substring of
// the other columns), "Errors only", Copy Table as TSV with a header line, the error styling, sorting, the
// current row kept across the model's refreshes, focusChannel (channel.status) clearing a filter that hides
// the row, and the double-click routing to the Maths Editor, the Alias editor and the Mapping editor (signal
// and registered command).

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "maths_editors_fixture.hpp"

#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSortFilterProxyModel>
#include <QTableView>

using namespace esses;
using namespace esses::widgets;
namespace mt = esses::widgets::mathstest;
using StatusModel = qt::ChannelStatusModel;

namespace {

// The project of tst_maths_service's AC-M2-12 case: a maths item, a constant, aliases that resolve by case
// fold, through a logged spelling, to a maths output or not at all, a used and an unused mapping, and the
// shipped workbook's references (several are not logged by the synthetic source).
std::shared_ptr<const model::DataSource> buildProject(mt::Workspace& ws, const std::string& name) {
    maths::MathModule user =
        mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2"), mt::item("Bad", "'Nope'")});
    maths::MathConstant k;
    k.name = "Track Length";
    k.value.v = 2000.0;
    k.value.unit = *qt::test::shippedUnits()->find("m");
    user.constants.push_back(k);
    ws.setModules({user});
    project::Aliases aliases;
    aliases.table.rows.push_back(model::AliasRow{"Speed Any", {{"wheel speed fl", true}}, -1, std::nullopt});
    aliases.table.rows.push_back(model::AliasRow{"Pedal", {{"TPS", true}}, -1, std::nullopt});
    aliases.table.rows.push_back(model::AliasRow{"Nothing", {{"Nope", true}}, -1, std::nullopt});
    ws.session.undoStack().push(new qt::EditAliasesCommand(ws.session, aliases, QStringLiteral("Aliases")));
    project::Mappings mappings;
    mappings.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
    mappings.rows.push_back(project::Mappings::Row{"Unused", "Elsewhere", std::nullopt, false});
    ws.session.undoStack().push(
        new qt::EditMappingsCommand(ws.session, mappings, QStringLiteral("Mappings")));
    qt::test::SyntheticOptions o;
    o.throttleLoggedName = "TPS"; // renamed at load by the mapping
    return ws.openSynthetic(name, o);
}

int visibleRowOf(ChannelStatusWindow& w, const QString& kind, const QString& channel) {
    QSortFilterProxyModel& p = w.filterModel();
    for (int r = 0; r < p.rowCount(); ++r) {
        if (p.index(r, StatusModel::KindColumn).data().toString() == kind &&
            p.index(r, StatusModel::ChannelColumn).data().toString() == channel) {
            return r;
        }
    }
    return -1;
}

// The first visible row whose trace has a step of `kind`, selected; its detail text.
QString detailOfTraceKind(ChannelStatusWindow& w, const QString& stepKind) {
    QSortFilterProxyModel& p = w.filterModel();
    for (int r = 0; r < p.rowCount(); ++r) {
        const QString trace = p.index(r, 0).data(StatusModel::TraceRole).toString();
        if (trace.contains(stepKind + QStringLiteral(": "))) {
            w.table().setCurrentIndex(p.index(r, 0));
            return w.detailPane().toPlainText();
        }
    }
    return {};
}

} // namespace

class TstChannelStatusWindow : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { mt::registerMetaTypes(); }

    // AC-M2-12: every ResolutionTrace kind is produced and rendered; "(missing)" rows stay.
    void everyTraceKindIsRendered() {
        mt::Workspace ws("csw-traces");
        buildProject(ws, "csw-traces");
        StatusModel model(ws.maths, &ws.session);
        model.refresh();
        qt::CommandRegistry commands;
        ChannelStatusWindow window(model, commands);
        window.show();
        QCOMPARE(window.table().model(), &window.filterModel());
        QCOMPARE(window.filterModel().columnCount(), StatusModel::kColumnCount);
        for (const char* kind : {"mapping", "alias", "case-fold", "missing"}) {
            const QString detail = detailOfTraceKind(window, QString::fromLatin1(kind));
            QVERIFY2(!detail.isEmpty(), kind);
            QVERIFY2(detail.contains(QStringLiteral("Resolution:")), kind);
            QVERIFY2(detail.contains(QString::fromLatin1(kind) + QStringLiteral(": '")), kind);
        }
        // The alias without a usable candidate and the workbook references this source lacks: "(missing)".
        const int nothing = visibleRowOf(window, QStringLiteral("Alias"), QStringLiteral("Nothing"));
        QVERIFY(nothing >= 0);
        QCOMPARE(window.filterModel().index(nothing, StatusModel::StatusColumn).data().toString(),
                 QStringLiteral("(missing)"));
        QCOMPARE(window.filterModel().index(nothing, StatusModel::ResolvedColumn).data().toString(),
                 QStringLiteral("(missing)"));
        int references = 0;
        for (int r = 0; r < window.filterModel().rowCount(); ++r) {
            if (window.filterModel().index(r, StatusModel::KindColumn).data().toString() ==
                QStringLiteral("Reference")) {
                QCOMPARE(window.filterModel().index(r, StatusModel::ResolvedColumn).data().toString(),
                         QStringLiteral("(missing)"));
                ++references;
            }
        }
        QVERIFY(references >= 1);
        // The mapping row shows the hop and the detail pane the step list.
        QVERIFY(window.selectRow(QStringLiteral("Throttle"), QStringLiteral("Mapping")));
        QVERIFY(window.detailPane().toPlainText().contains(QStringLiteral("mapping")));
        QVERIFY(window.detailPane().toPlainText().contains(QStringLiteral("Resolved to: 'TPS'")));
        // The logged-name step: the file logged 'TPS', renamed 'Throttle' at load by the mapping; without the
        // mapping the alias candidate 'TPS' still finds the channel by its logged spelling.
        ws.session.undoStack().push(
            new qt::EditMappingsCommand(ws.session, {}, QStringLiteral("No mappings")));
        model.refresh();
        const QString logged = detailOfTraceKind(window, QStringLiteral("logged name"));
        QVERIFY(!logged.isEmpty());
        QVERIFY(logged.contains(QStringLiteral("logged name: 'TPS'")));
        QVERIFY(window.selectRow(QStringLiteral("Pedal"), QStringLiteral("Alias")));
        QVERIFY(window.detailPane().toPlainText().contains(QStringLiteral("Status: OK")));
        // Error rows are red; the tooltip of "Resolved to" carries the trace.
        const int bad = visibleRowOf(window, QStringLiteral("Expression"), QStringLiteral("Bad"));
        QVERIFY(bad >= 0);
        QCOMPARE(window.filterModel().index(bad, 0).data(Qt::ForegroundRole).value<QColor>(),
                 QColor(0xD3, 0x2F, 0x2F));
        const int speed = visibleRowOf(window, QStringLiteral("Alias"), QStringLiteral("Speed Any"));
        QVERIFY(window.filterModel()
                    .index(speed, StatusModel::ResolvedColumn)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("case-fold")));
    }

    // Filter box, errors only, Copy Table, sorting, the current row across refreshes.
    void filterErrorsOnlyCopyAndSelection() {
        mt::Workspace ws("csw-filter");
        const auto src = buildProject(ws, "csw-filter");
        StatusModel model(ws.maths, &ws.session);
        model.refresh();
        qt::CommandRegistry commands;
        ChannelStatusWindow window(model, commands);
        window.show();
        const int all = window.filterModel().rowCount();
        QVERIFY(all > 5);
        // Word prefix on the channel name ("tw" finds Twice).
        window.filterEdit().setText(QStringLiteral("tw"));
        QCOMPARE(window.filterModel().rowCount(), 1);
        QCOMPARE(window.filterModel().index(0, StatusModel::ChannelColumn).data().toString(),
                 QStringLiteral("Twice"));
        // A substring of the other columns ("mapping" in Kind / Resolved to).
        window.filterEdit().setText(QStringLiteral("mapping"));
        QVERIFY(window.filterModel().rowCount() >= 2);
        window.filterEdit().clear();
        QCOMPARE(window.filterModel().rowCount(), all);
        // Errors only.
        window.setErrorsOnly(true);
        QVERIFY(window.errorsOnlyBox().isChecked());
        QVERIFY(window.filterModel().rowCount() >= 1);
        for (int r = 0; r < window.filterModel().rowCount(); ++r) {
            QVERIFY(window.filterModel().index(r, 0).data(StatusModel::ErrorRole).toBool());
        }
        window.errorsOnlyBox().setChecked(false);
        QVERIFY(!window.errorsOnly());
        QCOMPARE(window.filterModel().rowCount(), all);
        // Copy Table: a header line and one tab-separated line per visible row.
        window.filterEdit().setText(QStringLiteral("tw"));
        const QString tsv = window.tableText();
        const QStringList lines = tsv.trimmed().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.front(), QStringLiteral("Source\tChannel\tKind\tStatus\tResolved to\tRate\tUnit"));
        QCOMPARE(lines.back().split(QLatin1Char('\t')).size(), StatusModel::kColumnCount);
        QVERIFY(lines.back().contains(QStringLiteral("Twice")));
        window.copyTable();
        QCOMPARE(QApplication::clipboard()->text(), tsv);
        window.filterEdit().clear();
        // Sorting by the channel column.
        window.table().sortByColumn(StatusModel::ChannelColumn, Qt::AscendingOrder);
        for (int r = 1; r < window.filterModel().rowCount(); ++r) {
            QVERIFY(QString::localeAwareCompare(
                        window.filterModel().index(r - 1, StatusModel::ChannelColumn).data().toString(),
                        window.filterModel().index(r, StatusModel::ChannelColumn).data().toString()) <= 0);
        }
        // The current row survives a refresh that resets the model (a new item adds a row).
        QVERIFY(window.selectRow(QStringLiteral("Twice"), QStringLiteral("Expression")));
        ws.setModules({mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2"), mt::item("Bad", "'Nope'"),
                                           mt::item("Third", "3")})});
        model.refresh();
        const int current = window.currentSourceRow();
        QVERIFY(current >= 0);
        QCOMPARE(model.rows()[static_cast<std::size_t>(current)].channel, QStringLiteral("Twice"));
        QVERIFY(window.detailPane().toPlainText().startsWith(QStringLiteral("Twice")));
        // The status updates reach the detail pane.
        static_cast<void>(
            ws.maths.require(maths::ChannelKey::make(src->id, "Normal", "Twice"), Priority::Visible));
        QTRY_VERIFY_WITH_TIMEOUT(window.detailPane().toPlainText().contains(QStringLiteral("Status: OK")),
                                 30000);
    }

    // channel.status (focusChannel) and the double-click routing.
    void focusAndOpenItems() {
        mt::Workspace ws("csw-open");
        const auto src = buildProject(ws, "csw-open");
        StatusModel model(ws.maths, &ws.session);
        model.refresh();
        qt::CommandRegistry commands;
        QVariantList mathsArgs;
        QVariantList aliasArgs;
        QVariantList mappingArgs;
        const auto addTool = [&commands](const char* id, QVariantList* sink) {
            qt::CommandSpec s;
            s.id = QString::fromLatin1(id);
            s.title = s.id;
            s.invoke = [sink](const QVariantList& args) { *sink = args; };
            commands.add(std::move(s));
        };
        addTool("tools.maths_editor", &mathsArgs);
        addTool("tools.aliases", &aliasArgs);
        addTool("tools.mappings", &mappingArgs);
        ChannelStatusWindow window(model, commands);
        window.show();
        QSignalSpy open(&window, &ChannelStatusWindow::openItemRequested);

        // focusChannel selects the maths output's row, clearing a filter that hides it.
        window.filterEdit().setText(QStringLiteral("zzz"));
        QCOMPARE(window.filterModel().rowCount(), 0);
        QVERIFY(window.focusChannel(maths::ChannelKey::make(src->id, "Normal", "Twice")));
        QVERIFY(window.filterEdit().text().isEmpty());
        QCOMPARE(model.rows()[static_cast<std::size_t>(window.currentSourceRow())].channel,
                 QStringLiteral("Twice"));
        QVERIFY(!window.focusChannel(maths::ChannelKey::make(src->id, "Normal", "Wheel Speed FL")));

        // Double-click: a maths item.
        const int twice = visibleRowOf(window, QStringLiteral("Expression"), QStringLiteral("Twice"));
        const QRect cell =
            window.table().visualRect(window.filterModel().index(twice, StatusModel::ChannelColumn));
        QTest::mouseClick(window.table().viewport(), Qt::LeftButton, {}, cell.center());
        QTest::mouseDClick(window.table().viewport(), Qt::LeftButton, {}, cell.center());
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.at(0).at(0).toString(), QStringLiteral("maths"));
        QCOMPARE(open.at(0).at(1).toString(), QStringLiteral("user"));
        QCOMPARE(open.at(0).at(2).toString(), QStringLiteral("Twice"));
        QCOMPARE(mathsArgs, (QVariantList{QStringLiteral("user"), QStringLiteral("Twice")}));
        // ENTER on a constant opens the Maths Editor at it too.
        const int constant = visibleRowOf(window, QStringLiteral("Constant"), QStringLiteral("Track Length"));
        window.table().setCurrentIndex(window.filterModel().index(constant, 0));
        QTest::keyClick(&window.table(), Qt::Key_Return);
        QCOMPARE(open.count(), 2);
        QCOMPARE(open.at(1).at(2).toString(), QStringLiteral("Track Length"));
        // An alias and a mapping (the logged name).
        QVERIFY(window.openRow(visibleRowOf(window, QStringLiteral("Alias"), QStringLiteral("Pedal"))));
        QCOMPARE(open.at(2).at(0).toString(), QStringLiteral("alias"));
        QCOMPARE(open.at(2).at(1).toString(), QStringLiteral("Pedal"));
        QCOMPARE(aliasArgs, QVariantList{QStringLiteral("Pedal")});
        QVERIFY(window.openRow(visibleRowOf(window, QStringLiteral("Mapping"), QStringLiteral("Throttle"))));
        QCOMPARE(open.at(3).at(0).toString(), QStringLiteral("mapping"));
        QCOMPARE(open.at(3).at(1).toString(), QStringLiteral("TPS"));
        QCOMPARE(mappingArgs, QVariantList{QStringLiteral("TPS")});
        // A built-in output and a missing reference have no editor.
        QVERIFY(
            !window.openRow(visibleRowOf(window, QStringLiteral("Built-in"), QStringLiteral("Corr Dist"))));
        int reference = -1;
        for (int r = 0; r < window.filterModel().rowCount() && reference < 0; ++r) {
            if (window.filterModel().index(r, StatusModel::KindColumn).data().toString() ==
                QStringLiteral("Reference")) {
                reference = r;
            }
        }
        QVERIFY(!window.openRow(reference));
        QCOMPARE(open.count(), 4);
    }
};

QTEST_MAIN(TstChannelStatusWindow)
#include "tst_channel_status_window.moc"
