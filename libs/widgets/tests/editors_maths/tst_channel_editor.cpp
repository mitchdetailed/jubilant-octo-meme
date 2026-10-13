// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelEditor (06 section 13.1, D-010, D-013): the rows of every open source and its maths outputs, the
// Channels-window search, sorting by value, single-channel edits as one SetOverrideCommand each (unit,
// min/max in the display unit stored as {v, unit}, DPS, colour, interpolation; refused input reported),
// "Clear override", the unit rule of item 6 at edit time (refused with the conflicting files), the Corr Dist
// unit, focusChannel, and the Aliases (circular references refused) and Mappings editors.

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "maths_editors_fixture.hpp"

#include <esses/widgets/editors_data/channel_editor.hpp>
#include <esses/widgets/editors_maths/channel_documents_dialogs.hpp>

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QSortFilterProxyModel>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTableWidget>

using namespace esses;
using namespace esses::widgets;
namespace mt = esses::widgets::mathstest;
using Col = ChannelEditor::Column;

namespace {

struct Sources {
    std::shared_ptr<const model::DataSource> a;
    std::shared_ptr<const model::DataSource> b;
};

// Two open files: both log the synthetic channels and "Oil Temp", a temperature in A and a pressure in B.
Sources openTwo(mt::Workspace& ws, const std::string& tag) {
    qt::test::SyntheticOptions oa;
    oa.path = tag + "-a.ld";
    const auto baseA = qt::test::makeSource(oa, ws.rt.cache());
    std::vector<float> temp(mt::sampleCount(*baseA), 363.15F);
    const auto a = mt::withChannel(*baseA, "Oil Temp", "K", temp, ws.rt.cache());
    qt::test::SyntheticOptions ob;
    ob.path = tag + "-b.ld";
    ob.lapSeconds = {40.0, 45.0, 30.0};
    const auto baseB = qt::test::makeSource(ob, ws.rt.cache());
    std::vector<float> pressure(mt::sampleCount(*baseB), 250000.0F);
    const auto b = mt::withChannel(*baseB, "Oil Temp", "Pa", pressure, ws.rt.cache());
    Sources s;
    s.a = ws.open(*a, tag + "-a");
    s.b = ws.open(*b, tag + "-b");
    return s;
}

// The proxy row of `channel` in the source named `file`.
int rowOf(ChannelEditor& e, const QString& channel, const QString& file = {}) {
    QAbstractItemModel* p = e.table().model();
    for (int r = 0; r < p->rowCount(); ++r) {
        if (p->index(r, Col::NameCol).data().toString() == channel &&
            (file.isEmpty() || p->index(r, Col::SourceCol).data().toString() == file)) {
            return r;
        }
    }
    return -1;
}

QString cell(ChannelEditor& e, const QString& channel, int column) {
    const int r = rowOf(e, channel);
    return r < 0 ? QString() : e.table().model()->index(r, column).data().toString();
}

bool setCell(ChannelEditor& e, const QString& channel, int column, const QVariant& value) {
    const int r = rowOf(e, channel);
    return r >= 0 && e.table().model()->setData(e.table().model()->index(r, column), value, Qt::EditRole);
}

} // namespace

