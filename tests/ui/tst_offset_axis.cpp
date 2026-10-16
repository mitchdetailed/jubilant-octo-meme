// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M3-09 (07 section 6.2; 04 section 3.3, 06 section 24 item 5, D-138), offscreen, in the real shell
// (MainWindow on a private configuration directory, the shipped workbook) over a written synthetic log at 50
// Hz and its sidecar: O shows the offset axis; CTRL+SHIFT+RIGHT moves Main by one offset step, one sample
// period of the active channel in Main's source (20,000 us), CTRL+SHIFT+UP by ten, ALT+SHIFT+LEFT the
// reference overlay (Overlay 0) by one step back, each burst committed as one undoable sidecar edit; in
// distance mode a step is 0.25 m; the amounts appear in the Data window ("+0.220 s", "-0.020 s", "+0.25 m");
// O again zeroes every selected range's offsets as one undo command ("Clear alignment offsets"), which undoes
// as one; the stored offsetUs / offsetM are written to the log's sidecar and survive closing and reopening
// the log, and selecting a range with a stored offset turns the offset axis on (item 5). The sub-axis drag
// that commits once at the release is ui.tst_keymap's offset.drag row and widgets.tst_time_distance_m3
// offsetAxisDragAndStep.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/base/thread_role.hpp>
#include <esses/model/range.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <cmath>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kTimeoutMs = 60000;
constexpr Micros kStepUs = 20'000; // one sample period at 50 Hz

QString offsetCell(MainWindow& w, const model::RangeRef& r) {
    const QModelIndex i = w.dataWindow().model().indexOf(r);
    return i.isValid() ? i.siblingAtColumn(qt::DataWindowModel::OffsetCol).data().toString() : QString();
}

const model::Range* rangeOf(MainWindow& w, const model::RangeRef& r) {
    return w.selection().resolve(r);
}

Micros offsetUsOf(MainWindow& w, const model::RangeRef& r) {
    const model::Range* range = rangeOf(w, r);
    return range == nullptr ? -1 : range->offsetUs;
}

double offsetMOf(MainWindow& w, const model::RangeRef& r) {
    const model::Range* range = rangeOf(w, r);
    return range == nullptr ? -1.0 : range->offsetM;
}

void focusGraph(MainWindow& w) {
    TimeDistanceGraph* g = uitest::graphOf(w);
    QVERIFY(g != nullptr);
    w.canvas().focusComponent(g);
    g->setFocus(Qt::OtherFocusReason);
}

} // namespace

