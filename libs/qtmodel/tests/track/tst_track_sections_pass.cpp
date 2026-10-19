// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's Track Sections pass and its inputs (02 sections 4.4 rule 13, 4.5 and 6.9; D-195, D-199 (e),
// D-204): the pass runs again whenever a lap index it reads changes, also when the number of lap indexes
// stays the same (a lap stretch published by LapStretchService replaces every index by a stretched copy), its
// dedupe is against the layer's latest run only (inputs that come back to an earlier run's run again), and a
// file closed and opened again is annotated afresh. M4 (D-210 (d)): a Venue detail edit binds the new venue's
// map and runs the pass again (M3 item 31), and a run superseded after it finished never publishes (M3 item
// 32). Expectations come from the synthetic source's speed model (qtmodel_test_support.cpp) and the ovals'
// geometry, never from the code under test.

#include "../qtmodel_test_support.hpp"
#include "../reports/report_track_fixtures.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QTest>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <thread>
#include <vector>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::rttest;

namespace {

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

// The time (s, from the lap start) at which the synthetic source's lap of `period` seconds and length L has
// covered `d` metres: its speed is L / P (1 + 0.3 sin(2 pi u)), so d(u) = L (u + 0.3 / (2 pi) (1 - cos(2 pi
// u))) (qtmodel_test_support.cpp), solved by bisection.
double lapTimeAtDistance(double d, double lengthM, double periodS) {
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 60; ++i) {
        const double u = 0.5 * (lo + hi);
        const double covered =
            lengthM * (u + 0.3 / (2.0 * std::numbers::pi) * (1.0 - std::cos(2.0 * std::numbers::pi * u)));
        (covered < d ? lo : hi) = u;
    }
    return 0.5 * (lo + hi) * periodS;
}

// Within 0.1 s of `want`.
bool closeTo(std::optional<double> got, double want) {
    return got && std::abs(*got - want) < 0.1;
}

bool poolIdle(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0;
}

// Every task has completed and every result it posted was delivered.
bool quiet(AppRuntime& rt) {
    return poolIdle(rt) && rt.dispatcher().pending() == 0;
}

// An oval of a 2000 m lap (the synthetic lap length) with straights of `straightM`.
std::shared_ptr<const analysis::TrackMap> ovalOf(const std::string& venue, double straightM) {
    const double radius = (2000.0 - 2.0 * straightM) / (2.0 * std::numbers::pi);
    return std::make_shared<const analysis::TrackMap>(ovalMap(venue, straightM, radius));
}

// A SourceLoader with the circuit rules and the Track Sections rule over "Test Oval", an oval of exactly the
// synthetic lap length (2000 m: straights 685.84 m, turns of radius 100 m); the synthetic source (out lap 40
// s from SOL to its end beacon, flying laps of 50, 48 and 49 s, in lap 30 s to EOL, every lap 2000 m) is
// written as an .ld with the venue and opened through the loader.
struct SectionsRig {
    static constexpr double kRadius = 100.0;
    static constexpr double kStraight = (2000.0 - 2.0 * std::numbers::pi * kRadius) / 2.0;
    static constexpr std::size_t kLaps = 5;

    AppRuntime rt{runtimeOptions()};
    MathsService maths{rt.pool(), rt.dispatcher(), test::shippedUnits()};
    DistanceService distance{maths, rt.dispatcher()};
    SourceLoader loader{rt.pool(), rt.dispatcher(), test::shippedUnits()};
    TrackMapService maps{nullptr};
    std::shared_ptr<const model::DataSource> latest; // the newest snapshot published or replaced

