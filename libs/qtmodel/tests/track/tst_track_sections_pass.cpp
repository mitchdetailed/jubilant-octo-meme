// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's Track Sections pass and its inputs (02 sections 4.4 rule 13, 4.5 and 6.9; D-195, D-199 (e),
// D-204): the pass runs again whenever a lap index it reads changes, also when the number of lap indexes
// stays the same (a lap stretch published by LapStretchService replaces every index by a stretched copy), its
// dedupe is against the layer's latest run only (inputs that come back to an earlier run's run again), and a
// file closed and opened again is annotated afresh. Expectations come from the synthetic source's speed model
// (qtmodel_test_support.cpp) and the oval's geometry, never from the code under test.

#include "../qtmodel_test_support.hpp"
#include "../reports/report_track_fixtures.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QTest>
#include <cmath>
#include <numbers>
#include <optional>

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
};

QTEST_GUILESS_MAIN(TstTrackSectionsPass)
#include "tst_track_sections_pass.moc"
