// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M3-11 (06 section 4.3 item 10, 04 section 7.2 item 6; test component_clipboard): ComponentClipboard's
// MIME form (application/x-esses-components+json plus text/plain with the same JSON, schemaVersion 1, the
// source domain), placeForPaste (snapped to the target grid, one cell further per successive paste, wrapping,
// fresh ids, bodies untouched); and in the real shell: CTRL+C on a component of a time worksheet and CTRL+V
// on a distance worksheet of another workbook paste a component whose body (its domain-dependent settings and
// manual scales) is the copied one, at its rectangle snapped to 1 %, then one cell further on the next paste;
// CTRL+X is one undo step that removes the component and puts it on the clipboard, DELETE removes without a
// question and is undone, select-all + copy takes every component, and a locked layout refuses cut, paste
// and delete.

#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/properties_shell/component_clipboard.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUndoStack>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

bool onGrid(double v, int cells) {
    return std::abs(v * cells - std::round(v * cells)) < 1e-9;
}

// A graph body with domain-dependent settings: a manual scale and variance limits in seconds.
JsonDoc domainBody() {
    return JsonDoc::parse(
               R"({"display":{"variance":{"show":true,"autoScale":false,"min":{"v":-0.5,"unit":"s"},)"
               R"("max":{"v":0.5,"unit":"s"}}},"groups":[{"name":"Speed","scaleMode":"manual",)"
               R"("scaleMin":{"v":0,"unit":"km/h"},"scaleMax":{"v":300,"unit":"km/h"},)"
               R"("traces":[{"channel":"Corr Speed"}]}]})")
        .value();
}

} // namespace

class TstComponentClipboard : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void mimeRoundTrip() {
        ComponentClipboard::Payload p;
        p.sourceDomain = QStringLiteral("distance");
        p.components.push_back(
            componentOf("time_distance", domainBody(), project::Rect{0.1, 0.2, 0.6, 0.7}, 3));
        p.components.push_back(
            componentOf("values_list", exampleBody("values_list"), project::Rect{0.6, 0, 1, 1}, 4));
        const std::unique_ptr<QMimeData> mime = ComponentClipboard::toMimeData(p);
        QVERIFY(mime->hasFormat(QString::fromLatin1(ComponentClipboard::kMimeType)));
        QVERIFY(mime->hasText());
        QCOMPARE(mime->text().toUtf8(), mime->data(QString::fromLatin1(ComponentClipboard::kMimeType)));
        const QJsonObject json = QJsonDocument::fromJson(mime->text().toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(json.value(QStringLiteral("sourceDomain")).toString(), QStringLiteral("distance"));
        QCOMPARE(json.value(QStringLiteral("components")).toArray().size(), 2);
        const std::optional<ComponentClipboard::Payload> back = ComponentClipboard::fromMimeData(*mime);
        QVERIFY(back.has_value());
        QCOMPARE(back->sourceDomain, QStringLiteral("distance"));
        QCOMPARE(back->components.size(), qsizetype{2});
        QVERIFY(back->components[0] == p.components[0]);
        QVERIFY(back->components[1] == p.components[1]);
        // text/plain alone (another application kept only the text), and garbage.
        QMimeData textOnly;
        textOnly.setText(mime->text());
        QVERIFY(ComponentClipboard::fromMimeData(textOnly).has_value());
        QMimeData junk;
        junk.setText(QStringLiteral("not a payload"));
        QVERIFY(!ComponentClipboard::fromMimeData(junk).has_value());
        QMimeData empty;
        empty.setText(QStringLiteral(R"({"schemaVersion":1,"components":[]})"));
        QVERIFY(!ComponentClipboard::fromMimeData(empty).has_value());
    }

