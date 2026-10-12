// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-01 (07 section 4.2, demo 4.5), offscreen, in the real shell (MainWindow on a private configuration
// directory, the shipped circuit-default workbook): open fixtures/motec.ld/synthetic-acc-dialect/input.ld
// (the public stand-in for an ACC export); F8; CTRL+F selects the fastest *trusted* lap as Main; DOWN,
// ALT+ENTER adds the next lap as Overlay 1; F9 switches to distance; the two traces align at the start line;
// a cursor drag in the graph updates the Values window rows for Main and Overlay 1. Every key goes through
// the KeyEventRouter and the KeymapBinder (QTest key clicks on the focus widget). The same walkthrough runs
// on a real logger file of the private corpus with beacon laps when one is present (fixtures/private, never
// committed); the user's corpus has no ACC export, so that run skips with the reason otherwise.
// The public stand-in's flying laps are identical copies (a tie for CTRL+F, and Main and Overlay 1 coincide
// in time as well as in distance), so the walkthrough also runs on a written synthetic log whose flying laps
// differ (50, 48, 49 s, the same track profile per lap fraction): there CTRL+F must pick the one 48 s lap
// from a later Main, and at one cursor position the Throttle of Main and Overlay 1 agree in distance mode but
// not in time mode, which only a distance alignment through each lap's DistanceIndex gives.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/model/range.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <vector>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kOpenTimeoutMs = 120000;
const QString kData = QStringLiteral("data");
const QString kValues = QStringLiteral("values");

// The polylines of one colour in a Static build (Overlay n draws in the channel colours too: the shipped
// schemes' overlayStyle is "channel" / "solid").
std::vector<std::vector<float>> polylinesOf(const plot::DrawList& list, project::Rgba colour) {
    std::vector<std::vector<float>> out;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind != plot::CmdKind::Polyline || c.colour != colour) {
            continue;
        }
        out.emplace_back(list.verts.begin() + 2 * c.vertexOffset,
                         list.verts.begin() + 2 * (c.vertexOffset + c.vertexCount));
    }
    return out;
}

bool samePolyline(const std::vector<float>& a, const std::vector<float>& b, float tol) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > tol) {
            return false;
        }
    }
    return true;
}

int rowOf(const qt::ValuesModel& m, const QString& channel) {
    return static_cast<int>(m.channels().indexOf(channel));
}

// The displayed row of a channel by its name cell (all-channels mode lists the Main source's channels).
int displayedRowOf(const qt::ValuesModel& m, const QString& channel) {
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.cellText(r, qt::ValuesModel::NameCol) == channel) {
            return r;
        }
    }
    return -1;
}

struct Cells {
    std::optional<double> main;
    std::optional<double> overlay;
};

Cells cellsOf(const qt::ValuesModel& m, int row) {
    return {uitest::numberOf(m.cellText(row, qt::ValuesModel::MainCol)),
            uitest::numberOf(m.cellText(row, qt::ValuesModel::ReferenceCol))};
}

bool settled(const qt::ValuesModel& m, int row) {
    const Cells c = cellsOf(m, row);
    return c.main.has_value() && c.overlay.has_value();
}

// What the log's laps allow the walkthrough to check.
enum class Laps {
    Identical, // the public stand-in: flying laps are copies; Main and Overlay 1 coincide exactly
    Unequal,   // the written synthetic log: flying laps of 50, 48 and 49 s with one profile per lap fraction
    Real       // a private logger file: nothing more is known
};

// The fastest trusted flying lap by the TT 2485 rule, computed here from the displayed laps (lowest number on
// a tie), and the slowest one (a Main other than the fastest before CTRL+F).
struct FlyingLaps {
    std::optional<model::RangeRef> fastest;
    std::optional<model::RangeRef> slowest;
};
FlyingLaps trustedFlying(MainWindow& w, const model::SourceId& src) {
    FlyingLaps out;
    const model::Range* best = nullptr;
    const model::Range* worst = nullptr;
    for (const model::RangeRef& r : w.selection().displayedRanges(src)) {
        const model::Range* x = w.selection().resolve(r);
        if (x == nullptr || x->lapClass != model::LapClass::Flying || !x->trusted) {
            continue;
        }
        if (best == nullptr || x->duration() < best->duration() ||
            (x->duration() == best->duration() && x->number < best->number)) {
            best = x;
            out.fastest = r;
        }
        if (worst == nullptr || x->duration() > worst->duration() ||
            (x->duration() == worst->duration() && x->number > worst->number)) {
            worst = x;
            out.slowest = r;
        }
    }
    return out;
}

