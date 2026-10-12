// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetViewModel and WorkspaceViewModel (06 section 4.5, 04 sections 2-3 and 8.5-8.6; D-016, D-018,
// D-133; AC-M1-09): F9 both ways with and without DistanceIndex (through the Main lap's index and back within
// one sample; the fraction rule and the "no distance data" banner without Corr Dist), N/P zoom and cursor
// preservation in both domains, W, Z, F2, zoom limits, pan clamping, cursor steps, datum, animation, offsets,
// the ChangeMask bits of each operation and the shared view across worksheets.

#include "qtmodel_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>

#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(esses::plot::ChangeMask)

namespace {

constexpr Micros kSec = 1'000'000;
constexpr Micros kPeriod = 20'000; // 50 Hz

SyntheticOptions lapsA(bool wheels = true) {
    SyntheticOptions o;
    o.path = wheels ? "wsvm-a.ld" : "wsvm-nodist.ld";
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0}; // out, Lap 1, Lap 2, Lap 3, in
    o.wheelSpeeds = wheels;
    return o;
}

project::WorksheetDef sheet(const char* domain) {
    project::WorksheetDef d;
    d.name = "Sheet";
    d.linkTo = "Laps";
    d.domain = domain;
    return d;
}

struct World {
    AppRuntime rt{[] {
        AppRuntimeOptions o;
        o.workers = 4;
        o.cacheBudget = std::size_t{256} << 20;
        return o;
    }()};
    MathsService maths{rt.pool(), rt.dispatcher(), shippedUnits()};
    DistanceService distance{maths, rt.dispatcher()};
    DataSelectionModel selection;
    WorkspaceViewModel workspace;
    std::shared_ptr<const model::DataSource> src;

    explicit World(bool wheels = true, bool prefetchDistance = true) {
        static_cast<void>(maths.loadSettingsSchema(repoRoot() / "data"));
        src = makeSource(lapsA(wheels), rt.cache());
        maths.addSource(src);
        if (prefetchDistance) {
            distance.prefetch(src->id);
        }
        selection.sourcePublished(src);
    }
    model::RangeRef lap(std::size_t i) const { return rangeRef(*src, i); }
    const model::Range& range(std::size_t i) const { return rangeAt(*src, i); }
    // Every lap index of the outing is built (prefetch at Background, AC-M1-10).
    bool distanceReady() {
        const auto c = distance.chain(lap(2), Priority::Interactive);
        return c != nullptr && c->complete();
    }
};

plot::ChangeMask lastMask(const QSignalSpy& spy) {
    return spy.isEmpty() ? plot::ChangeMask{0} : spy.last().at(0).value<plot::ChangeMask>();
}

bool near(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

} // namespace

class TstWorksheetViewModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<plot::ChangeMask>("esses::plot::ChangeMask"); }

    void sessionStartIsTheMainRange() {
        World w;
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        QVERIFY(ws.isActive());
        QCOMPARE(ws.state().zoom, (plot::XSpan{0.0, 40.0 * kSec})); // Main = the out lap
        QCOMPARE(ws.state().cursorT, Micros{0});
        QCOMPARE(ws.domain(), plot::Domain::Time);
    }

    // AC-M1-09, with Corr Dist: Time -> Distance through the Main lap's DistanceIndex, and back within one
    // sample.
    void f9ThroughTheDistanceIndex() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(), 30000);
        QCOMPARE(w.distance.state(w.src->id), DistanceState::Ready);
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{10.0 * kSec, 20.0 * kSec});
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        QSignalSpy banner(&ws, &WorksheetViewModel::noDistanceData);
        ws.toggleDomain();
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(plot::has(lastMask(changed), plot::Change::Domain));
        QCOMPARE(banner.count(), 0);
        const auto chain = w.distance.chain(w.lap(2), Priority::Interactive);
        QVERIFY(chain != nullptr);
        const Micros start = w.range(2).start;
        QVERIFY(near(ws.state().zoom.a, chain->distAt(start + 10 * kSec), 1e-6));
        QVERIFY(near(ws.state().zoom.b, chain->distAt(start + 20 * kSec), 1e-6));
        QVERIFY(ws.state().zoom.a > 0.0 && ws.state().zoom.b > ws.state().zoom.a);
        QVERIFY(ws.state().zoom.b < 2000.0); // inside the 2 km lap
        QCOMPARE(w.workspace.zoomDomain(), plot::Domain::Distance);
        QCOMPARE(ws.state().cursorT, w.workspace.cursorT()); // cursor unchanged (lap-relative time)
        ws.toggleDomain();
        QCOMPARE(ws.domain(), plot::Domain::Time);
        QVERIFY(near(ws.state().zoom.a, 10.0 * kSec, static_cast<double>(kPeriod)));
        QVERIFY(near(ws.state().zoom.b, 20.0 * kSec, static_cast<double>(kPeriod)));
        // The whole Main lap converts to [0, lap length).
        ws.zoomToMainRange();
        ws.toggleDomain();
        QVERIFY(near(ws.state().zoom.a, 0.0, 1e-6));
        QVERIFY(near(ws.state().zoom.b, 2000.0, 2.0)); // the synthetic lap is 2 km (trapezoid at 50 Hz)
    }

    // AC-M1-09, without Corr Dist: the fraction rule both ways (lossless) and the banner for the source.
    void f9FractionRuleAndBannerWithoutDistance() {
        World w(false);
        QTRY_COMPARE_WITH_TIMEOUT(w.distance.state(w.src->id), DistanceState::NoDistance, 30000);
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{12.0 * kSec, 36.0 * kSec}); // fractions 0.25 .. 0.75 of the 48 s lap
        QSignalSpy banner(&ws, &WorksheetViewModel::noDistanceData);
        ws.setDomain(plot::Domain::Distance);
        QCOMPARE(banner.count(), 1);
        QVERIFY(banner.last().at(0).toBool());
        QVERIFY(ws.noDistance());
        QVERIFY(near(ws.state().zoom.a, 0.25, 1e-12)); // extent'(R) = 1.0 without index or track map
        QVERIFY(near(ws.state().zoom.b, 0.75, 1e-12));
        ws.setDomain(plot::Domain::Time);
        QCOMPARE(ws.state().zoom, (plot::XSpan{12.0 * kSec, 36.0 * kSec}));
        QCOMPARE(banner.count(), 2);
        QVERIFY(!ws.noDistance());
    }

    // M1 integration (found by tests/ui/tst_walkthrough_m1 on the ACC stand-in): F9 and W while the Main
    // lap's index is still pending and another lap of the outing already has one. The span is the fraction
    // rule's, in normalised units (it used to be clamped to the indexed lap's metres before the origin,
    // [-4868 m, 0] on that file), and becomes metres with the same zoomFrac when the index arrives.
    void f9WhileTheMainIndexIsPending() {
        World w(true, false); // no prefetch: indexes are built only on request
        QTRY_VERIFY_WITH_TIMEOUT(w.distance.index(w.lap(0), Priority::Interactive) != nullptr, 30000);
        // Park every worker: the Main lap's index (requested by the view model) cannot be built yet.
        std::atomic<bool> release{false};
        std::atomic<int> parked{0};
        struct Release { // frees the workers on every exit path, before the runtime joins them
            std::atomic<bool>& flag;
            ~Release() { flag.store(true); }
        } releaseOnExit{release};
        for (int i = 0; i < 4; ++i) {
            w.rt.pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                parked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return 0;
            });
        }
        QTRY_COMPARE(parked.load(), 4);
        w.selection.setMain(w.lap(2)); // the 48 s lap
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{12.0 * kSec, 36.0 * kSec}); // fractions 0.25 .. 0.75
        const auto partial = w.distance.chain(w.lap(2), Priority::Interactive);
        QVERIFY(partial != nullptr && partial->anyIndexed());
        QVERIFY(!partial->coversTime(w.range(2).start, w.range(2).end));
        ws.toggleDomain();
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(!ws.noDistance()); // pending, not "no distance data"
        QVERIFY(w.workspace.zoomNormalised());
        QVERIFY(near(ws.state().zoom.a, 0.25, 1e-12));
        QVERIFY(near(ws.state().zoom.b, 0.75, 1e-12));

        release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(!w.workspace.zoomNormalised(), 30000);
        const auto chain = w.distance.chain(w.lap(2), Priority::Interactive);
        QVERIFY(chain != nullptr && chain->coversTime(w.range(2).start, w.range(2).end));
        const double len = chain->distAt(w.range(2).end);
        QVERIFY(len > 1900.0 && len < 2100.0);
        QVERIFY(near(ws.state().zoom.a, 0.25 * len, 1e-6));
        QVERIFY(near(ws.state().zoom.b, 0.75 * len, 1e-6));
        // Back to time, now through the index (distance fractions are not time fractions).
        ws.toggleDomain();
        const Micros start = w.range(2).start;
        QVERIFY(near(ws.state().zoom.a, static_cast<double>(chain->timeAt(0.25 * len) - start),
                     static_cast<double>(kPeriod)));
        QVERIFY(near(ws.state().zoom.b, static_cast<double>(chain->timeAt(0.75 * len) - start),
                     static_cast<double>(kPeriod)));
    }

    // W in distance mode before the Main lap's index exists: [0, 1] normalised, then the whole lap in metres.
    void wWhileTheMainIndexIsPending() {
        World w(true, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.distance.index(w.lap(0), Priority::Interactive) != nullptr, 30000);
        std::atomic<bool> release{false};
        std::atomic<int> parked{0};
        struct Release {
            std::atomic<bool>& flag;
            ~Release() { flag.store(true); }
        } releaseOnExit{release};
        for (int i = 0; i < 4; ++i) {
            w.rt.pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                parked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return 0;
            });
        }
        QTRY_COMPARE(parked.load(), 4);
        w.selection.setMain(w.lap(3));
        WorksheetViewModel ws(sheet("distance"), w.workspace, w.selection, w.distance);
        ws.zoomToMainRange();
        QVERIFY(w.workspace.zoomNormalised());
        QVERIFY(near(ws.state().zoom.a, 0.0, 1e-12));
        QVERIFY(near(ws.state().zoom.b, 1.0, 1e-12));
        release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(!w.workspace.zoomNormalised(), 30000);
        const auto chain = w.distance.chain(w.lap(3), Priority::Interactive);
        QVERIFY(chain != nullptr);
        QVERIFY(near(ws.state().zoom.a, 0.0, 1e-6));
        QVERIFY(near(ws.state().zoom.b, chain->distAt(w.range(3).end), 1e-6));
    }

    // N/P keep the zoom relative to the range start; the cursor keeps its time (time mode) or its lap
    // distance (distance mode), 04 section 2.1 item 1.
    void nextRangeKeepsZoomAndCursor() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(), 30000);
        w.selection.setMain(w.lap(1));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{5.0 * kSec, 15.0 * kSec});
        ws.setCursor(7 * kSec, CursorSource::Click);
        w.selection.nextRange();
        QCOMPARE(w.selection.snapshot().main, w.lap(2));
        QCOMPARE(ws.state().zoom, (plot::XSpan{5.0 * kSec, 15.0 * kSec}));
        QCOMPARE(ws.state().cursorT, 7 * kSec);
        // Distance mode: the cursor stays at the same lap distance.
        ws.setDomain(plot::Domain::Distance);
        const auto before = w.distance.chain(w.lap(2), Priority::Interactive);
        const double d = before->distAt(w.range(2).start + ws.state().cursorT);
        const plot::XSpan zoom = ws.state().zoom;
        w.selection.nextRange(); // Lap 3 (49 s): the same distance is reached at another time
        const auto after = w.distance.chain(w.lap(3), Priority::Interactive);
        QVERIFY(near(after->distAt(w.range(3).start + ws.state().cursorT), d, 0.5));
        QVERIFY(ws.state().cursorT != 7 * kSec);
        QCOMPARE(ws.state().zoom, zoom);
        // ... and P brings it back.
        w.selection.prevRange();
        QVERIFY(near(static_cast<double>(ws.state().cursorT), 7.0 * kSec, static_cast<double>(kPeriod)));
    }

    void zoomToMainRangePullsTheCursorInside() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.setCursor(-5 * kSec, CursorSource::Click); // in the previous lap
        QCOMPARE(ws.state().cursorT, -5 * kSec);
        ws.zoomFull();
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.zoomToMainRange();
        QCOMPARE(ws.state().zoom, (plot::XSpan{0.0, 48.0 * kSec}));
        QCOMPARE(ws.state().cursorT, Micros{0});
        QCOMPARE(changed.count(), 1); // one change for both
        QVERIFY(plot::has(lastMask(changed), plot::Change::Zoom));
        QVERIFY(plot::has(lastMask(changed), plot::Change::Cursor));
        ws.setCursor(10 * kSec, CursorSource::Click);
        ws.zoomToMainRange(); // inside: the cursor stays
        QCOMPARE(ws.state().cursorT, 10 * kSec);
    }

    void zoomFullIsTheMainOuting() {
        World w;
        w.selection.setMain(w.lap(2)); // starts at 90 s in a 217 s outing
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomFull();
        QCOMPARE(ws.state().zoom, (plot::XSpan{-90.0 * kSec, 127.0 * kSec}));
        ws.zoomOut(); // the maximum is the outing extent
        QCOMPARE(ws.state().zoom, (plot::XSpan{-90.0 * kSec, 127.0 * kSec}));
    }

    void zoomInOutAboutTheCursor() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{0.0, 40.0 * kSec});
        ws.setCursor(10 * kSec, CursorSource::Click);
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.zoomIn();
        QCOMPARE(ws.state().zoom, (plot::XSpan{5.0 * kSec, 25.0 * kSec}));
        QCOMPARE(lastMask(changed), plot::mask(plot::Change::Zoom));
        ws.zoomOut();
        QCOMPARE(ws.state().zoom, (plot::XSpan{0.0, 40.0 * kSec}));
        for (int i = 0; i < 40; ++i) {
            ws.zoomIn();
        }
        QCOMPARE(ws.state().zoom.length(), static_cast<double>(4 * kPeriod)); // >= 4 samples
        QVERIFY(ws.state().zoom.a <= 10.0 * kSec && ws.state().zoom.b >= 10.0 * kSec);
    }

    void panIsClampedToTheOuting() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{0.0, 10.0 * kSec});
        ws.panBy(0.10);
        QCOMPARE(ws.state().zoom, (plot::XSpan{1.0 * kSec, 11.0 * kSec}));
        ws.panBy(-0.90);
        QCOMPARE(ws.state().zoom, (plot::XSpan{-8.0 * kSec, 2.0 * kSec})); // into the previous lap
        for (int i = 0; i < 30; ++i) {
            ws.panBy(-0.90);
        }
        QCOMPARE(ws.state().zoom, (plot::XSpan{-90.0 * kSec, -80.0 * kSec})); // the outing start, never 0
        for (int i = 0; i < 40; ++i) {
            ws.panBy(0.90);
        }
        QCOMPARE(ws.state().zoom, (plot::XSpan{117.0 * kSec, 127.0 * kSec}));
        ws.setCursor(30 * kSec, CursorSource::Click);
        ws.centreOnCursor();
        QCOMPARE(ws.state().zoom, (plot::XSpan{25.0 * kSec, 35.0 * kSec}));
        ws.extendZoom(40.0 * kSec);
        QCOMPARE(ws.state().zoom, (plot::XSpan{25.0 * kSec, 40.0 * kSec}));
    }

    void cursorStepsAndEdges() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.zoomTo(plot::XSpan{0.0, 10.0 * kSec});
        ws.setSize(100); // 100 ms per pixel = 5 samples per pixel
        ws.setCursor(5 * kSec, CursorSource::Click);
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.stepCursor(1);
        QCOMPARE(ws.state().cursorT, 5 * kSec + 5 * kPeriod);
        QCOMPARE(lastMask(changed), plot::mask(plot::Change::Cursor)); // Dynamic layer only
        ws.stepCursor(-10);
        QCOMPARE(ws.state().cursorT, 5 * kSec + 5 * kPeriod - 50 * kPeriod);
        ws.setSize(100'000); // far below one sample per pixel: one sample per step
        ws.stepCursor(1);
        QCOMPARE(ws.state().cursorT, 5 * kSec - 45 * kPeriod + kPeriod);
        ws.cursorHome();
        QCOMPARE(ws.state().cursorT, Micros{0});
        ws.cursorEnd();
        QCOMPARE(ws.state().cursorT, 10 * kSec - kPeriod);
        ws.setCursor(-1000 * kSec, CursorSource::Click); // clamped to the outing
        QCOMPARE(ws.state().cursorT, -90 * kSec);
        // Distance-mode steps move by at least one sample too.
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(), 30000);
        ws.setDomain(plot::Domain::Distance);
        ws.setCursor(5 * kSec, CursorSource::Click);
        ws.stepCursor(1);
        QVERIFY(ws.state().cursorT > 5 * kSec);
        ws.stepCursor(-1);
        QVERIFY(near(static_cast<double>(ws.state().cursorT), 5.0 * kSec, static_cast<double>(kPeriod)));
    }

    void datumPlaceToggleSwapAndZ() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.setCursor(10 * kSec, CursorSource::Click);
        ws.placeDatum();
        QCOMPARE(lastMask(changed), plot::mask(plot::Change::Datum));
        QCOMPARE(*ws.state().datumT, 10 * kSec);
        ws.setCursor(30 * kSec, CursorSource::Click);
        ws.toggleDatum(); // hidden, value kept
        QVERIFY(!ws.state().datumT);
        QCOMPARE(*w.workspace.storedDatumT(), 10 * kSec);
        ws.zoomToDatumMain(); // needs a visible datum: nothing
        QCOMPARE(ws.state().zoom, (plot::XSpan{0.0, 48.0 * kSec}));
        ws.toggleDatum();
        QCOMPARE(*ws.state().datumT, 10 * kSec);
        ws.zoomToDatumMain();
        QCOMPARE(ws.state().zoom, (plot::XSpan{10.0 * kSec, 30.0 * kSec}));
        ws.swapDatum();
        QCOMPARE(lastMask(changed), plot::Change::Cursor | plot::Change::Datum);
        QCOMPARE(ws.state().cursorT, 10 * kSec);
        QCOMPARE(*ws.state().datumT, 30 * kSec);
    }

    void animationAdvancesTheCursor() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(), 30000); // no Data change from the prefetch from here on
        w.selection.setMain(w.lap(4));                      // the 30 s in lap, the last of the outing
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        ws.setAnimationRate(10.0);
        QCOMPARE(ws.animationRate(), 4.0); // x0.25 .. x4
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.setAnimating(true);
        QVERIFY(ws.animating());
        QTRY_VERIFY(ws.state().cursorT > 200'000); // 4x real time
        for (const QList<QVariant>& args : changed) {
            QCOMPARE(args.at(0).value<plot::ChangeMask>(), plot::mask(plot::Change::Cursor)); // Cursor only
        }
        ws.setAnimating(false);
        const Micros stopped = ws.state().cursorT;
        QTest::qWait(100);
        QCOMPARE(ws.state().cursorT, stopped);
        // At the end of the outing the animation stops by itself.
        ws.setCursor(30 * kSec - 2 * kPeriod, CursorSource::Click);
        ws.setAnimating(true);
        QTRY_VERIFY(!ws.animating());
        QCOMPARE(ws.state().cursorT, 30 * kSec - 1);
    }

    // D-133: one shared zoom/cursor/datum; the domain stays per worksheet and the shared zoom converts on
    // activation.
    void worksheetsShareTheView() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.distanceReady(), 30000);
        w.selection.setMain(w.lap(2));
        WorksheetViewModel time(sheet("time"), w.workspace, w.selection, w.distance);
        WorksheetViewModel dist(sheet("distance"), w.workspace, w.selection, w.distance);
        QVERIFY(time.isActive());
        QVERIFY(!dist.isActive());
        time.zoomTo(plot::XSpan{10.0 * kSec, 20.0 * kSec});
        time.setCursor(12 * kSec, CursorSource::Click);
        QCOMPARE(dist.state().cursorT, 12 * kSec);
        const auto chain = w.distance.chain(w.lap(2), Priority::Interactive);
        // The inactive distance sheet shows the shared zoom converted on the fly.
        QVERIFY(near(dist.state().zoom.a, chain->distAt(w.range(2).start + 10 * kSec), 1e-6));
        dist.activate();
        QVERIFY(dist.isActive());
        QCOMPARE(w.workspace.zoomDomain(), plot::Domain::Distance);
        QVERIFY(near(time.state().zoom.a, 10.0 * kSec, static_cast<double>(kPeriod)));
        QCOMPARE(dist.state().cursorT, 12 * kSec);
        QCOMPARE(time.domain(), plot::Domain::Time);
        QCOMPARE(dist.domain(), plot::Domain::Distance);
    }

    void offsetsArePendingThenCommitted() {
        World w;
        w.selection.setMain(w.lap(2));
        WorksheetViewModel ws(sheet("time"), w.workspace, w.selection, w.distance);
        int commits = 0;
        double committed = 0.0;
        ws.setOffsetCommit([&](const model::RangeRef& r, plot::Domain d, double v) {
            ++commits;
            committed = v;
            QCOMPARE(r, w.lap(2));
            QCOMPARE(d, plot::Domain::Time);
        });
        QSignalSpy changed(&ws, &WorksheetViewModel::changed);
        ws.offsetRange(w.lap(2), 1);
        ws.offsetRange(w.lap(2), 10);
        QCOMPARE(lastMask(changed), plot::mask(plot::Change::Offsets));
        QVERIFY(ws.state().offsetAxis);
        QCOMPARE(ws.pendingOffsets().at(w.lap(2)), static_cast<double>(11 * kPeriod));
        QTRY_COMPARE(commits, 1); // 250 ms after the last nudge
        QCOMPARE(committed, static_cast<double>(11 * kPeriod));
        QVERIFY(ws.pendingOffsets().empty());
        ws.setOffsetAxis(false);
        QVERIFY(!ws.state().offsetAxis);
    }

    void workspaceZoomFractionAndDatum() {
        WorkspaceViewModel ws;
        QSignalSpy changed(&ws, &WorkspaceViewModel::changed);
        ws.setZoom(plot::XSpan{25.0, 75.0}, plot::Domain::Time, 100.0);
        QCOMPARE(ws.zoomFrac().fa, 0.25);
        QCOMPARE(ws.zoomFrac().fb, 0.75);
        ws.setZoom(plot::XSpan{25.0, 75.0}, plot::Domain::Time, 100.0);
        QCOMPARE(changed.count(), 1); // unchanged: no signal
        ws.convertZoom(plot::Domain::Distance, 2000.0, std::nullopt, plot::XSpan{0.0, 1000.0});
        QCOMPARE(ws.zoom(), (plot::XSpan{0.0, 1000.0})); // fraction rule [500, 1500), shifted into the clamp
        QCOMPARE(ws.zoomDomain(), plot::Domain::Distance);
        ws.setDatum(5);
        ws.setDatumVisible(false);
        QVERIFY(!ws.datumT());
        QCOMPARE(*ws.storedDatumT(), Micros{5});
        ws.setCursorDatum(7, 9);
        QCOMPARE(ws.cursorT(), Micros{7});
        QCOMPARE(*ws.storedDatumT(), Micros{9});
        QVERIFY(!ws.datumT());
    }
};

QTEST_GUILESS_MAIN(TstWorksheetViewModel)
#include "tst_worksheetviewmodel.moc"
