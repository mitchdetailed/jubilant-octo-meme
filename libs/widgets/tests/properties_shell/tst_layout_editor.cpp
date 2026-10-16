// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayoutEditor (06 section 4.2, MVP basics) over a ProjectSession with an open project: the tree of workbooks
// and worksheets with the active workbook preselected; Move Up/Down (worksheets and workbooks), Copy To (a
// fresh worksheet id and fresh component ids, components, linkTo and domain kept), Move To (one undo step,
// never the last worksheet), Rename (inline through the tree too; a duplicate refused with the reason, the
// old name back), Delete (the confirmation names the component count; never the last worksheet or workbook),
// Duplicate ("<name> (2)"), New worksheet (name, data view, domain) and New workbook, Import and Export of a
// workbook file, Lock Layout (project.json ui.layoutLocked); every change one undo step, the tree following
// the session (also after undo).

#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>

#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QUndoStack>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

struct Project {
    QTemporaryDir config;
    QTemporaryDir projects;
    std::filesystem::path folder;
    std::unique_ptr<qt::ProjectSession> session;
    bool start() {
        if (!config.isValid() || !projects.isValid()) {
            return false;
        }
        const Paths paths = pathsFor(config.path());
        folder = fsPath(projects.path()) / "layout";
        if (!makeProject(paths, folder, "Layout")) {
            return false;
        }
        session = std::make_unique<qt::ProjectSession>(paths);
        if (!session->loadDefaults().ok() || !session->openProject(folder).ok()) {
            return false;
        }
        // A second workbook to copy and move to.
        project::WorkbookFile other;
        other.name = "Other";
        project::WorksheetDef sheet;
        sheet.name = "Only";
        other.worksheets.push_back(sheet);
        session->undoStack().push(new qt::AddWorkbookCommand(*session, other));
        session->undoStack().clear();
        return session->workbooks().size() == 2;
    }
    const project::WorkbookFile& book(int i) const {
        return session->workbooks()[static_cast<std::size_t>(i)];
    }
};

QStringList sheetNames(const project::WorkbookFile& b) {
    QStringList out;
    for (const project::WorksheetDef& s : b.worksheets) {
        out << QString::fromStdString(s.name);
    }
    return out;
}

} // namespace

