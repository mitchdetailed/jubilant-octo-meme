// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M3-08 (06 section 12 item 7, test properties_roundtrip): the schema-driven PropertiesDialog round trip
// in the real shell. A project gets a worksheet with one component of each of the ten MVP types, twice: once
// with the full example body of schemas/examples/components/<type>-1/valid/full.json and once with an empty
// body. For each component, every leaf property of its body schema (schemas/components/<type>-1.schema.json,
// FieldBinder::schemaPointers) must be bound (an unbound property fails), is mutated through its bound editor
// (FieldBinder::editorFor: the leaf widget, inside a record list or an optional member where needed) to a
// value its schema node allows, the header fields too; the dialog is valid, Apply is one undo step that
// gives the component the edited spec, the project is saved and reopened, and the reopened component's body
// and header equal what the dialog showed. Also: opening and pressing Apply without an edit changes nothing,
// Cancel after Apply restores the spec at open time (one more undo step), undo while the dialog is open
// refreshes the pages, F5 again raises the same dialog, deleting the component closes it, CTRL+N opens it on
// the Channels tab at the active channel.

#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QListWidget>
#include <QSignalSpy>
#include <QTabWidget>
#include <QUndoStack>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

struct Shell {
    QTemporaryDir config;
    QTemporaryDir projects;
    std::unique_ptr<qt::AppRuntime> rt = runtime();
    std::unique_ptr<MainWindow> w;
    std::filesystem::path folder;

    bool start() {
        if (!config.isValid() || !projects.isValid()) {
            return false;
        }
        folder = fsPath(projects.path()) / "roundtrip";
        const Paths paths = pathsFor(config.path());
        if (!makeProject(paths, folder, "Round trip")) {
            return false;
        }
        w = std::make_unique<MainWindow>(*rt, paths);
        w->setSaveLayoutOnExit(false);
        if (!showAndActivate(*w)) {
            return false;
        }
        return w->openProject(folder);
    }
    ~Shell() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }

    // A worksheet "Properties <which>" with one component of every MVP type, shown.
    std::vector<Uuid> addWorksheet(const std::string& which) {
        qt::ProjectSession& s = w->session();
        project::WorksheetDef sheet;
        sheet.name = "Properties " + which;
        sheet.linkTo = "Laps";
        std::vector<Uuid> ids;
        int z = 0;
        for (const char* type : kMvpTypes) {
            const double l = 0.1 * (z % 5);
            const double t = z < 5 ? 0.0 : 0.5;
            project::ComponentSpec c =
                componentOf(type, exampleBody(type, which), project::Rect{l, t, l + 0.1, t + 0.5}, z);
            ids.push_back(c.id);
            sheet.components.push_back(std::move(c));
            ++z;
        }
        s.undoStack().push(new qt::AddWorksheetCommand(s, s.activeWorkbook(), sheet));
        w->selectWorksheet(w->worksheetCount() - 1);
        return ids;
    }

    const project::ComponentSpec* stored(const Uuid& id) const {
        for (const project::WorkbookFile& book : w->session().workbooks()) {
            for (const project::WorksheetDef& sheet : book.worksheets) {
                for (const project::ComponentSpec& c : sheet.components) {
                    if (c.id == id) {
                        return &c;
                    }
                }
            }
        }
        return nullptr;
    }
};

QString bodyText(const JsonDoc& body) {
    return QString::fromStdString(body.compact());
}

} // namespace

