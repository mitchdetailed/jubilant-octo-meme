// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-05 in the real shell (06 sections 13.3 and 14.4; D-014, D-065), offscreen, on a 60-lap outing:
//   insertAtCursorKeepsKeyedFlags  Tools > Lap Editor opens the editor on the Main source; SHIFT+F7 in the
//   graph
//                                  inserts a user beacon at the main cursor: the laps rebuild before the key
//                                  returns (the editor and the Data window's order show one lap more), one
//                                  undo step "Insert beacon"; a later lap is renumbered and keeps its trust
//                                  flag and both alignment offsets, which follow its start marker (02
//                                  section 4.3); CTRL+Z removes the beacon and CTRL+Y brings it back. The
//                                  rebuild time of the key is printed (the < 5 ms gate is
//                                  analysis.marker_edit's, in optimised builds).
//   offsetAndRestoreLogged         a per-beacon time offset (MarkerEdit.offset) moves the lap's start at once
//                                  as one step; "Restore Logged Laps" is one step that deletes every marker
//                                  edit and the user beacon (the laps are the logged ones again); undo brings
//                                  them back.
// Every runtime marks the GUI thread NoBlock as esses does; no ESSES_ASSERT may fail on any thread.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <memory>
#include <optional>
#include <vector>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr Micros kSec = 1'000'000;
constexpr std::size_t kFlyingLaps = 60;

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

// The laps of the source's current snapshot (time order).
std::vector<model::Range> lapsOf(MainWindow& w, const model::SourceId& src) {
    const auto source = w.selection().source(src);
    if (source == nullptr) {
        return {};
    }
    const model::DataLayer* layer = source->layer(model::kNormalLayer);
    const model::RangeGroup* group = layer == nullptr ? nullptr : layer->ranges.find("Outings:Laps");
    return group == nullptr ? std::vector<model::Range>{} : group->ranges;
}

std::optional<model::Range> lapStartingWith(MainWindow& w, const model::SourceId& src,
                                            const model::MarkerId& m) {
    for (const model::Range& r : lapsOf(w, src)) {
        if (r.startMarker == m) {
            return r;
        }
    }
    return std::nullopt;
}

struct Project {
    QTemporaryDir root;
    QTemporaryDir config;
    std::filesystem::path folder;
    std::filesystem::path ld;

    bool prepare(qt::AppRuntime& rt) {
        if (!root.isValid() || !config.isValid()) {
            return false;
        }
        folder = fsPath(root.path()) / "Outing60";
        std::filesystem::create_directories(folder / "logs");
        const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
        qt::test::SyntheticOptions o;
        o.path = "outing60.ld";
        o.lapSeconds.assign(kFlyingLaps + 2, 0.0);
        for (std::size_t i = 0; i < o.lapSeconds.size(); ++i) {
            o.lapSeconds[i] = 30.0 + static_cast<double>(i % 5) * 0.5; // 30.0 .. 32.0 s
        }
        o.rateHz = 20;
        ld = qt::test::writeLd(*qt::test::makeSource(o, rt.cache()), folder / "logs", "outing60");
        return true;
    }
};

struct Shell {
    std::unique_ptr<MainWindow> w;
    model::SourceId src;
    std::vector<model::RangeRef> laps;

    bool open(qt::AppRuntime& rt, const Project& p) {
        w = std::make_unique<MainWindow>(rt, uitest::pathsFor(p.config.path()));
        w->setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
        if (!w->session().newProject(p.folder, "Outing60").ok() || !w->openProject(p.folder) ||
            !uitest::showAndActivate(*w)) {
            return false;
        }
        w->openLog(p.ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == 1; }, 30000)) {
            return false;
        }
        src = w->selection().sources().front()->id;
        laps = w->selection().displayedRanges(src);
        return true;
    }
    QUndoStack& undo() { return w->session().undoStack(); }
    ComponentWidget* graph() {
        for (ComponentWidget* c : w->canvas().components()) {
            if (QLatin1String(c->type()) == QLatin1String("time_distance")) {
                return c;
            }
        }
        return nullptr;
    }
    void focusGraph() {
        ComponentWidget* g = graph();
        w->canvas().focusComponent(g);
        g->setFocus(Qt::OtherFocusReason);
    }
};

} // namespace