class TstLayoutEditor : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void treeMovesAndRenames() {
        Project p;
        QVERIFY(p.start());
        LayoutEditor editor(*p.session);
        QCOMPARE(editor.tree().topLevelItemCount(), 2);
        QCOMPARE(editor.current(), (std::pair<int, int>{0, -1})); // the active workbook
        QUndoStack& stack = p.session->undoStack();
        const QStringList original = sheetNames(p.book(0));
        QVERIFY(original.size() >= 2);
        editor.setCurrent(0, 0);
        QVERIFY(editor.moveDown());
        QCOMPARE(stack.index(), 1);
        QCOMPARE(sheetNames(p.book(0))[1], original[0]);
        QCOMPARE(editor.current(), (std::pair<int, int>{0, 1})); // the moved one stays highlighted
        QVERIFY(editor.moveUp());
        QCOMPARE(sheetNames(p.book(0)), original);
        QVERIFY(!editor.moveUp()); // already first
        // Workbooks move too.
        editor.setCurrent(1);
        QVERIFY(editor.moveUp());
        QCOMPARE(QString::fromStdString(p.book(0).name), QStringLiteral("Other"));
        stack.undo();
        QCOMPARE(QString::fromStdString(p.book(1).name), QStringLiteral("Other"));
        // Rename: a duplicate is refused with the reason; through the tree the old name comes back.
        editor.setCurrent(0, 0);
        QVERIFY(!editor.rename(original[1]));
        QVERIFY(editor.lastRefusal().contains(original[1]));
        const int before = stack.index();
        QVERIFY(editor.rename(QStringLiteral("Renamed")));
        QCOMPARE(stack.index(), before + 1);
        QCOMPARE(QString::fromStdString(p.book(0).worksheets[0].name), QStringLiteral("Renamed"));
        QTreeWidgetItem* item = editor.tree().topLevelItem(0)->child(1);
        item->setText(0, QStringLiteral("Renamed")); // inline, a duplicate
        QTRY_COMPARE(editor.tree().topLevelItem(0)->child(1)->text(0), original[1]);
        QCOMPARE(QString::fromStdString(p.book(0).worksheets[1].name), original[1]);
        editor.tree().topLevelItem(0)->child(1)->setText(0, QStringLiteral("Inline"));
        QTRY_COMPARE(QString::fromStdString(p.book(0).worksheets[1].name), QStringLiteral("Inline"));
        // A workbook name is a file name.
        editor.setCurrent(1);
        QVERIFY(!editor.rename(QStringLiteral("a/b")));
        QVERIFY(editor.rename(QStringLiteral("Renamed Book")));
        QCOMPARE(QString::fromStdString(p.book(1).name), QStringLiteral("Renamed Book"));
    }

    void copyMoveDuplicateDelete() {
        Project p;
        QVERIFY(p.start());
        LayoutEditor editor(*p.session);
        QUndoStack& stack = p.session->undoStack();
        int confirmations = 0;
        QString lastQuestion;
        LayoutEditor::Prompts prompts;
        prompts.choose = [](const QString&, const QStringList&) { return std::optional<int>(0); };
        prompts.confirm = [&](const QString& q) {
            ++confirmations;
            lastQuestion = q;
            return true;
        };
        editor.setPrompts(prompts);
        // Give the first worksheet two components.
        project::WorkbookFile b0 = p.book(0);
        b0.worksheets[0].components.push_back(componentOf("values_list", exampleBody("values_list"), {}, 0));
        b0.worksheets[0].components.push_back(
            componentOf("gauge_numeric", exampleBody("gauge_numeric"), {}, 1));
        b0.worksheets[0].domain = "distance";
        stack.push(new qt::WorkbookCommand(*p.session, 0, b0, QStringLiteral("Setup")));
        const project::WorksheetDef source = p.book(0).worksheets[0];
        // Copy To the other workbook: fresh ids, the rest kept.
        editor.setCurrent(0, 0);
        int base = stack.index();
        QVERIFY(editor.copyTo());
        QCOMPARE(stack.index(), base + 1);
        const project::WorksheetDef& copy = p.book(1).worksheets.back();
        QVERIFY(copy.id != source.id);
        QCOMPARE(copy.components.size(), source.components.size());
        QVERIFY(copy.components[0].id != source.components[0].id);
        QVERIFY(copy.components[0].body == source.components[0].body);
        QCOMPARE(copy.domain, std::string("distance"));
        QCOMPARE(copy.linkTo, source.linkTo);
        // Duplicate: "<name> (2)" after it.
        editor.setCurrent(0, 0);
        QVERIFY(editor.duplicate());
        QCOMPARE(QString::fromStdString(p.book(0).worksheets[1].name),
                 QString::fromStdString(source.name) + QStringLiteral(" (2)"));
        QVERIFY(p.book(0).worksheets[1].id != source.id);
        // Move To: one undo step; the last worksheet of a workbook is refused.
        const std::size_t sheets0 = p.book(0).worksheets.size();
        const std::size_t sheets1 = p.book(1).worksheets.size();
        editor.setCurrent(0, 1);
        base = stack.index();
        QVERIFY(editor.moveTo());
        QCOMPARE(stack.index(), base + 1);
        QCOMPARE(p.book(0).worksheets.size(), sheets0 - 1);
        QCOMPARE(p.book(1).worksheets.size(), sheets1 + 1);
        stack.undo();
        QCOMPARE(p.book(0).worksheets.size(), sheets0);
        QCOMPARE(p.book(1).worksheets.size(), sheets1);
        // Delete: the question names the component count; the last worksheet and the last workbook stay.
        editor.setCurrent(0, 0);
        QVERIFY(editor.remove());
        QVERIFY(lastQuestion.contains(QStringLiteral("%1 component").arg(source.components.size())));
        QCOMPARE(p.book(0).worksheets.size(), sheets0 - 1);
        stack.undo();
        editor.setCurrent(1);
        QVERIFY(editor.remove()); // a workbook
        QCOMPARE(p.session->workbooks().size(), std::size_t{1});
        editor.setCurrent(0);
        QVERIFY(!editor.remove());
        QVERIFY(editor.lastRefusal().contains(QStringLiteral("last workbook")));
        stack.undo();
        QCOMPARE(p.session->workbooks().size(), std::size_t{2});
        QCOMPARE(editor.tree().topLevelItemCount(), 2); // the tree follows the session
    }

    void newImportExportLock() {
        Project p;
        QVERIFY(p.start());
        LayoutEditor editor(*p.session);
        QUndoStack& stack = p.session->undoStack();
        QStringList texts{QStringLiteral("Brakes"), QStringLiteral("Imported Book")};
        LayoutEditor::Prompts prompts;
        prompts.text = [&](const QString&, const QString&, const QString&) -> std::optional<QString> {
            return texts.isEmpty() ? std::nullopt : std::optional<QString>(texts.takeFirst());
        };
        prompts.choose = [](const QString&, const QStringList& items) {
            return std::optional<int>(static_cast<int>(items.size()) - 1); // the last data view; Distance
        };
        const QString exported = QString::fromStdWString((p.folder / "exported.workbook.json").wstring());
        prompts.file = [&](const QString&, bool) { return exported; };
        editor.setPrompts(prompts);
        editor.setCurrent(0, 0);
        QVERIFY(editor.newWorksheet());
        const project::WorksheetDef& added = p.book(0).worksheets[1];
        QCOMPARE(added.name, std::string("Brakes"));
        QCOMPARE(added.domain, std::string("distance"));
        QVERIFY(!added.linkTo.empty());
        QVERIFY(editor.newWorkbook());
        QCOMPARE(p.session->workbooks().back().name, std::string("Imported Book"));
        QCOMPARE(p.session->workbooks().back().worksheets.size(), std::size_t{1});
        // Export the first workbook and import it again: a second copy with its own id.
        editor.setCurrent(0);
        QVERIFY(editor.exportWorkbook());
        QVERIFY(std::filesystem::exists(p.folder / "exported.workbook.json"));
        const std::size_t books = p.session->workbooks().size();
        QSignalSpy messages(&editor, &LayoutEditor::message);
        QVERIFY(editor.importWorkbook());
        QCOMPARE(p.session->workbooks().size(), books + 1);
        QVERIFY(p.session->workbooks().back().id != p.book(0).id);
        QCOMPARE(sheetNames(p.session->workbooks().back()), sheetNames(p.book(0)));
        // The copy has a name of its own: a second workbook may not share the file name (D-203 (p)).
        QVERIFY(p.session->workbooks().back().name != p.book(0).name);
        QVERIFY(!messages.isEmpty());
        // A name with path and reserved characters becomes a safe file name, and the user is told.
        {
            QFile f(exported);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            o.insert(QStringLiteral("name"), QStringLiteral("../evil:book"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(o).toJson());
        }
        const int told = static_cast<int>(messages.count());
        QVERIFY(editor.importWorkbook());
        const std::string imported = p.session->workbooks().back().name;
        QCOMPARE(imported, std::string(".._evil_book"));
        QVERIFY(messages.count() > told);
        // Lock Layout: project.json ui.layoutLocked, one undo step.
        const int base = stack.index();
        editor.lockBox().setChecked(true);
        QVERIFY(p.session->projectFile().layoutLocked);
        QCOMPARE(stack.index(), base + 1);
        stack.undo();
        QVERIFY(!p.session->projectFile().layoutLocked);
        QVERIFY(!editor.lockBox().isChecked());
    }
};

QTEST_MAIN(TstLayoutEditor)
#include "tst_layout_editor.moc"
