// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsEditor (03 section 11, 06 section 13.2; the Qt Test rows of 03 section 11): live diagnostics after the
// 150 ms debounce with Errors red, Warnings amber and Info grey and the spans underlined; a dimensioned
// reference without [unit] shows W-UNIT-MISSING in amber and a warning never blocks Apply; an Error cannot be
// applied, switched away from or closed (ESC included), and Revert offers the way out; editing without data
// (I-UNRESOLVED); the [unit] popup lists the quantity's units, filters while typing, ENTER inserts and ESC
// closes it only; Insert Range inserts a quoted group path (also through its menu) and Insert Channel
// 'Name'[unit]; hover help shows the precedence rule with the unaryMinusAbovePower setting and a function's
// signature, also through the tooltip event; Apply and every structural edit are one undo step; module add /
// delete / move / enable / condition / import / export; copy, cut and paste keep names; local maths writes
// the sidecar; transient maths is listed read-only; the status column follows MathsService; auto-save on
// close writes the module file only after a module edit.

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "maths_editors_fixture.hpp"

#include <esses/maths/math_module_io.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <fstream>
#include <sstream>

using namespace esses;
using namespace esses::widgets;
namespace mt = esses::widgets::mathstest;

namespace {

QString level(MathsEditor& e) {
    return e.diagnosticsLine().property("esses.level").toString();
}

void setCaretToEnd(QPlainTextEdit& edit) {
    QTextCursor c = edit.textCursor();
    c.movePosition(QTextCursor::End);
    edit.setTextCursor(c);
}

const maths::MathModule* moduleNamed(const qt::ProjectSession& s, const std::string& id) {
    for (const maths::MathModule& m : s.mathsModules()) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

int rowNamed(QTableWidget& t, const QString& name) {
    for (int r = 0; r < t.rowCount(); ++r) {
        if (t.item(r, 2) != nullptr && t.item(r, 2)->text() == name) {
            return r;
        }
    }
    return -1;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool treeHas(QTreeWidget& tree, const QString& text) {
    for (QTreeWidgetItemIterator it(&tree); *it != nullptr; ++it) {
        if ((*it)->text(0) == text) {
            return true;
        }
    }
    return false;
}

} // namespace

class TstMathsEditor : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { mt::registerMetaTypes(); }

    // 03 section 11 item 2 and the rows "a dimensioned reference without [unit] shows W-UNIT-MISSING in
    // amber" and "warning does not block Apply".
    void liveDiagnosticsAndWarningsDoNotBlock() {
        mt::Workspace ws("me-live");
        ws.setModules({mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2")})});
        ws.openSynthetic("me-live");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(!editor.isModal());
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("Twice")));
        QCOMPARE(editor.currentItemName(), QStringLiteral("Twice"));
        QCOMPARE(editor.expressionEdit().toPlainText(), QStringLiteral("'Throttle'[%] * 2"));
        QCOMPARE(level(editor), QStringLiteral("ok"));

        // An unknown channel with data loaded is an Error, shown after the debounce only.
        editor.expressionEdit().setPlainText(QStringLiteral("'Nope' * 2"));
        QVERIFY(editor.isPageDirty());
        QCOMPARE(level(editor), QStringLiteral("ok")); // the 150 ms debounce has not expired yet
        QTRY_COMPARE_WITH_TIMEOUT(level(editor), QStringLiteral("error"), 5000);
        QVERIFY(editor.diagnosticsLine().text().contains(QStringLiteral("E-UNKNOWN-CHANNEL")));
        QCOMPARE(editor.diagnosticsLine().palette().color(QPalette::WindowText), QColor(0xD3, 0x2F, 0x2F));
        QVERIFY(maths::hasError(editor.currentDiagnostics()));
        const QList<QTextEdit::ExtraSelection> marks = editor.expressionEdit().extraSelections();
        QVERIFY(!marks.isEmpty());
        QCOMPARE(marks.front().format.underlineStyle(), QTextCharFormat::WaveUnderline);
        QCOMPARE(marks.front().cursor.selectedText(), QStringLiteral("'Nope'"));
        QVERIFY(editor.diagnosticsList().count() >= 1);

        // W-UNIT-MISSING in amber; a warning never blocks Apply.
        const int before = ws.session.undoStack().count();
        editor.expressionEdit().setPlainText(QStringLiteral("'Wheel Speed FL' * 2"));
        editor.flushDiagnostics();
        QCOMPARE(level(editor), QStringLiteral("warning"));
        QVERIFY(editor.diagnosticsLine().text().contains(QStringLiteral("W-UNIT-MISSING")));
        QCOMPARE(editor.diagnosticsLine().palette().color(QPalette::WindowText), QColor(0xC7, 0x7C, 0x02));
        QVERIFY(editor.canClose());
        QVERIFY(editor.apply());
        QVERIFY(!editor.isPageDirty());
        QCOMPARE(ws.session.undoStack().count(), before + 1);
        QCOMPARE(moduleNamed(ws.session, "user")->items.front().expr, std::string("'Wheel Speed FL' * 2"));
        // Apply is project undo: the expression comes back with the page.
        ws.session.undoStack().undo();
        QCOMPARE(editor.expressionEdit().toPlainText(), QStringLiteral("'Throttle'[%] * 2"));
        QVERIFY(!editor.isPageDirty());
    }

    // 03 section 11 item 3: "cannot close with error", Revert, no apply and no switch while invalid.
    void cannotCloseWithErrorOffersRevert() {
        mt::Workspace ws("me-close");
        ws.setModules(
            {mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2"), mt::item("Other", "1 + 1")})});
        ws.openSynthetic("me-close");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("Twice")));
        const int before = ws.session.undoStack().count();
        editor.expressionEdit().setPlainText(QStringLiteral("2 * ("));
        QString reason;
        QVERIFY(!editor.canClose(&reason));
        QVERIFY(reason.contains(QStringLiteral("E-")));
        QVERIFY(!editor.close());
        QVERIFY(editor.isVisible());
        QVERIFY(editor.refusalText().contains(QStringLiteral("cannot close")));
        // ESC closes like the close button: refused as well.
        QTest::keyClick(&editor.expressionEdit(), Qt::Key_Escape);
        QVERIFY(editor.isVisible());
        // Neither applied nor left.
        QVERIFY(!editor.apply());
        QVERIFY(editor.refusalText().contains(QStringLiteral("cannot be applied")));
        QVERIFY(!editor.openAt(QStringLiteral("user"), QStringLiteral("Other")));
        const int otherRow = rowNamed(editor.itemsTable(), QStringLiteral("Other"));
        QVERIFY(otherRow >= 0);
        editor.itemsTable().setCurrentCell(otherRow, 2);
        QCOMPARE(editor.currentItemName(), QStringLiteral("Twice"));
        QCOMPARE(editor.expressionEdit().toPlainText(), QStringLiteral("2 * ("));
        QCOMPARE(ws.session.undoStack().count(), before);
        // Revert, then the dialog closes.
        editor.revert();
        QVERIFY(!editor.isPageDirty());
        QCOMPARE(editor.expressionEdit().toPlainText(), QStringLiteral("'Throttle'[%] * 2"));
        QVERIFY(editor.canClose());
        QVERIFY(editor.close());
        QVERIFY(!editor.isVisible());
        QCOMPARE(ws.session.undoStack().count(), before);
    }

    // Item switch applies a valid edit (03 section 11 item 9); the name field refuses the quote.
    void itemSwitchAppliesTheEdit() {
        mt::Workspace ws("me-switch");
        ws.setModules({mt::module("user", {mt::item("A", "1"), mt::item("B", "2")})});
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("A")));
        editor.expressionEdit().setPlainText(QStringLiteral("10 * 2"));
        QTest::keyClicks(&editor.nameEdit(), QStringLiteral("x'y"));
        QCOMPARE(editor.nameEdit().text(), QStringLiteral("Axy")); // a name never contains '
        const int b = rowNamed(editor.itemsTable(), QStringLiteral("B"));
        editor.itemsTable().setCurrentCell(b, 2);
        QCOMPARE(editor.currentItemName(), QStringLiteral("B"));
        const maths::MathModule* m = moduleNamed(ws.session, "user");
        QCOMPARE(m->items.front().name, std::string("Axy"));
        QCOMPARE(m->items.front().expr, std::string("10 * 2"));
        // The page's own diagnostics: an empty name is an Error.
        editor.nameEdit().clear();
        QTest::keyClick(&editor.nameEdit(), Qt::Key_Space);
        QTest::keyClick(&editor.nameEdit(), Qt::Key_Backspace);
        QVERIFY(!editor.canClose());
        bool emptyName = false;
        for (const maths::Diagnostic& d : editor.currentDiagnostics()) {
            emptyName = emptyName || d.code == "E-NAME-EMPTY";
        }
        QVERIFY(emptyName);
        editor.revert();
        QVERIFY(editor.canClose());
    }

    // 03 section 11 item 12, 06 section 13.2 item 5: editing without data; unknown names are I-UNRESOLVED.
    void editingWithoutDataIsAllowed() {
        mt::Workspace ws("me-nodata");
        ws.setModules({mt::module("user", {mt::item("X", "1")})});
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("X")));
        QVERIFY(!editor.mainSource().has_value());
        editor.expressionEdit().setPlainText(QStringLiteral("'Nope' * 2"));
        editor.flushDiagnostics();
        QCOMPARE(level(editor), QStringLiteral("info"));
        QVERIFY(editor.diagnosticsLine().text().contains(QStringLiteral("I-UNRESOLVED")));
        QVERIFY(editor.canClose());
        QVERIFY(editor.apply());
        QCOMPARE(moduleNamed(ws.session, "user")->items.front().expr, std::string("'Nope' * 2"));
    }

    // 03 section 11 item 5: "[unit] popup lists quantity units".
    void unitPopupListsQuantityUnits() {
        mt::Workspace ws("me-units");
        maths::MathModule user = mt::module("user", {mt::item("Twice", "1")});
        maths::MathConstant k;
        k.name = "Track Length";
        k.value.v = 2000.0;
        k.value.unit = *qt::test::shippedUnits()->find("m");
        user.constants.push_back(k);
        ws.setModules({user});
        ws.openSynthetic("me-units");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("Twice")));
        QPlainTextEdit& e = editor.expressionEdit();
        e.setFocus();
        e.setPlainText(QStringLiteral("'Wheel Speed FL'["));
        setCaretToEnd(e);
        QVERIFY(editor.unitPopupVisible());
        const QStringList speed = editor.unitPopupEntries();
        QVERIFY(speed.contains(QStringLiteral("m/s")));
        QVERIFY(speed.contains(QStringLiteral("km/h")));
        QVERIFY(speed.contains(QStringLiteral("mph")));
        QVERIFY(!speed.contains(QStringLiteral("%")));
        QCOMPARE(speed, editor.unitsAt(static_cast<int>(e.toPlainText().size())));
        // Typing filters; ENTER inserts the symbol and the closing bracket.
        QTest::keyClicks(&e, QStringLiteral("k"));
        QCOMPARE(editor.unitPopupEntries(), QStringList{QStringLiteral("km/h")});
        QTest::keyClick(&e, Qt::Key_Return);
        QCOMPARE(e.toPlainText(), QStringLiteral("'Wheel Speed FL'[km/h]"));
        QVERIFY(!editor.unitPopupVisible());
        QCOMPARE(e.textCursor().position(), static_cast<int>(e.toPlainText().size()));
        // Inside existing brackets the content is replaced; arrows choose.
        e.setPlainText(QStringLiteral("'Throttle'[] * 2"));
        QTextCursor c = e.textCursor();
        c.setPosition(11);
        e.setTextCursor(c);
        QVERIFY(editor.unitPopupVisible());
        QVERIFY(editor.unitPopupEntries().contains(QStringLiteral("%")));
        QVERIFY(editor.unitPopupEntries().contains(QStringLiteral("ratio")));
        QVERIFY(!editor.unitPopupEntries().contains(QStringLiteral("Trim"))); // compat units are not offered
        const int percent = static_cast<int>(editor.unitPopupEntries().indexOf(QStringLiteral("%")));
        for (int i = 0; i < percent; ++i) {
            QTest::keyClick(&e, Qt::Key_Down);
        }
        QVERIFY(editor.acceptUnitPopup());
        QCOMPARE(e.toPlainText(), QStringLiteral("'Throttle'[%] * 2"));
        // A constant has the units of its quantity; ESC closes the popup and not the dialog.
        e.setPlainText(QStringLiteral("'Track Length' ["));
        setCaretToEnd(e);
        QVERIFY(editor.unitPopupVisible());
        QVERIFY(editor.unitPopupEntries().contains(QStringLiteral("km")));
        QTest::keyClick(&e, Qt::Key_Escape);
        QVERIFY(!editor.unitPopupVisible());
        QVERIFY(editor.isVisible());
        // No popup outside the brackets of a reference.
        QVERIFY(editor.unitsAt(0).isEmpty());
        e.setPlainText(QStringLiteral("smooth("));
        setCaretToEnd(e);
        QVERIFY(!editor.unitPopupVisible());
        e.setPlainText(QStringLiteral("'Nope'["));
        setCaretToEnd(e);
        QVERIFY(!editor.unitPopupVisible());
        editor.revert();
    }

    // 03 section 11 item 6: "Insert Range inserts quoted path"; Insert Channel inserts 'Name'[unit].
    void insertRangeAndChannel() {
        mt::Workspace ws("me-range");
        ws.setModules({mt::module("user", {mt::item("Twice", "1")})});
        ws.openSynthetic("me-range");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("Twice")));
        const QStringList paths = editor.rangePaths();
        QVERIFY(paths.contains(QStringLiteral("Outings")));
        QVERIFY(paths.contains(QStringLiteral("Outings:Laps")));
        QPlainTextEdit& e = editor.expressionEdit();
        e.setPlainText(QStringLiteral("range_is()"));
        QTextCursor c = e.textCursor();
        c.setPosition(9);
        e.setTextCursor(c);
        editor.insertRange(QStringLiteral("Outings:Laps"));
        QCOMPARE(e.toPlainText(), QStringLiteral("range_is(\"Outings:Laps\")"));
        // Through the button's menu.
        QToolButton* button = nullptr;
        for (QToolButton* b : editor.findChildren<QToolButton*>()) {
            if (b->text() == QStringLiteral("Insert Range")) {
                button = b;
            }
        }
        QVERIFY(button != nullptr && button->menu() != nullptr);
        e.setPlainText(QString());
        Q_EMIT button->menu()->aboutToShow();
        QVERIFY(!button->menu()->actions().isEmpty());
        for (QAction* a : button->menu()->actions()) {
            if (a->text() == QStringLiteral("Outings")) {
                a->trigger();
            }
        }
        QCOMPARE(e.toPlainText(), QStringLiteral("\"Outings\""));
        // Insert Channel: the stored unit of a dimensioned channel.
        QVERIFY(editor.channelNames().contains(QStringLiteral("Wheel Speed FL")));
        QVERIFY(editor.channelNames().contains(QStringLiteral("Corr Speed")));
        e.setPlainText(QString());
        editor.insertChannel(QStringLiteral("Wheel Speed FL"));
        QCOMPARE(e.toPlainText(), QStringLiteral("'Wheel Speed FL'[m/s]"));
        editor.revert();
    }

    // 03 section 11 item 7 and D-020: the precedence rule and the compat switch on hover; a function's help.
    void hoverHelpShowsPrecedenceAndCompat() {
        mt::Workspace ws("me-hover");
        ws.setModules({mt::module("user", {mt::item("H", "-2^2 + smooth('Throttle'[%], 5)")})});
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("H")));
        const QString text = editor.expressionEdit().toPlainText();
        const int caret = static_cast<int>(text.indexOf(QLatin1Char('^')));
        QString help = editor.hoverText(caret);
        QVERIFY(help.contains(QStringLiteral("unaryMinusAbovePower is off")));
        QVERIFY(help.contains(QStringLiteral("-2^2 = -4")));
        QVERIFY(editor.hoverText(static_cast<int>(text.indexOf(QLatin1Char('+'))))
                    .contains(QStringLiteral("precedence")));
        maths::MathsCompat compat;
        compat.unaryMinusAbovePower = true;
        ws.maths.setCompat(compat);
        help = editor.hoverText(caret);
        QVERIFY(help.contains(QStringLiteral("unaryMinusAbovePower is on")));
        QVERIFY(help.contains(QStringLiteral("-2^2 = 4")));
        const QString fn = editor.hoverText(static_cast<int>(text.indexOf(QStringLiteral("smooth"))) + 2);
        QVERIFY(fn.startsWith(QStringLiteral("smooth(")));
        QVERIFY(editor.hoverText(static_cast<int>(text.indexOf(QStringLiteral("Throttle")))).isEmpty());
        // The tooltip event of the expression field shows the same text.
        QPlainTextEdit& e = editor.expressionEdit();
        QTextCursor c = e.textCursor();
        c.setPosition(caret);
        const QRect r = e.cursorRect(c);
        const QPoint at = r.center() + QPoint(2, 0);
        QHelpEvent event(QEvent::ToolTip, at, e.viewport()->mapToGlobal(at));
        QApplication::sendEvent(e.viewport(), &event);
        QTRY_VERIFY_WITH_TIMEOUT(QToolTip::isVisible(), 3000);
        QVERIFY(QToolTip::text().contains(QStringLiteral("precedence")));
        QToolTip::hideText();
    }

    // Structural edits: modules (add, enable, condition, move, delete, import/export) and items (add,
    // constant, enable, copy/cut/paste with names kept), one undo step each.
    void moduleAndItemOperations() {
        mt::Workspace ws("me-ops");
        ws.setModules({mt::module("a", {mt::item("X", "1"), mt::item("Y", "2")})});
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QUndoStack& stack = ws.session.undoStack();
        const int start = stack.index();
        int count = stack.count();
        QVERIFY(!editor.addModule(QStringLiteral("local"))); // reserved
        QVERIFY(!editor.addModule(QStringLiteral("~x")));    // reserved
        QVERIFY(!editor.addModule(QStringLiteral("a/b")));   // not a file name
        QVERIFY(editor.addModule(QStringLiteral("b")));
        QCOMPARE(stack.count(), ++count);
        QVERIFY(!editor.addModule(QStringLiteral("B"))); // ids are unique ignoring case
        QCOMPARE(editor.currentModule()->moduleId, QStringLiteral("b"));
        QVERIFY(treeHas(editor.modulesTree(), QStringLiteral("b")));
        QVERIFY(treeHas(editor.modulesTree(), QStringLiteral("Local maths")));
        QVERIFY(editor.setModuleEnabled(QStringLiteral("b"), false));
        QCOMPARE(stack.count(), ++count);
        QVERIFY(!moduleNamed(ws.session, "b")->enabled);
        QVERIFY(editor.setModuleCondition(QStringLiteral("b"), QStringLiteral("'Vehicle Id' == \"GT3-07\"")));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(*moduleNamed(ws.session, "b")->condition, std::string("'Vehicle Id' == \"GT3-07\""));
        QVERIFY(editor.moveModule(QStringLiteral("b"), -1));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(ws.session.mathsModules().front().id, std::string("b"));
        QVERIFY(!editor.moveModule(QStringLiteral("b"), -1));
        // A new item exists once applied; the page starts dirty and a new name is unique.
        QVERIFY(editor.addItem(maths::ItemKind::Expression));
        QVERIFY(editor.isPageDirty());
        QCOMPARE(editor.currentItemName(), QStringLiteral("New Expression"));
        editor.expressionEdit().setPlainText(QStringLiteral("3 * 4"));
        QVERIFY(editor.apply());
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(moduleNamed(ws.session, "b")->items.size(), std::size_t{1});
        QVERIFY(editor.addConstant());
        auto* value = qobject_cast<QLineEdit*>(editor.field(QStringLiteral("constValue")));
        auto* unit = qobject_cast<QComboBox*>(editor.field(QStringLiteral("constUnit")));
        QVERIFY(value != nullptr && unit != nullptr);
        value->setText(QStringLiteral("2.5"));
        unit->setEditText(QStringLiteral("nosuchunit"));
        QVERIFY(!editor.apply());
        unit->setEditText(QStringLiteral("km"));
        QVERIFY(editor.apply());
        QCOMPARE(stack.count(), ++count);
        const maths::MathConstant& kc = moduleNamed(ws.session, "b")->constants.front();
        QCOMPARE(kc.name, std::string("New Constant"));
        QCOMPARE(kc.value.v, 2.5);
        QCOMPARE(kc.value.unit, *qt::test::shippedUnits()->find("km"));
        // A Filter and a Scale & Offset item.
        QVERIFY(editor.addItem(maths::ItemKind::Filter));
        editor.nameEdit().setText(QStringLiteral("Throttle"));
        qobject_cast<QComboBox*>(editor.field(QStringLiteral("filterMode")))->setCurrentIndex(1);
        qobject_cast<QDoubleSpinBox*>(editor.field(QStringLiteral("filterValue")))->setValue(0.25);
        QVERIFY(editor.apply());
        QCOMPARE(stack.count(), ++count);
        const maths::MathItem& filter = moduleNamed(ws.session, "b")->items.back();
        QCOMPARE(filter.kind, maths::ItemKind::Filter);
        QCOMPARE(filter.name, std::string("Throttle"));
        QCOMPARE(*filter.window.seconds, 0.25);
        QVERIFY(!filter.window.points.has_value());

        // Copy two items of "a" and paste them into "b": the names are kept.
        QVERIFY(editor.openAt(QStringLiteral("a"), QStringLiteral("X")));
        editor.itemsTable().selectAll();
        QVERIFY(editor.copySelected());
        QVERIFY(QApplication::clipboard()->mimeData()->hasFormat(
            QString::fromLatin1(MathsEditor::kItemsMimeType)));
        QVERIFY(editor.openAt(QStringLiteral("b"), QString()));
        QVERIFY(editor.paste());
        QCOMPARE(stack.count(), ++count);
        const maths::MathModule* b = moduleNamed(ws.session, "b");
        QCOMPARE(b->items.size(), std::size_t{4});
        QCOMPARE(b->items[2].name, std::string("X"));
        QCOMPARE(b->items[3].name, std::string("Y"));
        // Cut from "a" removes both; disabling items is one step.
        QVERIFY(editor.openAt(QStringLiteral("a"), QStringLiteral("X")));
        editor.itemsTable().selectAll();
        QVERIFY(editor.cutSelected());
        QCOMPARE(stack.count(), ++count);
        QVERIFY(moduleNamed(ws.session, "a")->items.empty());
        QVERIFY(editor.openAt(QStringLiteral("b"), QStringLiteral("X")));
        editor.itemsTable().selectAll();
        QVERIFY(editor.setSelectedEnabled(false));
        QCOMPARE(stack.count(), ++count);
        for (const maths::MathItem& i : moduleNamed(ws.session, "b")->items) {
            QVERIFY(!i.enabled);
        }
        // The enable box of a row is one step.
        QTableWidgetItem* box = editor.itemsTable().item(0, 0);
        box->setCheckState(Qt::Checked);
        QCOMPARE(stack.count(), ++count);
        QVERIFY(moduleNamed(ws.session, "b")->items.front().enabled);
        // Delete the selected row.
        editor.itemsTable().clearSelection();
        editor.itemsTable().setCurrentCell(0, 2);
        editor.itemsTable().selectRow(0);
        const std::size_t items = moduleNamed(ws.session, "b")->items.size();
        QVERIFY(editor.removeSelected());
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(moduleNamed(ws.session, "b")->items.size(), items - 1);

        // Export and import (a copy under a new id).
        const std::filesystem::path file = ws.dir / "b.maths.json";
        QString error;
        QVERIFY2(editor.exportModule(QStringLiteral("b"), file, &error), qPrintable(error));
        QVERIFY(std::filesystem::exists(file));
        QVERIFY2(editor.importModule(file, &error), qPrintable(error));
        QCOMPARE(stack.count(), ++count);
        QCOMPARE(editor.currentModule()->moduleId, QStringLiteral("b-2"));
        QCOMPARE(moduleNamed(ws.session, "b-2")->items, moduleNamed(ws.session, "b")->items);
        QVERIFY(!editor.importModule(ws.dir / "missing.maths.json", &error));
        QVERIFY(!error.isEmpty());
        // Delete a module; undo brings everything back step by step.
        QVERIFY(editor.removeModule(QStringLiteral("b-2")));
        QCOMPARE(stack.count(), ++count);
        QVERIFY(moduleNamed(ws.session, "b-2") == nullptr);
        stack.setIndex(start);
        QCOMPARE(ws.session.mathsModules().size(), std::size_t{1});
        QCOMPARE(ws.session.mathsModules().front().items.size(), std::size_t{2});
    }

    // Local maths (sidecar localMaths) and the transient node (read-only).
    void localAndTransientMaths() {
        mt::Workspace ws("me-local");
        const auto src = ws.openSynthetic("me-local");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(treeHas(editor.modulesTree(), QStringLiteral("me-local.ld")));
        MathsEditor::ModuleRef local;
        local.scope = MathsEditor::ModuleRef::Scope::Local;
        local.source = src->id;
        QVERIFY(editor.openAt(local, QString()));
        QVERIFY(editor.addItem(maths::ItemKind::Expression));
        editor.nameEdit().setText(QStringLiteral("Local Double"));
        editor.expressionEdit().setPlainText(QStringLiteral("'Throttle'[%] * 2"));
        const int before = ws.session.undoStack().count();
        QVERIFY(editor.apply());
        QCOMPARE(ws.session.undoStack().count(), before + 1);
        const auto now = ws.loader.source(src->id);
        const std::optional<JsonDoc>& doc = now->sidecar.layer(model::kNormalLayer).localMaths;
        QVERIFY(doc.has_value());
        QVERIFY(doc->compact().find("Local Double") != std::string::npos);
        QVERIFY(ws.maths.isMathsChannel(src->id, "Normal", "Local Double"));
        QCOMPARE(editor.currentItemName(), QStringLiteral("Local Double"));
        QVERIFY(editor.openAt(QStringLiteral("local"), QStringLiteral("Local Double")));
        // Undo removes it from the sidecar again.
        ws.session.undoStack().undo();
        QVERIFY(!ws.maths.isMathsChannel(src->id, "Normal", "Local Double"));

        // Transient maths: listed under "Transient", never editable here.
        maths::MathModule transient;
        maths::MathItem f;
        f.kind = maths::ItemKind::Filter;
        f.name = "Throttle";
        f.window.points = 5;
        transient.items.push_back(f);
        ws.maths.setTransientModule(src->id, transient);
        QTRY_VERIFY_WITH_TIMEOUT(treeHas(editor.modulesTree(), QStringLiteral("Transient")), 5000);
        QVERIFY(editor.openAt(QStringLiteral("~transient"), QStringLiteral("Throttle")));
        QCOMPARE(editor.currentModule()->scope, MathsEditor::ModuleRef::Scope::Transient);
        QVERIFY(!editor.addItem(maths::ItemKind::Expression));
        QVERIFY(!editor.nameEdit().isEnabled());
    }

    // The status column and the page's status line follow MathsService; Recalculate makes them Pending.
    void statusFollowsTheService() {
        mt::Workspace ws("me-status");
        ws.setModules(
            {mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2"), mt::item("Bad", "'Nope'")})});
        const auto src = ws.openSynthetic("me-status");
        MathsEditor editor(ws.session, ws.maths, &ws.loader);
        editor.show();
        QVERIFY(editor.openAt(QStringLiteral("user"), QStringLiteral("Twice")));
        const int twice = rowNamed(editor.itemsTable(), QStringLiteral("Twice"));
        const int bad = rowNamed(editor.itemsTable(), QStringLiteral("Bad"));
        static_cast<void>(
            ws.maths.require(maths::ChannelKey::make(src->id, "Normal", "Twice"), Priority::Visible));
        QTRY_COMPARE_WITH_TIMEOUT(editor.itemsTable().item(twice, 5)->text(), QStringLiteral("OK"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(editor.statusLine().text(), QStringLiteral("Status: OK"), 5000);
        QVERIFY(editor.itemsTable().item(bad, 5)->text().startsWith(QStringLiteral("Error")));
        editor.recalculate();
        QTRY_VERIFY_WITH_TIMEOUT(editor.itemsTable().item(twice, 5)->text() != QStringLiteral("OK"), 5000);
    }

    // Auto-save on close (D-019): the module file after a module edit, nothing without one.
    void autoSaveOnClose() {
        mt::Workspace ws("me-save", true);
        const std::filesystem::path file = ws.dir / "Project" / "maths" / "user.maths.json";
        {
            MathsEditor editor(ws.session, ws.maths, &ws.loader);
            editor.show();
            QVERIFY(editor.addModule(QStringLiteral("user")));
            QVERIFY(editor.addItem(maths::ItemKind::Expression));
            editor.nameEdit().setText(QStringLiteral("Doubled"));
            editor.expressionEdit().setPlainText(QStringLiteral("2 * 3"));
            // Closing applies the valid page, then saves.
            QVERIFY(!std::filesystem::exists(file));
            QVERIFY(editor.close());
            QVERIFY(std::filesystem::exists(file));
            QVERIFY(readFile(file).find("Doubled") != std::string::npos);
            QVERIFY(readFile(ws.dir / "Project" / "project.json").find("\"user\"") != std::string::npos);
            QVERIFY(!ws.session.isDirty());
        }
        // Another document edit is not saved by a close without module edits.
        project::ChannelOverrides::Entry e;
        e.dps = 4;
        ws.session.undoStack().push(new qt::SetOverrideCommand(ws.session, "Throttle", e));
        QVERIFY(ws.session.isDirty());
        {
            MathsEditor editor(ws.session, ws.maths, &ws.loader);
            editor.show();
            QVERIFY(editor.close());
        }
        QVERIFY(ws.session.isDirty());
        // The loaded file reads back.
        std::vector<std::string> log;
        Result<maths::MathModule> back = maths::MathModuleIo::load(file, *qt::test::shippedUnits(), log);
        QVERIFY(back.ok());
        QCOMPARE(back->items.front().expr, std::string("2 * 3"));
    }

    // D-190 (amends D-177 (a)): closing after a module edit saves the modules and their order only; an edit
    // of another document made before stays unsaved and undoable.
    void closeSavesOnlyTheModules() {
        mt::Workspace ws("me-save-only", true);
        const std::filesystem::path project = ws.dir / "Project";
        project::ChannelOverrides::Entry e;
        e.dps = 3;
        ws.session.undoStack().push(new qt::SetOverrideCommand(ws.session, "Throttle", e));
        {
            MathsEditor editor(ws.session, ws.maths, &ws.loader);
            editor.show();
            QVERIFY(editor.addModule(QStringLiteral("only")));
            QVERIFY(editor.addItem(maths::ItemKind::Expression));
            editor.nameEdit().setText(QStringLiteral("Tripled"));
            editor.expressionEdit().setPlainText(QStringLiteral("3 * 3"));
            QVERIFY(editor.close());
        }
        QVERIFY(std::filesystem::exists(project / "maths" / "only.maths.json"));
        QVERIFY(readFile(project / "project.json").find("\"only\"") != std::string::npos);
        QVERIFY(!std::filesystem::exists(project / "channels" / "overrides.json"));
        QVERIFY(ws.session.isDirty());
        QVERIFY(ws.session.overrides().channels.count("Throttle") == 1);
        // The override edit is still on the stack below the module edits.
        while (ws.session.undoStack().canUndo()) {
            ws.session.undoStack().undo();
        }
        QVERIFY(ws.session.overrides().channels.count("Throttle") == 0);
    }
};

QTEST_MAIN(TstMathsEditor)
#include "tst_maths_editor.moc"