class TstChannelEditor : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { mt::registerMetaTypes(); }

    void rowsSearchAndSorting() {
        mt::Workspace ws("ce-rows");
        openTwo(ws, "ce-rows");
        ChannelEditor editor(ws.session, ws.loader, &ws.maths);
        editor.show();
        QVERIFY(!editor.isModal());
        QAbstractItemModel& rows = editor.channelModel();
        QCOMPARE(rows.columnCount(), static_cast<int>(Col::ColumnCount));
        QCOMPARE(rows.headerData(Col::OverrideCol, Qt::Horizontal).toString(), QStringLiteral("Override"));
        QVERIFY(rowOf(editor, QStringLiteral("Throttle"), QStringLiteral("ce-rows-a.ld")) >= 0);
        QVERIFY(rowOf(editor, QStringLiteral("Throttle"), QStringLiteral("ce-rows-b.ld")) >= 0);
        QVERIFY(rowOf(editor, QStringLiteral("Oil Temp"), QStringLiteral("ce-rows-b.ld")) >= 0);
        // The maths outputs of each source are rows (Corr Speed / Corr Dist, built in).
        const int corrDist = rowOf(editor, QStringLiteral("Corr Dist"));
        QVERIFY(corrDist >= 0);
        QCOMPARE(editor.table().model()->index(corrDist, Col::CategoryCol).data().toString(),
                 QStringLiteral("Maths"));
        QCOMPARE(cell(editor, QStringLiteral("Throttle"), Col::CategoryCol), QStringLiteral("Logged"));
        QCOMPARE(cell(editor, QStringLiteral("Throttle"), Col::RateCol), QStringLiteral("50 Hz"));
        QCOMPARE(cell(editor, QStringLiteral("Throttle"), Col::UnitCol),
                 QStringLiteral("ratio")); // the stored unit
        QCOMPARE(cell(editor, QStringLiteral("Throttle"), Col::TypeCol), QStringLiteral("F32 Real"));
        QCOMPARE(cell(editor, QStringLiteral("Throttle"), Col::ColourCol), QStringLiteral("Auto"));

        // The Channels-window search: every word a prefix of a word of the name.
        editor.filterEdit().setText(QStringLiteral("wh sp"));
        QCOMPARE(editor.table().model()->rowCount(), 4); // Wheel Speed FL / FR of both files
        editor.filterEdit().setText(QStringLiteral("spe"));
        QVERIFY(rowOf(editor, QStringLiteral("Corr Speed")) >= 0);
        QVERIFY(rowOf(editor, QStringLiteral("Throttle")) < 0);
        editor.filterEdit().setText(QStringLiteral("eed")); // not a word prefix
        QCOMPARE(editor.table().model()->rowCount(), 0);
        editor.filterEdit().clear();

        // Sorting by value, not by text: colour 10 sorts after colour 2.
        QVERIFY(editor.setColourIndex(QStringLiteral("Throttle"), 10));
        QVERIFY(editor.setColourIndex(QStringLiteral("Wheel Speed FL"), 2));
        editor.table().sortByColumn(Col::ColourCol, Qt::AscendingOrder);
        QVERIFY(rowOf(editor, QStringLiteral("Wheel Speed FL")) < rowOf(editor, QStringLiteral("Throttle")));
        editor.table().sortByColumn(Col::NameCol, Qt::AscendingOrder);
        QVERIFY(rowOf(editor, QStringLiteral("Corr Dist")) < rowOf(editor, QStringLiteral("Throttle")));

        // focusChannel clears a filter that hides the row.
        editor.filterEdit().setText(QStringLiteral("zzz"));
        QVERIFY(editor.focusChannel(QStringLiteral("throttle")));
        QVERIFY(editor.filterEdit().text().isEmpty());
        QCOMPARE(editor.currentChannel(), QStringLiteral("Throttle"));
        QVERIFY(!editor.focusChannel(QStringLiteral("Nope")));
    }

    void editsAreUndoableOverrides() {
        mt::Workspace ws("ce-edit");
        openTwo(ws, "ce-edit");
        ChannelEditor editor(ws.session, ws.loader, &ws.maths);
        editor.show();
        QUndoStack& stack = ws.session.undoStack();
        int count = stack.count();
        const QString speed = QStringLiteral("Wheel Speed FL");
        // The unit cell edits through a combo of the quantity's units.
        const int r = rowOf(editor, speed);
        QStyleOptionViewItem option;
        QWidget* combo = editor.table().itemDelegate()->createEditor(
            editor.table().viewport(), option, editor.table().model()->index(r, Col::UnitCol));
        auto* box = qobject_cast<QComboBox*>(combo);
        QVERIFY(box != nullptr);
        QVERIFY(box->findText(QStringLiteral("km/h")) >= 0);
        QVERIFY(box->findText(QStringLiteral("%")) < 0);
        delete combo;
        QVERIFY(setCell(editor, speed, Col::UnitCol, QStringLiteral("km/h")));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(*ws.session.overrides().find("Wheel Speed FL")->displayUnit, std::string("km/h"));
        QCOMPARE(cell(editor, speed, Col::UnitCol), QStringLiteral("km/h"));
        QCOMPARE(cell(editor, speed, Col::OverrideCol), QStringLiteral("●"));
        // Limits in the display unit, stored as {v, unit}.
        QVERIFY(setCell(editor, speed, Col::MaxCol, QStringLiteral("300")));
        QCOMPARE(stack.count(), ++count);
        const auto& max = ws.session.overrides().find("Wheel Speed FL")->scaleMax;
        QVERIFY(max.has_value());
        QCOMPARE(max->v, 300.0);
        QCOMPARE(max->symbol, std::string("km/h"));
        QCOMPARE(cell(editor, speed, Col::MaxCol), QStringLiteral("300.00"));
        QVERIFY(!setCell(editor, speed, Col::MinCol, QStringLiteral("fast")));
        QVERIFY(editor.lastRefusal().contains(QStringLiteral("not a number")));
        QCOMPARE(stack.count(), count);
        // Back to m/s: the stored limit shows converted.
        QVERIFY(setCell(editor, speed, Col::UnitCol, QStringLiteral("m/s")));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(cell(editor, speed, Col::MaxCol), QStringLiteral("83.33"));
        // DPS, colour, interpolation.
        QVERIFY(setCell(editor, speed, Col::DpsCol, 3));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(*ws.session.overrides().find("Wheel Speed FL")->dps, 3);
        QCOMPARE(cell(editor, speed, Col::MaxCol), QStringLiteral("83.333"));
        QVERIFY(setCell(editor, speed, Col::ColourCol, 5));
        QCOMPARE(stack.count(), ++count);
        QVERIFY(setCell(editor, speed, Col::InterpCol, QStringLiteral("Closest")));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(*ws.session.overrides().find("Wheel Speed FL")->interp, std::string("Closest"));
        QString refusal;
        QVERIFY(!editor.setInterp(speed, QStringLiteral("Cubic"), &refusal));
        QVERIFY(refusal.contains(QStringLiteral("Cubic")));
        QVERIFY(!editor.setDisplayUnit(speed, QStringLiteral("nosuchunit"), &refusal));
        QCOMPARE(stack.count(), count);
        // Clear override: one step; undo brings the entry back.
        QVERIFY(editor.clearOverride(speed));
        QCOMPARE(stack.count(), ++count);
        QVERIFY(ws.session.overrides().find("Wheel Speed FL") == nullptr);
        QCOMPARE(cell(editor, speed, Col::OverrideCol), QString());
        QCOMPARE(cell(editor, speed, Col::UnitCol), QStringLiteral("m/s"));
        QVERIFY(!editor.clearOverride(speed));
        stack.undo();
        QCOMPARE(*ws.session.overrides().find("Wheel Speed FL")->dps, 3);
        QCOMPARE(cell(editor, speed, Col::DpsCol), QStringLiteral("3"));
    }

    // 06 section 13.1 item 6 at edit time, and the Corr Dist unit.
    void unitRuleAndCorrDist() {
        mt::Workspace ws("ce-unit");
        const Sources s = openTwo(ws, "ce-unit");
        ChannelEditor editor(ws.session, ws.loader, &ws.maths);
        editor.show();
        QSignalSpy refused(&editor, &ChannelEditor::editRefused);
        const int count = ws.session.undoStack().count();
        QString refusal;
        QVERIFY(!editor.setDisplayUnit(QStringLiteral("Oil Temp"), QStringLiteral("deg C"), &refusal));
        QVERIFY(refusal.contains(QStringLiteral("ce-unit-b.ld")));
        QVERIFY(!refusal.contains(QStringLiteral("ce-unit-a.ld")));
        QCOMPARE(refused.count(), 1);
        QCOMPARE(ws.session.undoStack().count(), count);
        QVERIFY(ws.session.overrides().find("Oil Temp") == nullptr);
        QVERIFY(!setCell(editor, QStringLiteral("Oil Temp"), Col::UnitCol, QStringLiteral("deg C")));
        // With the pressure file closed the unit applies.
        QSignalSpy closed(&ws.loader, &qt::SourceLoader::sourceClosed);
        ws.loader.close(s.b->id);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            rowOf(editor, QStringLiteral("Oil Temp"), QStringLiteral("ce-unit-b.ld")) < 0, 5000);
        QVERIFY(editor.setDisplayUnit(QStringLiteral("Oil Temp"), QStringLiteral("deg C")));
        QCOMPARE(cell(editor, QStringLiteral("Oil Temp"), Col::UnitCol), QStringLiteral("deg C"));
        QVERIFY(editor.lastRefusal().isEmpty());
        // Corr Dist: a maths output whose unit is editable here (the distance axis follows it).
        QCOMPARE(cell(editor, QStringLiteral("Corr Dist"), Col::UnitCol), QStringLiteral("m"));
        QVERIFY(editor.setDisplayUnit(QStringLiteral("Corr Dist"), QStringLiteral("km")));
        QCOMPARE(*ws.session.overrides().find("Corr Dist")->displayUnit, std::string("km"));
        QCOMPARE(cell(editor, QStringLiteral("Corr Dist"), Col::UnitCol), QStringLiteral("km"));
        QVERIFY(!editor.setDisplayUnit(QStringLiteral("Corr Dist"), QStringLiteral("km/h")));
    }

    // 06 section 13.1 item 7: the Aliases and Mappings editors.
    void aliasesAndMappingsEditors() {
        mt::Workspace ws("ce-docs");
        openTwo(ws, "ce-docs");
        ChannelEditor editor(ws.session, ws.loader, &ws.maths);
        editor.show();
        QUndoStack& stack = ws.session.undoStack();
        int count = stack.count();
        QVERIFY(editor.aliasesDialog() == nullptr);
        editor.openAliases();
        AliasesDialog* aliases = editor.aliasesDialog();
        QVERIFY(aliases != nullptr && aliases->isVisible() && !aliases->isModal());
        // Circular references are refused.
        project::Aliases cyclic;
        cyclic.table.rows.push_back(model::AliasRow{"A", {{"B", true}}, -1, std::nullopt});
        cyclic.table.rows.push_back(model::AliasRow{"B", {{"A", true}}, -1, std::nullopt});
        aliases->setDocument(cyclic);
        QString error;
        QVERIFY(!aliases->apply(&error));
        QVERIFY(error.contains(QStringLiteral("Circular")));
        QVERIFY(aliases->lastError().contains(QStringLiteral("Circular")));
        QCOMPARE(stack.count(), count);
        project::Aliases twice;
        twice.table.rows.push_back(model::AliasRow{"A", {{"Throttle", true}}, -1, std::nullopt});
        twice.table.rows.push_back(model::AliasRow{"a", {{"Throttle", true}}, -1, std::nullopt});
        aliases->setDocument(twice);
        QVERIFY(!aliases->apply(&error));
        // A valid document is one undo step; the candidate's enable box is edited in the list.
        project::Aliases speed;
        speed.table.rows.push_back(model::AliasRow{
            "Speed Any", {{"Wheel Speed FL", true}, {"Wheel Speed FR", true}}, -1, std::nullopt});
        aliases->setDocument(speed);
        QVERIFY(aliases->selectAlias(QStringLiteral("speed any")));
        QCOMPARE(aliases->candidateList().count(), 2);
        aliases->candidateList().item(1)->setCheckState(Qt::Unchecked);
        QVERIFY(aliases->apply(&error));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(ws.session.aliases().table.rows.size(), std::size_t{1});
        QVERIFY(!ws.session.aliases().table.rows.front().candidates[1].enabled);
        QVERIFY(!aliases->isDirty());
        // Undo reloads the clean dialog.
        stack.undo();
        QCOMPARE(aliases->document().table.rows.size(), std::size_t{0});
        const int index = stack.index();

        editor.openMappings();
        MappingsDialog* mappings = editor.mappingsDialog();
        QVERIFY(mappings != nullptr && mappings->isVisible());
        project::Mappings bad;
        bad.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::string("nosuchunit"), false});
        mappings->setDocument(bad);
        QVERIFY(!mappings->apply(&error));
        QVERIFY(error.contains(QStringLiteral("nosuchunit")));
        project::Mappings dup;
        dup.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
        dup.rows.push_back(project::Mappings::Row{"tps", "Pedal", std::nullopt, false});
        mappings->setDocument(dup);
        QVERIFY(!mappings->apply(&error));
        QVERIFY(error.contains(QStringLiteral("twice")));
        project::Mappings good;
        good.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::string("%"), true});
        mappings->setDocument(good);
        QCOMPARE(mappings->table().rowCount(), 1);
        QVERIFY(mappings->selectMapping(QStringLiteral("tps")));
        QVERIFY(mappings->apply(&error));
        QCOMPARE(stack.index(), index + 1);
        QCOMPARE(ws.session.mappings(), good);
    }
};

QTEST_MAIN(TstChannelEditor)
#include "tst_channel_editor.moc"