// The walkthrough on one log.
void walkthrough(const std::filesystem::path& ld, Laps laps) {
    const bool identicalLaps = laps == Laps::Identical;
    QTemporaryDir config;
    auto rt = uitest::makeRuntime(4);
    {
        MainWindow w(*rt, uitest::pathsFor(config.path()));
        w.resize(1600, 1000);
        QVERIFY(uitest::showAndActivate(w));
        w.openLog(ld);
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kOpenTimeoutMs);
        const auto src = w.selection().sources().front();
        QVERIFY(w.selection().hasMain());
        // Corr Speed / Corr Dist and the lap distance indexes arrive without any request (AC-M1-10).
        QTRY_VERIFY_WITH_TIMEOUT(w.distance().state(src->id) == qt::DistanceState::Ready ||
                                     w.distance().state(src->id) == qt::DistanceState::NoDistance,
                                 kOpenTimeoutMs);
        QCOMPARE(w.distance().state(src->id), qt::DistanceState::Ready);

        // F8: the Data window takes the focus.
        w.returnFocus();
        uitest::press(w, Qt::Key_F8);
        QCOMPARE(w.docks().focusedDockId(), kData);

        // CTRL+F: the fastest trusted lap becomes Main (from another Main: the slowest trusted flying lap, or
        // the last one on a tie); the highlight follows it.
        const FlyingLaps flyingLaps = trustedFlying(w, src->id);
        QVERIFY(flyingLaps.fastest.has_value() && flyingLaps.slowest.has_value());
        QVERIFY(*flyingLaps.fastest != *flyingLaps.slowest);
        w.selection().setMain(*flyingLaps.slowest);
        uitest::press(w, Qt::Key_F, Qt::ControlModifier);
        const model::RangeRef mainRef = w.selection().snapshot().main;
        QVERIFY(mainRef == *flyingLaps.fastest);
        const model::Range* main = w.selection().resolve(mainRef);
        QVERIFY(main != nullptr);
        if (laps == Laps::Identical) {
            QCOMPARE(main->number, 1); // a tie: the lowest lap number
        } else if (laps == Laps::Unequal) {
            QCOMPARE(main->duration(), Micros{48000000}); // the one 48 s lap, flying lap 2
            QCOMPARE(main->number, 2);
        }
        QCOMPARE(main->lapClass, model::LapClass::Flying);
        QVERIFY(main->trusted);
        int flying = 0;
        for (const model::RangeRef& r : w.selection().displayedRanges(src->id)) {
            const model::Range* x = w.selection().resolve(r);
            if (x != nullptr && x->lapClass == model::LapClass::Flying && x->trusted) {
                ++flying;
                QVERIFY2(main->duration() <= x->duration(), "Main is the fastest trusted flying lap");
            }
        }
        QVERIFY(flying >= 2);
        QVERIFY(w.dataWindow().highlightedRange().has_value());
        QCOMPARE(*w.dataWindow().highlightedRange(), mainRef);

        // DOWN, ALT+ENTER: the next lap becomes Overlay 1 (Main unchanged).
        uitest::press(w, Qt::Key_Down);
        const std::optional<model::RangeRef> next = w.dataWindow().highlightedRange();
        QVERIFY(next.has_value());
        QVERIFY(*next != mainRef);
        uitest::press(w, Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(w.selection().snapshot().main, mainRef);
        QCOMPARE(w.selection().snapshot().overlays.size(), std::size_t{1});
        QCOMPARE(w.selection().snapshot().overlays.front(), *next);
        QCOMPARE(w.selection().overlayNumber(*next), 1);
        const model::Range* overlay = w.selection().resolve(*next);
        QVERIFY(overlay != nullptr);

        // F9 (global, from the Data window): distance mode, no banner.
        qt::WorksheetViewModel* ws = w.activeWorksheet();
        QVERIFY(ws != nullptr);
        TimeDistanceGraph* graph = uitest::graphOf(w);
        QVERIFY(graph != nullptr);
        plot::ComponentBuild last; // the last delivered Static build
        graph->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b; });
        QCOMPARE(ws->domain(), plot::Domain::Time);
        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws->domain(), plot::Domain::Distance);
        QVERIFY(!ws->noDistance());

        // The graph shows the whole Main lap from the start line (W in the graph).
        // The lap distance indexes are prefetched in the Background band: no action is needed for them to
        // arrive (a span set before they do is re-expressed in metres when they land).
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const model::RangeRef& r : {mainRef, *next}) {
                    const auto c = w.distance().chain(r, Priority::Interactive);
                    const model::Range* x = w.selection().resolve(r);
                    if (c == nullptr || x == nullptr || !c->coversTime(x->start, x->end)) {
                        return false;
                    }
                }
                return true;
            }(),
            kOpenTimeoutMs);
        w.canvas().focusComponent(graph);
        uitest::press(w, Qt::Key_W);
        QVERIFY(std::abs(ws->state().zoom.a) < 1e-6);
        const auto mainChain = w.distance().chain(mainRef, Priority::Interactive);
        QVERIFY(std::abs(ws->state().zoom.b - mainChain->distAt(main->end)) < 1e-6);
        QVERIFY(uitest::waitQuiet(*graph));
        graph->setBuildObserver({});
        QCOMPARE(graph->inputs().view.domain, plot::Domain::Distance);
        QCOMPARE(graph->inputs().slots.size(), std::size_t{2});
        // Both laps start at distance 0 of their own chains: the start line is the same x for both.
        for (const plot::SlotData& s : graph->inputs().slots) {
            QVERIFY(s.chain != nullptr);
            QVERIFY(std::abs(s.chain->distAt(s.range.start)) < 1e-6);
            QVERIFY(std::abs(s.chain->timeAt(0.0) - s.range.start) <= 1000); // within a 1 kHz sample
        }
        QVERIFY(std::abs(graph->logicalXOf(0.0) -
                         static_cast<double>(graph->layout().traceArea.x / graph->layout().dpr)) < 1.0);
        // The Corr Speed traces of Main and Overlay 1 start at the trace area's left edge (the start line)
        // and, on identical laps, coincide.
        QVERIFY(!graph->layout().groups.empty() && !graph->layout().groups.front().rows.empty());
        const plot::KeyRow& speedRow = graph->layout().groups.front().rows.front();
        QVERIFY(speedRow.label.rfind("Corr Speed", 0) == 0);
        const auto lines = polylinesOf(last.list, speedRow.colour);
        QVERIFY2(lines.size() >= 2, "a Corr Speed polyline per slot");
        const float left = static_cast<float>(graph->layout().traceArea.x);
        int atStart = 0;
        for (const auto& l : lines) {
            atStart += std::abs(l.front() - left) <= 1.0F ? 1 : 0;
        }
        QVERIFY2(atStart >= 2, "Main and Overlay 1 both begin at the start line");
        if (identicalLaps) {
            for (const auto& l : lines) {
                const auto twins =
                    std::count_if(lines.begin(), lines.end(),
                                  [&l](const std::vector<float>& o) { return samePolyline(l, o, 1.0F); });
                QVERIFY2(twins >= 2, "the Overlay 1 trace lies on the Main trace in distance mode");
            }
        }

        // V shows the Values window; its rows follow the focused graph.
        if (!w.docks().isVisible(kValues)) {
            w.canvas().focusComponent(graph);
            uitest::press(w, Qt::Key_V);
        }
        QVERIFY(w.docks().isVisible(kValues));
        qt::ValuesModel& values = w.valuesWindow().model();
        QTRY_VERIFY(rowOf(values, QStringLiteral("Corr Speed")) >= 0);
        const int row = rowOf(values, QStringLiteral("Corr Speed"));

        // Cursor at the start line: Main and Overlay 1 read their own lap starts.
        ws->setCursor(0, qt::CursorSource::Programmatic);
        QTRY_VERIFY_WITH_TIMEOUT(settled(values, row), 10000);
        if (identicalLaps) {
            const Cells c = cellsOf(values, row);
            QCOMPARE(*c.main, *c.overlay);
        }

        // Cursor drag in the first panel: the Main and Overlay 1 cells follow.
        const plot::RectPx t = graph->layout().groups.front().panel;
        const auto dpr = static_cast<double>(graph->layout().dpr);
        const double y = (static_cast<double>(t.y) + 0.5 * static_cast<double>(t.h)) / dpr;
        const double x1 = (static_cast<double>(t.x) + 0.35 * static_cast<double>(t.w)) / dpr;
        const double x2 = (static_cast<double>(t.x) + 0.60 * static_cast<double>(t.w)) / dpr;
        uitest::sendMouse(graph, QEvent::MouseButtonPress, {x1, y}, Qt::LeftButton, Qt::LeftButton);
        const Micros cursorAtX1 = ws->state().cursorT;
        QTRY_VERIFY_WITH_TIMEOUT(settled(values, row), 10000);
        const Cells before = cellsOf(values, row);
        uitest::sendMouse(graph, QEvent::MouseMove, {x2, y}, Qt::NoButton, Qt::LeftButton);
        uitest::sendMouse(graph, QEvent::MouseButtonRelease, {x2, y}, Qt::LeftButton, Qt::NoButton);
        QVERIFY(ws->state().cursorT > cursorAtX1);
        QTRY_VERIFY_WITH_TIMEOUT(settled(values, row) && cellsOf(values, row).main != before.main, 10000);
        const Cells after = cellsOf(values, row);
        QVERIFY2(after.main != before.main, "the Main cell follows the cursor");
        QVERIFY2(after.overlay != before.overlay, "the Overlay 1 cell follows the cursor");
        if (identicalLaps) {
            QCOMPARE(*before.main, *before.overlay); // aligned at the same lap distance
            QCOMPARE(*after.main, *after.overlay);
        }
        qInfo("walkthrough on %s: Main %s, Overlay 1 %s; Corr Speed %.1f -> %.1f (Main), %.1f -> %.1f "
              "(Overlay 1)",
              ld.filename().string().c_str(), main->name.c_str(), overlay->name.c_str(), *before.main,
              *after.main, *before.overlay, *after.overlay);

        if (laps == Laps::Unequal) {
            // The synthetic laps share one Throttle profile per lap fraction and their distance per lap
            // fraction is the same too, so at one cursor (72 % of the 48 s Main) Main and Overlay 1 (49 s)
            // read the same Throttle in distance mode and different values in time mode (Overlay 1 is then
            // at 70.5 %).
            values.setAllChannels(true);
            QTRY_VERIFY(displayedRowOf(values, QStringLiteral("Throttle")) >= 0);
            const int throttle = displayedRowOf(values, QStringLiteral("Throttle"));
            const Micros at = Micros{34560000};
            ws->setCursor(at, qt::CursorSource::Programmatic);
            QCOMPARE(ws->domain(), plot::Domain::Distance);
            QTRY_VERIFY_WITH_TIMEOUT(settled(values, throttle), 10000);
            const Cells inDistance = cellsOf(values, throttle);
            uitest::press(w, Qt::Key_F9);
            QCOMPARE(ws->domain(), plot::Domain::Time);
            ws->setCursor(at, qt::CursorSource::Programmatic);
            QTRY_VERIFY_WITH_TIMEOUT(
                settled(values, throttle) && cellsOf(values, throttle).main == inDistance.main, 10000);
            const Cells inTime = cellsOf(values, throttle);
            qInfo("Throttle at 72 %% of Main: distance %.2f / %.2f, time %.2f / %.2f (Main / Overlay 1)",
                  *inDistance.main, *inDistance.overlay, *inTime.main, *inTime.overlay);
            // Throttle (%) displays as a fraction with two decimals: 40.6 % reads 0.41 for both in distance
            // mode; in time mode Main reads 0.41 and Overlay 1 0.36 (36.1 %). One display step is 0.01.
            QVERIFY2(std::abs(*inDistance.main - *inDistance.overlay) <= 0.011,
                     "distance mode aligns Overlay 1 to Main by lap distance");
            QVERIFY2(std::abs(*inTime.main - *inTime.overlay) >= 0.03,
                     "time mode does not: the alignment above is not a time alignment");
        }
    }
    rt->shutdown();
}

} // namespace