class TstOffsetAxis : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void cleanup() { QCOMPARE(uitest::assertFailures(), 0); }

    void stepsCommitShowZeroAndSurviveReopen() {
        QTemporaryDir config;
        QTemporaryDir logs;
        QVERIFY(config.isValid() && logs.isValid());
        auto rt = uitest::makeRuntime();
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
            qt::test::SyntheticOptions o;
            o.path = "offsets.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            ld = qt::test::writeLd(*qt::test::makeSource(o, rt->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "offsets");
        }
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(uitest::showAndActivate(w));
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, kTimeoutMs);
            const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src);
            QCOMPARE(laps.size(), std::size_t{5});
            const model::RangeRef& mainLap = laps[2];
            const model::RangeRef& refLap = laps[3];
            w.selection().setMain(mainLap);
            w.selection().toggleOverlay0(refLap);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            QCOMPARE(ws.domain(), plot::Domain::Time);
            QUndoStack& stack = w.session().undoStack();
            focusGraph(w);
            QVERIFY(ws.samplePeriodOf(src) == kStepUs);

            // O shows the axis; the nudges commit once each burst (250 ms after the last key).
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
            QVERIFY(ws.state().offsetAxis);
            int base = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right,
                            Qt::ControlModifier | Qt::ShiftModifier);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier);
            QVERIFY(!ws.pendingOffsets().empty()); // the traces move at once, before the commit
            QTRY_COMPARE_WITH_TIMEOUT(offsetUsOf(w, mainLap), 11 * kStepUs, 5000);
            QCOMPARE(stack.index(), base + 1); // one undoable sidecar edit for the burst
            base = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Left, Qt::AltModifier | Qt::ShiftModifier);
            QTRY_COMPARE_WITH_TIMEOUT(offsetUsOf(w, refLap), -kStepUs, 5000);
            QCOMPARE(stack.index(), base + 1);
            QCOMPARE(offsetUsOf(w, mainLap), 11 * kStepUs);
            // The Data window shows the amounts (04 section 3.3 item 6).
            QTRY_COMPARE(offsetCell(w, mainLap), QStringLiteral("+0.220 s"));
            QCOMPARE(offsetCell(w, refLap), QStringLiteral("-0.020 s"));

            // O off: every selected range's offset back to zero as one undo command.
            focusGraph(w);
            base = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
            QVERIFY(!ws.state().offsetAxis);
            QTRY_VERIFY_WITH_TIMEOUT(offsetUsOf(w, mainLap) == 0 && offsetUsOf(w, refLap) == 0, 5000);
            QCOMPARE(stack.index(), base + 1);
            QCOMPARE(stack.text(stack.index() - 1), QStringLiteral("Clear alignment offsets"));
            QCOMPARE(offsetCell(w, mainLap), QString());
            stack.undo(); // both come back as one step
            QTRY_VERIFY_WITH_TIMEOUT(
                offsetUsOf(w, mainLap) == 11 * kStepUs && offsetUsOf(w, refLap) == -kStepUs, 5000);

            // Distance mode: one step is the DistanceIndex step, 0.25 m.
            ws.setOffsetAxis(true);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_F9);
            QTRY_COMPARE(ws.domain(), plot::Domain::Distance);
            focusGraph(w);
            base = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right,
                            Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(offsetMOf(w, mainLap) - 0.25) < 1e-9, 5000);
            QCOMPARE(stack.index(), base + 1);
            QCOMPARE(offsetUsOf(w, mainLap), 11 * kStepUs); // the time offset is kept
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right, Qt::AltModifier | Qt::ShiftModifier);
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(offsetMOf(w, refLap) - 0.25) < 1e-9, 5000);

            // Close the log (its sidecar is written first) and open it again: the stored offsets are back,
            // and selecting the lap shows the axis.
            focusGraph(w);
            base = stack.index();
            QTest::keyClick(QApplication::focusWidget(),
                            Qt::Key_O); // zeroes all four offsets (one command) ...
            QVERIFY(!ws.state().offsetAxis);
            QTRY_VERIFY_WITH_TIMEOUT(offsetUsOf(w, mainLap) == 0 && offsetMOf(w, mainLap) == 0.0 &&
                                         offsetUsOf(w, refLap) == 0 && offsetMOf(w, refLap) == 0.0,
                                     5000);
            QCOMPARE(stack.index(), base + 1);
            stack.undo(); // ... and back as one step
            QTRY_VERIFY_WITH_TIMEOUT(
                offsetUsOf(w, mainLap) == 11 * kStepUs && std::abs(offsetMOf(w, mainLap) - 0.25) < 1e-9 &&
                    offsetUsOf(w, refLap) == -kStepUs && std::abs(offsetMOf(w, refLap) - 0.25) < 1e-9,
                5000);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_F9);
            QTRY_COMPARE(ws.domain(), plot::Domain::Time);
            w.loader().close(src);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().empty(), kTimeoutMs);
            std::filesystem::path sidecar = ld;
            sidecar += ".sidecar.json";
            QVERIFY(std::filesystem::is_regular_file(sidecar));
            const int beforeClear = stack.index();
            ws.setOffsetAxis(false); // nothing is selected any more: nothing to zero, nothing committed
            QCOMPARE(stack.index(), beforeClear);
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            QCOMPARE(w.selection().sources().front()->id, src);
            QCOMPARE(offsetUsOf(w, mainLap), 11 * kStepUs);
            QVERIFY(std::abs(offsetMOf(w, mainLap) - 0.25) < 1e-9);
            QCOMPARE(offsetUsOf(w, refLap), -kStepUs);
            QVERIFY(std::abs(offsetMOf(w, refLap) - 0.25) < 1e-9);
            QVERIFY(!ws.state().offsetAxis);
            w.selection().setMain(mainLap);
            QTRY_VERIFY(ws.state().offsetAxis); // a stored offset shows the axis (04 section 3.3 item 5)
            QTRY_COMPARE(offsetCell(w, mainLap), QStringLiteral("+0.220 s"));
        }
        rt->shutdown();
    }

    // O off zeroes the offsets of the selected overlays while F4 hides them as well (06 section 14.4: every
    // offset of the selected ranges), in the one "Clear alignment offsets" command.
    void axisOffZeroesHiddenOverlays() {
        QTemporaryDir config;
        QTemporaryDir logs;
        QVERIFY(config.isValid() && logs.isValid());
        auto rt = uitest::makeRuntime();
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            qt::test::SyntheticOptions o;
            o.path = "hidden.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
            ld = qt::test::writeLd(*qt::test::makeSource(o, rt->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "hidden");
        }
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(uitest::showAndActivate(w));
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, kTimeoutMs);
            const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src);
            QCOMPARE(laps.size(), std::size_t{5});
            const model::RangeRef& mainLap = laps[2];
            const model::RangeRef& refLap = laps[3];
            w.selection().setMain(mainLap);
            w.selection().toggleOverlay0(refLap);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            QUndoStack& stack = w.session().undoStack();
            focusGraph(w);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
            QVERIFY(ws.state().offsetAxis);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Left, Qt::AltModifier | Qt::ShiftModifier);
            QTRY_COMPARE_WITH_TIMEOUT(offsetUsOf(w, refLap), -kStepUs, 5000);
            QCOMPARE(offsetUsOf(w, mainLap), Micros{0});
            // F4 hides the overlays; the reference overlay stays selected with its offset.
            w.selection().toggleOverlaysHidden();
            QVERIFY(w.selection().snapshot().overlaysHidden);
            focusGraph(w);
            const int base = stack.index();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
            QVERIFY(!ws.state().offsetAxis);
            QTRY_COMPARE_WITH_TIMEOUT(offsetUsOf(w, refLap), Micros{0}, 5000);
            QCOMPARE(stack.index(), base + 1);
            QCOMPARE(stack.text(stack.index() - 1), QStringLiteral("Clear alignment offsets"));
            stack.undo();
            QTRY_COMPARE_WITH_TIMEOUT(offsetUsOf(w, refLap), -kStepUs, 5000);
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstOffsetAxis)
#include "tst_offset_axis.moc"