    void placeForPasteSnapsAndOffsets() {
        ComponentClipboard::Payload p;
        p.components.push_back(
            componentOf("time_distance", domainBody(), project::Rect{0.1234, 0.2, 0.5678, 0.7}, 0));
        const QList<project::ComponentSpec> first = ComponentClipboard::placeForPaste(p, 0, QSize(100, 100));
        QCOMPARE(first.size(), qsizetype{1});
        const project::Rect r = first[0].rect;
        QVERIFY(onGrid(r.l, 100) && onGrid(r.t, 100) && onGrid(r.r, 100) && onGrid(r.b, 100));
        QCOMPARE(std::lround(r.l * 100), 12L);
        QCOMPARE(std::lround(r.r * 100), 56L);          // 44 cells wide (0.4444 of the width)
        QVERIFY(first[0].body == p.components[0].body); // bodies untouched
        QVERIFY(first[0].id != p.components[0].id);     // fresh ids
        const QList<project::ComponentSpec> second = ComponentClipboard::placeForPaste(p, 1, QSize(100, 100));
        QCOMPARE(std::lround(second[0].rect.l * 100), 13L);
        QCOMPARE(std::lround(second[0].rect.t * 100), 21L);
        QCOMPARE(std::lround((second[0].rect.r - second[0].rect.l) * 100), 44L); // size kept
        QVERIFY(second[0].id != first[0].id);
        // Wrapping inside the worksheet: never past the right or bottom edge.
        for (int k = 0; k < 120; ++k) {
            const project::Rect q = ComponentClipboard::placeForPaste(p, k, QSize(100, 100))[0].rect;
            QVERIFY(q.l >= 0.0 && q.r <= 1.0 + 1e-12 && q.t >= 0.0 && q.b <= 1.0 + 1e-12);
        }
        // Another grid.
        const project::Rect g = ComponentClipboard::placeForPaste(p, 0, QSize(20, 50))[0].rect;
        QVERIFY(onGrid(g.l, 20) && onGrid(g.r, 20) && onGrid(g.t, 50) && onGrid(g.b, 50));
    }

