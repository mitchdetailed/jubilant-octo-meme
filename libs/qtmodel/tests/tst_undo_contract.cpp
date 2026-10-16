// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M2 undo contract (D-032, D-161; 02 section 7): ProjectSession owns a QUndoStack that works in
// esses_qtmodel under a QCoreApplication (no GUI application needed, D-161), document commands write the
// documents exactly back and forth, consecutive geometry edits of one component merge, and worksheet moves
// round-trip. The AC-M2-09 property test (30 randomised sequences) belongs to the owner of src/undo/.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <memory>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

} // namespace

class TstUndoContract : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void undoStackWorksWithoutAGuiApplication() {
        QVERIFY(QCoreApplication::instance() != nullptr);
        const auto dir = scratchDir("undo-core");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.undoStack().count() == 0);

        project::ChannelOverrides::Entry rpm;
        rpm.displayUnit = "rpm";
        rpm.dps = 0;
        QSignalSpy docs(&session, &ProjectSession::documentsChanged);
        session.undoStack().push(new SetOverrideCommand(session, "Engine RPM", rpm));
        QCOMPARE(docs.count(), 1);
        QVERIFY(session.overrides().find("engine rpm") != nullptr);
        QVERIFY(session.isDirty());

        session.undoStack().push(new ClearOverrideCommand(session, "Engine RPM"));
        QVERIFY(session.overrides().find("Engine RPM") == nullptr);
        session.undoStack().undo();
        QVERIFY(session.overrides().find("Engine RPM") != nullptr);
        session.undoStack().undo();
        QVERIFY(session.overrides().channels.empty());
        session.undoStack().redo();
        QCOMPARE(session.overrides().find("Engine RPM")->dps, std::optional<int>(0));
        QCOMPARE(session.undoStack().undoText(), QStringLiteral("Edit channel Engine RPM"));
    }

    void workbookCommandsMergeAndMoveWorksheets() {
        const auto dir = scratchDir("undo-workbook");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        const project::WorkbookFile original = session.workbooks().front();
        QVERIFY(original.worksheets.size() >= 2); // the shipped workbook: M1's two worksheets and M3's five

        session.undoStack().push(new MoveWorksheetCommand(session, 0, 0, 1));
        QCOMPARE(session.workbooks().front().worksheets[1].name, original.worksheets[0].name);
        session.undoStack().undo();
        QVERIFY(session.workbooks().front() == original);

        // Two geometry edits of one component merge into one command (06 section 4.3 item 11).
        project::WorkbookFile a = original;
        a.worksheets[0].components[0].rect.r = 0.5;
        project::WorkbookFile b = a;
        b.worksheets[0].components[0].rect.r = 0.6;
        const std::string cid = original.worksheets[0].components[0].id.str();
        session.undoStack().push(new WorkbookCommand(session, 0, a, QStringLiteral("Resize component"),
                                                     UndoMergeId::ComponentGeometry, cid));
        session.undoStack().push(new WorkbookCommand(session, 0, b, QStringLiteral("Resize component"),
                                                     UndoMergeId::ComponentGeometry, cid));
        QCOMPARE(session.undoStack().count(), 1); // the undone move was dropped; the two resizes merged
        QVERIFY(session.workbooks().front() == b);
        session.undoStack().undo();
        QVERIFY(session.workbooks().front() == original);
    }
};

QTEST_GUILESS_MAIN(TstUndoContract)
#include "tst_undo_contract.moc"