    // False when the rules or the map cannot be set.
    bool configure() {
        loader.setUseSidecars(false);
        const std::vector<std::string> ids{"circuit.whole-outing", "circuit.beacon-pair",
                                           "circuit.track-sections"};
        auto runner = analysis::RuleRunner::fromRuleIds(test::repoRoot() / "data" / "rules", ids);
        if (!runner) {
            return false;
        }
        loader.setRuleRunner(std::move(runner).value());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
        const auto map = std::make_shared<const analysis::TrackMap>(ovalMap("Test Oval", kStraight, kRadius));
        if (!maps.setMap("Test Oval", map).ok()) {
            return false;
        }
        loader.setTrackMaps(&maps);
        QObject::connect(&loader, &SourceLoader::sourcePublished, &loader,
                         [this](std::shared_ptr<const model::DataSource> s) { latest = std::move(s); });
        QObject::connect(&loader, &SourceLoader::sourceReplaced, &loader,
                         [this](std::shared_ptr<const model::DataSource> s) { latest = std::move(s); });
        return true;
    }
    std::filesystem::path writeSource(const std::string& name) {
        auto src =
            std::make_shared<model::DataSource>(*test::makeSource(test::SyntheticOptions{}, rt.cache()));
        src->details.push_back(textDetail("Venue", "Test Oval"));
        src->baseDetails = src->details;
        return test::writeLd(*src, test::scratchDir(name), "oval");
    }
    const model::RangeGroup* group(const std::string& path) const {
        return latest == nullptr ? nullptr : latest->layers.front().ranges.find(path);
    }
    // The start (s) of the "Turn 1" section inside lap `lap` (time order) of the newest snapshot.
    std::optional<double> turn1Start(std::size_t lap) const {
        const model::RangeGroup* laps = group("Outings:Laps");
        const model::RangeGroup* sections = group("Outings:Laps:Track Sections:Default");
        if (laps == nullptr || sections == nullptr || lap >= laps->ranges.size()) {
            return std::nullopt;
        }
        const model::Range& l = laps->ranges[lap];
        for (const model::Range& r : sections->ranges) {
            if (r.name == "Turn 1" && r.start >= l.start && r.start < l.end) {
                return static_cast<double>(r.start) / 1e6;
            }
        }
        return std::nullopt;
    }
    // Every lap has its Turn 1: all lap indexes were in the pass that built the current groups.
    bool everyLapHasSections() const {
        const model::RangeGroup* laps = group("Outings:Laps");
        if (laps == nullptr || laps->ranges.size() != kLaps) {
            return false;
        }
        for (std::size_t lap = 0; lap < kLaps; ++lap) {
            if (!turn1Start(lap)) {
                return false;
            }
        }
        return true;
    }
    // Range::stretch of lap `lap` in the newest snapshot.
    std::optional<double> stretchOf(std::size_t lap) const {
        const model::RangeGroup* laps = group("Outings:Laps");
        return laps == nullptr || lap >= laps->ranges.size() ? std::nullopt : laps->ranges[lap].stretch;
    }
};

} // namespace