class TstWalkthroughM1 : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerBuiltinComponents(); }

    // AC-M1-12: the GUI thread never blocked on a Future (no assertion recorded, any thread).
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    // AC-M1-01 on the public stand-in (generated by fixtures.generate_inputs or esses-gen-fixtures).
    void accDialectWalkthrough() {
        const std::filesystem::path ld = uitest::accFixtureInput();
        if (ld.empty()) {
            QSKIP("synthetic-acc-dialect/input.ld is not generated (ctest fixtures.generate_inputs or "
                  "esses-gen-fixtures --inputs-only)");
        }
        walkthrough(ld, Laps::Identical);
    }

    // AC-M1-01 on laps that tell the fastest lap and the distance alignment apart (see the header).
    void unequalLapsWalkthrough() {
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        std::filesystem::path ld;
        {
            auto writer = uitest::makeRuntime(2, false); // writing the log reads every chunk (blocking)
            qt::test::SyntheticOptions o;
            o.path = "unequal-laps.ld";
            o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0}; // out lap, flying 50 / 48 / 49 s, in lap
            ld = qt::test::writeLd(*qt::test::makeSource(o, writer->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "unequal-laps");
            writer->shutdown();
        }
        walkthrough(ld, Laps::Unequal);
    }

    // The private run of AC-M1-01: a real logger file with beacon laps (no ACC export exists in the corpus).
    void privateRealFileWalkthrough() {
        std::string reason;
        const auto file = uitest::privateBeaconLapFile(reason);
        if (!file) {
            QSKIP(("private run skipped: " + reason).c_str());
        }
        qInfo("private file %s", file->name.c_str());
        walkthrough(file->input, Laps::Real);
    }
};

QTEST_MAIN(TstWalkthroughM1)
#include "tst_walkthrough_m1.moc"