class TstPropertiesRoundtrip : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void cleanup() {
        // The dialogs of a test never outlive it.
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* d = qobject_cast<PropertiesDialog*>(top)) {
                delete d;
            }
        }
    }

    // Every leaf property of every MVP body schema has a binding (the coverage half of AC-M3-08).
    void everySchemaPropertyIsBound_data() {
        QTest::addColumn<QString>("type");
        for (const char* type : kMvpTypes) {
            QTest::newRow(type) << QString::fromLatin1(type);
        }
    }
    void everySchemaPropertyIsBound() {
        QFETCH(QString, type);
        const JsonDoc schema = PropertiesDialog::bodySchema(type);
        QVERIFY2(!schema.isNull(), qPrintable(QStringLiteral("no schema for ") + type));
        const QStringList leaves = FieldBinder::schemaPointers(schema);
        QVERIFY(leaves.size() >= 5);
        cmptest::World world;
        project::ComponentSpec spec = componentOf(type.toStdString(), exampleBody(type.toStdString()), {}, 0);
        std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *world.context));
        PropertiesDialog dialog(*c);
        for (const QString& p : leaves) {
            QVERIFY2(dialog.binder().isBound(p), qPrintable(type + QStringLiteral(": unbound ") + p));
            QVERIFY2(dialog.binder().editorFor(p) != nullptr,
                     qPrintable(type + QStringLiteral(": no editor for ") + p));
        }
        // Every bound pointer is a property of the schema (no row binds something the body cannot hold).
        const SchemaTree tree = SchemaTree::fromDoc(schema);
        for (const QString& p : dialog.binder().boundPointers()) {
            QVERIFY2(!tree.node(p).isEmpty(),
                     qPrintable(type + QStringLiteral(": bound outside the schema ") + p));
        }
        QVERIFY(!dialog.tabTitles().isEmpty());
        QVERIFY(dialog.tabTitles().contains(QStringLiteral("Display")));
    }

    // The round trip of AC-M3-08: mutate every leaf, Apply, save, reopen, identical.
    void roundTrip_data() {
        QTest::addColumn<QString>("which");
        QTest::newRow("full bodies") << QStringLiteral("full");
        QTest::newRow("empty bodies") << QStringLiteral("empty");
    }
    void roundTrip() {
        QFETCH(QString, which);
        Shell sh;
        QVERIFY(sh.start());
        const std::vector<Uuid> ids = sh.addWorksheet(which.toStdString());
        QCOMPARE(sh.w->canvas().components().size(), qsizetype{10});
        std::map<Uuid, project::ComponentSpec> expected;
        for (const Uuid& id : ids) {
            ComponentWidget* c = sh.w->canvas().componentById(id);
            QVERIFY(c != nullptr);
            const QString type = QString::fromLatin1(c->type());
            PropertiesDialog* dialog = PropertiesDialog::openFor(*c);
            QVERIFY(dialog != nullptr);
            QCOMPARE(PropertiesDialog::find(*c), dialog);
            const SchemaTree tree = SchemaTree::fromDoc(PropertiesDialog::bodySchema(type));
            for (const QString& p : FieldBinder::schemaPointers(PropertiesDialog::bodySchema(type))) {
                QWidget* editor = dialog->binder().editorFor(p);
                QVERIFY2(editor != nullptr, qPrintable(type + QStringLiteral(": no editor for ") + p));
                QVERIFY2(mutateLeaf(editor, tree.node(p)),
                         qPrintable(type + QStringLiteral(": cannot mutate ") + p + QStringLiteral(" (") +
                                    QString::fromLatin1(editor->metaObject()->className()) +
                                    QLatin1Char(')')));
            }
            for (const FieldBinding& b : dialog->headerBinder().bindings()) {
                QVERIFY(mutateLeaf(b.editor.data(), {}));
            }
            QVERIFY2(dialog->isValid(),
                     qPrintable(type + QStringLiteral(": ") + dialog->validationMessage()));
            const project::ComponentSpec edited = dialog->editedSpec();
            QVERIFY2(!(edited.body == c->spec().body),
                     qPrintable(type + QStringLiteral(": nothing changed")));
            QVERIFY2(tree.validate(tree.node(QString()), jsonptr::fromDoc(edited.body), nullptr),
                     qPrintable(type + QStringLiteral(": the edited body breaks its schema ") +
                                bodyText(edited.body)));
            const int undoBefore = sh.w->session().undoStack().index();
            dialog->apply();
            QCOMPARE(sh.w->session().undoStack().index(), undoBefore + 1); // one undo step per Apply
            QVERIFY(c->spec() == edited);
            QVERIFY(sh.stored(id) != nullptr && *sh.stored(id) == edited);
            expected.emplace(id, edited);
            dialog->accept(); // OK with nothing more to apply closes it
        }
        // Save, reopen, identical.
        QVERIFY(sh.w->session().save());
        QVERIFY(sh.w->openProject(sh.folder));
        for (const auto& [id, spec] : expected) {
            const project::ComponentSpec* back = sh.stored(id);
            QVERIFY(back != nullptr);
            QVERIFY2(back->body == spec.body,
                     qPrintable(QString::fromStdString(spec.type) + QStringLiteral(":\n") +
                                bodyText(back->body) + QStringLiteral("\n") + bodyText(spec.body)));
            QVERIFY(back->caption == spec.caption);
            QVERIFY(back->links == spec.links);
            QVERIFY(back->legend == spec.legend);
        }
    }

    // Apply without an edit changes nothing; Cancel after an Apply restores the spec at open time.
    void applyCancelAndUndo() {
        Shell sh;
        QVERIFY(sh.start());
        const std::vector<Uuid> ids = sh.addWorksheet("full");
        ComponentWidget* c = sh.w->canvas().componentById(ids.front()); // time_distance
        QVERIFY(c != nullptr);
        const project::ComponentSpec open = c->spec();
        QUndoStack& stack = sh.w->session().undoStack();
        PropertiesDialog* dialog = PropertiesDialog::openFor(*c);
        const int base = stack.index();
        dialog->apply();
        QCOMPARE(stack.index(), base); // nothing changed: no command
        QVERIFY(c->spec() == open);

        // F5 again raises the same dialog, at the requested tab.
        QCOMPARE(PropertiesDialog::openFor(*c, 1), dialog);
        QCOMPARE(dialog->tabs().currentIndex(), 1);

        auto* grid =
            qobject_cast<QCheckBox*>(dialog->binder().editorFor(QStringLiteral("/display/showGrid")));
        QVERIFY(grid != nullptr);
        grid->click();
        dialog->apply();
        QCOMPARE(stack.index(), base + 1);
        QVERIFY(!(c->spec() == open));

        // Undo while the dialog is open refreshes the pages (the canvas updates the same widget in place).
        stack.undo();
        QCOMPARE(sh.w->canvas().componentById(open.id), c); // the same widget
        QVERIFY(c->spec() == open);
        QCOMPARE(grid->isChecked(),
                 jsonptr::get(jsonptr::fromDoc(open.body), QStringLiteral("/display/showGrid")).toBool(true));
        stack.redo();
        QVERIFY(!(c->spec() == open));
        QCOMPARE(
            grid->isChecked(),
            !jsonptr::get(jsonptr::fromDoc(open.body), QStringLiteral("/display/showGrid")).toBool(true));

        // Cancel restores the spec at open time as one more command.
        dialog->reject();
        QCOMPARE(stack.index(), base + 2);
        QVERIFY(c->spec() == open);
        QTRY_VERIFY(PropertiesDialog::find(*c) == nullptr);
    }

    // Invalid fields: red with a reason, OK and Apply disabled, Apply does nothing.
    void inlineValidation() {
        cmptest::World world;
        project::ComponentSpec spec = componentOf("time_distance", exampleBody("time_distance"), {}, 0);
        std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *world.context));
        PropertiesDialog dialog(*c);
        QVERIFY(dialog.isValid());
        auto* width =
            qobject_cast<NumberEdit*>(dialog.binder().editorFor(QStringLiteral("/display/lineWidth")));
        QVERIFY(width != nullptr);
        width->setText(QStringLiteral("42")); // maximum 10
        QVERIFY(!dialog.isValid());
        QVERIFY(dialog.validationMessage().contains(QStringLiteral("at most 10")));
        QVERIFY(width->property("esses.invalid").toBool());
        QVERIFY(!width->toolTip().isEmpty());
        width->setText(QStringLiteral("abc"));
        QVERIFY(dialog.validationMessage().contains(QStringLiteral("number")));
        QSignalSpy edits(c.get(), &ComponentWidget::specEditRequested);
        dialog.apply();
        QCOMPARE(edits.count(), 0);
        width->setText(QStringLiteral("2.5"));
        QVERIFY(dialog.isValid());
        QVERIFY(!width->property("esses.invalid").toBool());
        dialog.apply();
        QCOMPARE(edits.count(), 1);
    }

    // The component goes: so does its dialog. CTRL+N: the Channels tab at the active channel.
    void tracksItsComponentAndChannels() {
        Shell sh;
        QVERIFY(sh.start());
        const std::vector<Uuid> ids = sh.addWorksheet("full");
        ComponentWidget* graph = sh.w->canvas().componentById(ids.front());
        QVERIFY(graph != nullptr);
        // The active channel: the second trace of the example's first group.
        const QJsonValue body = jsonptr::fromDoc(graph->spec().body);
        const QString channel = jsonptr::get(body, QStringLiteral("/groups/0/traces/1/channel")).toString();
        QVERIFY(!channel.isEmpty());
        project::ComponentSpec withActive = graph->spec();
        withActive.activeChannel = channel.toStdString();
        Q_EMIT graph->specEditRequested(withActive, QStringLiteral("Active channel"));
        sh.w->canvas().focusComponent(graph);
        QVERIFY(sh.w->commands().invoke(QStringLiteral("channel.properties")));
        PropertiesDialog* dialog = PropertiesDialog::find(*graph);
        QVERIFY(dialog != nullptr);
        QCOMPARE(dialog->tabs().currentIndex(), dialog->channelsTabIndex());
        auto* groups = qobject_cast<RecordListEditor*>(
            dialog->binder().binding(QStringLiteral("/groups"))->editor.data());
        QVERIFY(groups != nullptr);
        QCOMPARE(groups->currentIndex(), 0);
        RecordListEditor* traces = groups->itemEditor().findChild<RecordListEditor*>();
        QVERIFY(traces != nullptr);
        QCOMPARE(traces->currentIndex(), 1);

        // Delete the component: the dialog closes.
        const QPointer<PropertiesDialog> guard(dialog);
        sh.w->canvas().focusComponent(graph);
        QVERIFY(sh.w->deleteComponents());
        QTRY_VERIFY(guard.isNull());
    }
};

QTEST_MAIN(TstPropertiesRoundtrip)
#include "tst_properties_roundtrip.moc"