class TstTrackSectionsPass : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // Review finding (Codex P1 on PR #4, D-204): the pass follows a lap index replaced at the same count. A
    // stretch published by LapStretchService (its deferred update, once every lap index exists) replaces the
    // indexes by stretched copies without adding one: the run key holds every index's D-140 key and the
    // publication starts the pass. Clearing the stretch brings the first run's sections back although that
    // run's key was seen before (the dedupe is against the layer's latest run, not every earlier one).
    void sectionsFollowTheLapStretch() {
        SectionsRig rig;
        QVERIFY(rig.configure());
        // No track length yet: no stretch.
        LapStretchService stretch(rig.loader, rig.distance, test::shippedUnits());
        rig.loader.open(rig.writeSource("tsp-stretch"));
        // The out lap (SOL at 0 s to its end beacon at 40 s) is measured backwards from that beacon, D-195
        // (d): track distance x lies at the stretched lap distance d = L s - (2000 - x), which Corr Dist
        // reaches at d / s, so the stretch s moves its sections. Turn 1 starts at x = straight / 2.
        const double x = SectionsRig::kStraight / 2.0;
        const double plain = lapTimeAtDistance(x, 2000.0, 40.0);
        const double flying = 40.0 + lapTimeAtDistance(x, 2000.0, 50.0); // lap 1, beacon to beacon
        QVERIFY(QTest::qWaitFor(
            [&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(0), plain); }, 60000));
        QVERIFY(!rig.stretchOf(0));

        // 2050 m: every lap (2000 m) is within the 5 % tolerance and gets s = 2050 / 2000, at the same count.
        stretch.setTrackLength(rig.latest->id, 2050.0);
        QVERIFY(QTest::qWaitFor([&] { return rig.stretchOf(0).has_value(); }, 60000));
        const double s = rig.stretchOf(0).value_or(0.0);
        QVERIFY(std::abs(s - 1.025) < 0.002);
        const double stretched = lapTimeAtDistance((2050.0 - 2000.0 + x) / s, 2000.0, 40.0);
        QVERIFY(std::abs(stretched - plain) > 0.3); // the check below tells the two apart
        QVERIFY2(QTest::qWaitFor([&] { return closeTo(rig.turn1Start(0), stretched); }, 60000),
                 qPrintable(QStringLiteral("out lap Turn 1 at %1 s, expected %2 s (unstretched %3 s)")
                                .arg(rig.turn1Start(0).value_or(-1.0))
                                .arg(stretched)
                                .arg(plain)));
        QVERIFY(rig.everyLapHasSections());
        // A lap from beacon to beacon maps proportionally (D-203 (e)): its sections stay in place.
        QVERIFY(closeTo(rig.turn1Start(1), flying));

        // The stretch cleared: the first run's inputs again, so its sections come back.
        stretch.setTrackLength(rig.latest->id, std::nullopt);
        QVERIFY(QTest::qWaitFor([&] { return !rig.stretchOf(0).has_value(); }, 60000));
        QVERIFY2(QTest::qWaitFor([&] { return closeTo(rig.turn1Start(0), plain); }, 60000),
                 qPrintable(QStringLiteral("out lap Turn 1 at %1 s, expected %2 s")
                                .arg(rig.turn1Start(0).value_or(-1.0))
                                .arg(plain)));
        QVERIFY(rig.everyLapHasSections());
        rig.loader.closeAll();
    }

    // The same file closed and opened again (the same SourceId) gets its Track Sections again: close()
    // forgets the closed source's runs, so none counts as done for it (a guard for D-204 (e)).
    void sectionsComeBackAfterAReopen() {
        SectionsRig rig;
        QVERIFY(rig.configure());
        const std::filesystem::path ld = rig.writeSource("tsp-reopen");
        const double flying = 40.0 + lapTimeAtDistance(SectionsRig::kStraight / 2.0, 2000.0, 50.0);
        rig.loader.open(ld);
        QVERIFY(QTest::qWaitFor(
            [&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(1), flying); }, 60000));
        const model::SourceId id = rig.latest->id;
        rig.loader.close(id);
        rig.latest = nullptr;
        rig.loader.open(ld);
        QVERIFY(QTest::qWaitFor([&] { return rig.latest != nullptr; }, 60000));
        QCOMPARE(rig.latest->id, id);
        QVERIFY(QTest::qWaitFor(
            [&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(1), flying); }, 60000));
        rig.loader.closeAll();
    }

    // M3 item 31 (D-210 (d)): a Venue detail edit (a sidecar edit, as the Details Editor commits it) binds
    // the new venue's map and runs the pass again; a venue without a map removes the groups. "Long Oval" has
    // straights of 600 m on the same 2000 m lap, so the first flying lap's Turn 1 starts at 300 m of it
    // instead of 342.92 m. Without the bound map's identity in the phase-2 key, the run the edit starts would
    // be deduplicated against the previous venue's run (same marker set, Corr Speed and indexes).
    void aVenueEditRunsThePassForTheNewVenue() {
        SectionsRig rig;
        QVERIFY(rig.configure());
        QVERIFY(rig.maps.setMap("Long Oval", ovalOf("Long Oval", 600.0)).ok());
        rig.loader.open(rig.writeSource("tsp-venue"));
        const double oval = 40.0 + lapTimeAtDistance(SectionsRig::kStraight / 2.0, 2000.0, 50.0);
        const double longer = 40.0 + lapTimeAtDistance(600.0 / 2.0, 2000.0, 50.0);
        QVERIFY(std::abs(oval - longer) > 0.5); // the check below tells the two apart
        QVERIFY(QTest::qWaitFor([&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(1), oval); },
                                60000));

        model::SidecarDocument doc = rig.latest->sidecar;
        doc.setDetail("Normal", textDetail("Venue", "Long Oval"));
        QVERIFY(rig.loader.commitSidecar(rig.latest, doc).ok());
        QCOMPARE(TrackMapService::venueOf(*rig.latest), std::string("Long Oval"));
        QVERIFY2(QTest::qWaitFor(
                     [&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(1), longer); }, 60000),
                 qPrintable(QStringLiteral("lap 1 Turn 1 at %1 s, expected %2 s (the old venue's %3 s)")
                                .arg(rig.turn1Start(1).value_or(-1.0))
                                .arg(longer)
                                .arg(oval)));

        // A venue without a map: the pass runs without one and removes the stale groups.
        doc = rig.latest->sidecar;
        doc.setDetail("Normal", textDetail("Venue", "Nowhere"));
        QVERIFY(rig.loader.commitSidecar(rig.latest, doc).ok());
        QVERIFY(QTest::qWaitFor([&] { return rig.group("Outings:Laps:Track Sections:Default") == nullptr; },
                                60000));
        rig.loader.closeAll();
    }

    // M3 item 32 (D-204 (d)): only the latest run of a layer publishes. Run A (the venue's map replaced by an
    // oval with 600 m straights) runs to its end on the pool; before its result reaches this thread the map
    // is replaced again (run B, 650 m straights). A's result is then dropped by the serial guard: no snapshot
    // ever shows A's sections, and B's arrive. The test holds the event loop between the two edits, so A has
    // finished (not been cancelled) when B starts.
    void aSupersededRunNeverPublishes() {
        SectionsRig rig;
        QVERIFY(rig.configure());
        rig.loader.open(rig.writeSource("tsp-serial"));
        const double oval = 40.0 + lapTimeAtDistance(SectionsRig::kStraight / 2.0, 2000.0, 50.0);
        QVERIFY(QTest::qWaitFor(
            [&] { return rig.everyLapHasSections() && closeTo(rig.turn1Start(1), oval) && quiet(rig.rt); },
            60000));
        std::vector<double> seen; // lap 1's Turn 1 in every replacement published from here
        QObject::connect(&rig.loader, &SourceLoader::sourceReplaced, &rig.loader,
                         [&](const std::shared_ptr<const model::DataSource>&) {
                             if (const std::optional<double> t = rig.turn1Start(1)) {
                                 seen.push_back(*t);
                             }
                         });
        const double timeA = 40.0 + lapTimeAtDistance(300.0, 2000.0, 50.0);
        const double timeB = 40.0 + lapTimeAtDistance(325.0, 2000.0, 50.0);
        QVERIFY(std::abs(timeA - timeB) > 0.3 && std::abs(timeA - oval) > 0.3);

        QVERIFY(rig.maps.setMap("Test Oval", ovalOf("Test Oval", 600.0)).ok()); // run A starts
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!poolIdle(rig.rt) && std::chrono::steady_clock::now() < until) {
            std::this_thread::yield(); // no event loop: A's result stays in the Dispatcher
        }
        QVERIFY(poolIdle(rig.rt));
        QVERIFY(rig.rt.dispatcher().pending() > 0); // A ran to its end and posted its result
        QVERIFY(rig.maps.setMap("Test Oval", ovalOf("Test Oval", 650.0)).ok()); // run B supersedes it

        QVERIFY2(QTest::qWaitFor([&] { return closeTo(rig.turn1Start(1), timeB) && quiet(rig.rt); }, 60000),
                 qPrintable(QStringLiteral("lap 1 Turn 1 at %1 s, expected %2 s")
                                .arg(rig.turn1Start(1).value_or(-1.0))
                                .arg(timeB)));
        QVERIFY(rig.everyLapHasSections());
        QVERIFY(std::none_of(seen.begin(), seen.end(), [&](double t) { return closeTo(t, timeA); }));
        QVERIFY(std::any_of(seen.begin(), seen.end(), [&](double t) { return closeTo(t, timeB); }));
        rig.loader.closeAll();
    }
};

QTEST_GUILESS_MAIN(TstTrackSectionsPass)
#include "tst_track_sections_pass.moc"