    // AC-M3-11 in the shell: copy in a time worksheet, paste in a distance worksheet of another workbook.
    void acrossDomains() {
        QTemporaryDir config;
        QTemporaryDir projects;
        QVERIFY(config.isValid() && projects.isValid());
        const Paths paths = pathsFor(config.path());
        const std::filesystem::path folder = fsPath(projects.path()) / "clip";
        QVERIFY(makeProject(paths, folder, "Clipboard"));
        auto rt = runtime();
        {
            MainWindow w(*rt, paths);
            w.setSaveLayoutOnExit(false);
            QVERIFY(showAndActivate(w));
            QVERIFY(w.openProject(folder));
            qt::ProjectSession& s = w.session();
            QUndoStack& stack = s.undoStack();
            // Workbook 0: a time worksheet with the graph; workbook 1: a distance worksheet.
            project::WorksheetDef timeSheet;
            timeSheet.name = "Time sheet";
            timeSheet.domain = "time";
            timeSheet.components.push_back(
                componentOf("time_distance", domainBody(), project::Rect{0.1234, 0.2, 0.5678, 0.7}, 0));
            timeSheet.components.push_back(
                componentOf("values_list", exampleBody("values_list"), project::Rect{0.6, 0, 1, 1}, 1));
            const Uuid graphId = timeSheet.components[0].id;
            stack.push(new qt::AddWorksheetCommand(s, 0, timeSheet));
            project::WorkbookFile other;
            other.name = "Other";
            project::WorksheetDef distanceSheet;
            distanceSheet.id = uuidRandom();
            distanceSheet.name = "Distance sheet";
            distanceSheet.domain = "distance";
            other.worksheets.push_back(distanceSheet);
            stack.push(new qt::AddWorkbookCommand(s, other));
            w.selectWorksheet(w.worksheetCount() - 1);
            ComponentWidget* graph = w.canvas().componentById(graphId);
            QVERIFY(graph != nullptr);
            w.canvas().focusComponent(graph);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_C, Qt::ControlModifier);
            const std::optional<ComponentClipboard::Payload> clip =
                ComponentClipboard::fromMimeData(*QGuiApplication::clipboard()->mimeData());
            QVERIFY(clip.has_value());
            QCOMPARE(clip->components.size(), qsizetype{1});
            QCOMPARE(clip->sourceDomain, QStringLiteral("time"));

            w.selectWorkbook(1);
            QCOMPARE(w.activeWorksheet()->domain(), plot::Domain::Distance);
            QVERIFY(w.canvas().components().isEmpty());
            w.canvas().setFocus(Qt::OtherFocusReason);
            const int before = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_V, Qt::ControlModifier);
            QCOMPARE(stack.index(), before + 1); // one undo step
            QCOMPARE(w.canvas().components().size(), qsizetype{1});
            const project::ComponentSpec pasted = w.canvas().components().front()->spec();
            QVERIFY(pasted.id != graphId);
            QVERIFY(pasted.body == domainBody()); // its own domain-dependent settings and manual scales
            QVERIFY(onGrid(pasted.rect.l, 100) && onGrid(pasted.rect.t, 100) && onGrid(pasted.rect.r, 100) &&
                    onGrid(pasted.rect.b, 100));
            QCOMPARE(std::lround(pasted.rect.l * 100), 12L);
            QVERIFY(w.canvas().isSelected(w.canvas().components().front())); // the pasted one is selected
            // The next paste into the same worksheet: one cell further.
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_V, Qt::ControlModifier);
            QCOMPARE(w.canvas().components().size(), qsizetype{2});
            const project::ComponentSpec second = w.canvas().components().back()->spec();
            QCOMPARE(std::lround(second.rect.l * 100), 13L);
            QCOMPARE(std::lround(second.rect.t * 100), 21L);
            stack.undo();
            stack.undo();
            QVERIFY(w.canvas().components().isEmpty());

            // CTRL+X: one undo step; the component leaves and is on the clipboard.
            w.selectWorkbook(0);
            w.selectWorksheet(w.worksheetCount() - 1);
            graph = w.canvas().componentById(graphId);
            QVERIFY(graph != nullptr);
            w.canvas().focusComponent(graph);
            QGuiApplication::clipboard()->clear();
            const int cutBase = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_X, Qt::ControlModifier);
            QCOMPARE(stack.index(), cutBase + 1);
            QVERIFY(w.canvas().componentById(graphId) == nullptr);
            QVERIFY(ComponentClipboard::fromMimeData(*QGuiApplication::clipboard()->mimeData()).has_value());
            stack.undo();
            QVERIFY(w.canvas().componentById(graphId) != nullptr);

            // DELETE: no question, undoable.
            w.canvas().focusComponent(w.canvas().componentById(graphId));
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Delete);
            QVERIFY(w.canvas().componentById(graphId) == nullptr);
            stack.undo();
            QVERIFY(w.canvas().componentById(graphId) != nullptr);

            // Select all + copy: both components.
            w.canvas().selectAll();
            QVERIFY(w.copyComponents());
            QCOMPARE(ComponentClipboard::fromMimeData(*QGuiApplication::clipboard()->mimeData())
                         ->components.size(),
                     qsizetype{2});

            // A locked layout refuses cut, paste and delete.
            project::ProjectFile locked = s.projectFile();
            locked.layoutLocked = true;
            stack.push(new qt::SetProjectOptionsCommand(s, locked, QStringLiteral("Lock")));
            QVERIFY(w.canvas().layoutLocked());
            const int lockedBase = stack.index();
            QVERIFY(!w.cutComponents());
            QVERIFY(!w.pasteComponents());
            QVERIFY(!w.deleteComponents());
            QCOMPARE(stack.index(), lockedBase);
            QVERIFY(w.lastHint().contains(QStringLiteral("locked")));
            QVERIFY(w.copyComponents()); // copying stays allowed
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstComponentClipboard)
#include "tst_component_clipboard.moc"