class TstLapEditor : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() { registerBuiltinComponents(); }

    void insertAtCursorKeepsKeyedFlags() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        {
            Shell s;
            QVERIFY(s.open(*rt, p));
            QCOMPARE(s.laps.size(), kFlyingLaps + 2); // out lap, 60 flying laps, in lap
            const model::RangeRef main = s.laps[10];
            s.w->selection().setMain(main);
            // A later lap: untrusted, with alignment offsets (both keyed by its start marker).
            const model::RangeRef later = s.laps[31];
            s.w->selection().setTrusted(later, false);
            const std::vector<model::Range> before = lapsOf(*s.w, s.src);
            QCOMPARE(before.size(), kFlyingLaps + 2);
            const model::Range laterRange = before[31];
            s.undo().push(new qt::OffsetRangeCommand(s.w->loader(), *s.w->selection().source(s.src), "Normal",
                                                     "Outings:Laps", laterRange, 125'000, 4.5));
            const int baseline = s.undo().index();

            // Tools > Lap Editor (menu only): the editor lists the Main source's laps.
            QAction* tools = s.w->actions().action(QStringLiteral("tools.lap_editor"));
            QVERIFY(tools != nullptr && tools->isEnabled());
            tools->trigger();
            LapEditor& editor = s.w->lapEditor();
            QVERIFY(editor.isVisible());
            QVERIFY(editor.source() == s.src);
            const int rowsBefore = editor.rowCount();
            QVERIFY(rowsBefore >= static_cast<int>(kFlyingLaps + 2));

            // SHIFT+F7 in the graph, the main cursor 12 s into the Main lap.
            s.w->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(s.w.get()));
            QVERIFY(s.w->activeWorksheet() != nullptr);
            s.w->activeWorksheet()->setCursor(12 * kSec, qt::CursorSource::Programmatic);
            s.focusGraph();
            QElapsedTimer timer;
            timer.start();
            uitest::press(*s.w, Qt::Key_F7, Qt::ShiftModifier);
            const qint64 ns = timer.nsecsElapsed();
            qInfo("SHIFT+F7 on a %zu-lap outing: laps rebuilt in %.2f ms (key to return, this build)",
                  kFlyingLaps, static_cast<double>(ns) / 1e6);
            // Synchronous: no wait. One lap more everywhere, one undo step.
            QCOMPARE(s.undo().index(), baseline + 1);
            QCOMPARE(s.undo().undoText(), QStringLiteral("Insert beacon"));
            QCOMPARE(lapsOf(*s.w, s.src).size(), kFlyingLaps + 3);
            QCOMPARE(s.w->selection().displayedRanges(s.src).size(), kFlyingLaps + 3);
            QCOMPARE(editor.rowCount(), rowsBefore + 1);
            // The later lap is renumbered and keeps its trust flag and offsets.
            const std::optional<model::Range> moved = lapStartingWith(*s.w, s.src, laterRange.startMarker);
            QVERIFY(moved.has_value());
            QCOMPARE(moved->number, laterRange.number + 1);
            QVERIFY(!moved->trusted);
            QCOMPARE(moved->offsetUs, Micros{125'000});
            QCOMPARE(moved->offsetM, 4.5);

            // CTRL+Z removes the beacon (the editor follows), CTRL+Y brings it back.
            s.focusGraph();
            uitest::press(*s.w, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(s.undo().index(), baseline);
            QCOMPARE(lapsOf(*s.w, s.src).size(), kFlyingLaps + 2);
            QCOMPARE(editor.rowCount(), rowsBefore);
            const std::optional<model::Range> back = lapStartingWith(*s.w, s.src, laterRange.startMarker);
            QVERIFY(back.has_value());
            QCOMPARE(back->number, laterRange.number);
            QVERIFY(!back->trusted);
            QCOMPARE(back->offsetUs, Micros{125'000});
            uitest::press(*s.w, Qt::Key_Y, Qt::ControlModifier);
            QCOMPARE(s.undo().index(), baseline + 1);
            QCOMPARE(lapsOf(*s.w, s.src).size(), kFlyingLaps + 3);
            editor.close();
        }
        rt->shutdown();
    }

    void offsetAndRestoreLogged() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        {
            Shell s;
            QVERIFY(s.open(*rt, p));
            s.w->selection().setMain(s.laps[5]);
            LapEditor& editor = s.w->lapEditor();
            s.w->commands().invoke(QStringLiteral("tools.lap_editor"));
            QVERIFY(editor.isVisible());
            const int base = s.undo().index();
            const std::vector<model::Range> logged = lapsOf(*s.w, s.src);

            // A user beacon inside lap 20 and a per-beacon time offset of lap 40's start: one step each.
            const model::Range lap20 = logged[20];
            QVERIFY(editor.insertBeaconAt(lap20.start + 10 * kSec));
            QCOMPARE(lapsOf(*s.w, s.src).size(), logged.size() + 1);
            const model::Range lap40 = logged[40];
            const int row40 = editor.rowOf(s.w->selection().displayedRanges(s.src)[41]);
            QVERIFY(row40 >= 0);
            QVERIFY(editor.setBeaconTimeOffset(row40, -2 * kSec));
            QCOMPARE(s.undo().index(), base + 2);
            const std::optional<model::Range> shifted = lapStartingWith(*s.w, s.src, lap40.startMarker);
            QVERIFY(shifted.has_value());
            QCOMPARE(shifted->start, lap40.start - 2 * kSec); // the laps rebuilt at once

            // Restore Logged Laps: one step; the logged laps again.
            QVERIFY(editor.restoreLogged());
            QCOMPARE(s.undo().index(), base + 3);
            const std::vector<model::Range> restored = lapsOf(*s.w, s.src);
            QCOMPARE(restored.size(), logged.size());
            for (std::size_t i = 0; i < logged.size(); ++i) {
                QCOMPARE(restored[i].start, logged[i].start);
                QCOMPARE(restored[i].end, logged[i].end);
            }
            // Undo brings the edits back; CTRL+Y restores again.
            s.undo().undo();
            QCOMPARE(lapsOf(*s.w, s.src).size(), logged.size() + 1);
            QCOMPARE(lapStartingWith(*s.w, s.src, lap40.startMarker)->start, lap40.start - 2 * kSec);
            s.undo().redo();
            QCOMPARE(lapsOf(*s.w, s.src).size(), logged.size());
            editor.close();
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstLapEditor)
#include "tst_lap_editor.moc"
