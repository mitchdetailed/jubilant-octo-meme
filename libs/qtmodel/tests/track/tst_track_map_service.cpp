// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapService, TrackMapCommand, RenderService's track-map binding and SourceLoader's Track Sections pass
// (04 section 14 item 1, 06 section 13.5, 02 sections 4.5, 6.9 and 7; AC-M3-04 user sections persist and
// reload; D-199). The generation path is measured on the AC-M3-04 synthetic circuit
// (fixtures/track/synthetic-circuit, expectations from tools/gen_track_fixtures.py, never from the code under
// test): the GPS map lies within 2 m RMS of the true centreline through the GUI path too.

#include "../qtmodel_test_support.hpp"
#include "../reports/report_track_fixtures.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/project/track_map_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <fstream>

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

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = test::repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

std::size_t filesIn(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        n += e.is_regular_file() ? 1 : 0;
    }
    return n;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::shared_ptr<const analysis::TrackMap> shared(analysis::TrackMap map) {
    return std::make_shared<const analysis::TrackMap>(std::move(map));
}

std::filesystem::path circuitDir() {
    return test::repoRoot() / "fixtures" / "track" / "synthetic-circuit";
}

// The source with a Venue detail added.
std::shared_ptr<const model::DataSource> withVenue(const std::shared_ptr<const model::DataSource>& src,
                                                   const std::string& venue) {
    auto copy = std::make_shared<model::DataSource>(*src);
    copy->details.push_back(textDetail("Venue", venue));
    copy->baseDetails = copy->details;
    return copy;
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

} // namespace

class TstTrackMapService : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void mapsLoadFromTheProjectOnceAndBindByVenue() {
        const auto dir = test::scratchDir("tms-load");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        QVERIFY(session.newProject(dir / "proj", "p").ok());
        project::TrackMapFile file;
        file.map = ovalMap("Test Oval");
        const std::filesystem::path path = dir / "proj" / "trackmaps" / "test-oval.track.json";
        QVERIFY(file.save(path).ok());

        TrackMapService maps(&session);
        QCOMPARE(maps.pathFor("Test Oval"), path);
        const auto map = maps.map("Test Oval");
        QVERIFY(map != nullptr);
        QCOMPARE(*map, file.map);
        QCOMPARE(maps.fileReads(), 1);
        QVERIFY(maps.map("Test Oval") == map); // cached
        QCOMPARE(maps.fileReads(), 1);
        QVERIFY(maps.map("Elsewhere") == nullptr); // no file: nullptr, cached too
        QVERIFY(maps.map("Elsewhere") == nullptr);
        QCOMPARE(maps.fileReads(), 1);
        QCOMPARE(maps.venues(), QStringList{QStringLiteral("Test Oval")});

        AppRuntime rt(runtimeOptions());
        const auto src = withVenue(test::makeSource(test::SyntheticOptions{}, rt.cache()), "Test Oval");
        QCOMPARE(TrackMapService::venueOf(*src), std::string("Test Oval"));
        QVERIFY(maps.mapFor(*src) == map);

        // A broken file: nullptr and a Messages line.
        std::ofstream(dir / "proj" / "trackmaps" / "broken.track.json") << "{ not json";
        QSignalSpy log(&maps, &TrackMapService::logMessage);
        QVERIFY(maps.map("Broken") == nullptr);
        QCOMPARE(log.count(), 1);

        // A project change drops the cache and announces what it held.
        QSignalSpy changed(&maps, &TrackMapService::mapChanged);
        session.closeProject();
        QVERIFY(changed.count() >= 1);
        QVERIFY(maps.map("Test Oval") == nullptr); // no project open now
    }

    void setMapWritesBacksUpRemovesAndAnnounces() {
        const auto dir = test::scratchDir("tms-write");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        QVERIFY(session.newProject(dir / "proj", "p").ok());
        TrackMapService maps(&session);
        QSignalSpy changed(&maps, &TrackMapService::mapChanged);
        const std::filesystem::path path = maps.pathFor("Test Oval");

        const auto first = shared(ovalMap("Test Oval"));
        QVERIFY(maps.setMap("Test Oval", first, true).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("Test Oval"));
        project::TrackMapFile expected;
        expected.map = *first;
        QCOMPARE(readText(path), expected.toJson());
        QCOMPARE(filesIn(dir / "proj" / "backup"), std::size_t{0}); // nothing to back up yet

        analysis::TrackMap edited = *first;
        edited.sections.front().name = "Main Straight";
        QVERIFY(maps.setMap("Test Oval", shared(edited), true).ok());
        QCOMPARE(filesIn(dir / "proj" / "backup"),
                 std::size_t{1}); // the previous file (06 section 13.5 item 2)
        QCOMPARE(maps.map("Test Oval")->sections.front().name, std::string("Main Straight"));

        // A map that breaks the 10 m minimum is refused and nothing changes.
        analysis::TrackMap bad = edited;
        bad.sections.front().endM = bad.sections.front().startM + 5.0;
        QCOMPARE(maps.setMap("Test Oval", shared(bad)).error().code, ErrorCode::Invalid);
        QCOMPARE(maps.map("Test Oval")->sections.front().endM, edited.sections.front().endM);
        QCOMPARE(changed.count(), 2);

        // Removing moves the file to backup/.
        QVERIFY(maps.setMap("Test Oval", nullptr).ok());
        QVERIFY(!std::filesystem::exists(path));
        QCOMPARE(filesIn(dir / "proj" / "backup"), std::size_t{2});
        QVERIFY(maps.map("Test Oval") == nullptr);
        QVERIFY(maps.venues().isEmpty());

        // A fresh service reads what was written.
        QVERIFY(maps.setMap("Test Oval", first).ok());
        TrackMapService again(&session);
        QCOMPARE(*again.map("Test Oval"), *first);
    }

    void withoutAProjectMapsLiveInMemory() {
        TrackMapService maps(nullptr);
        QVERIFY(maps.pathFor("X").empty());
        QVERIFY(maps.map("X") == nullptr);
        QVERIFY(maps.setMap("X", shared(ovalMap("X"))).ok());
        QVERIFY(maps.map("X") != nullptr);
        QCOMPARE(maps.venues(), QStringList{QStringLiteral("X")});
        QCOMPARE(maps.fileReads(), 0);
    }

    void commandsUndoAndRedoTheFile() {
        const auto dir = test::scratchDir("tms-undo");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        QVERIFY(session.newProject(dir / "proj", "p").ok());
        TrackMapService maps(&session);
        const std::filesystem::path path = maps.pathFor("Test Oval");
        const auto generated = shared(ovalMap("Test Oval"));
        QUndoStack& stack = session.undoStack();
        auto* accept =
            new TrackMapCommand(maps, "Test Oval", nullptr, generated, QStringLiteral("Generate"), true);
        stack.push(accept);
        QVERIFY(!accept->failed());
        QVERIFY(std::filesystem::exists(path));
        // A user section, one command (02 section 7).
        analysis::TrackMap withUser = *generated;
        analysis::TrackSection user;
        user.id = "u1";
        user.set = "User";
        user.name = "T1 entry";
        user.startM = 150.0;
        user.endM = 220.0;
        user.userDefined = true;
        withUser.sections.push_back(user);
        stack.push(new TrackMapCommand(maps, "Test Oval", generated, shared(withUser),
                                       QStringLiteral("Add section")));
        QCOMPARE(maps.map("Test Oval")->sections.size(), withUser.sections.size());
        // AC-M3-04: user sections persist in trackmaps/<venue>.track.json and reload.
        {
            std::vector<std::string> log;
            const auto reloaded = project::TrackMapFile::load(path, log);
            QVERIFY(reloaded.ok());
            QCOMPARE(reloaded->map.sections.back(), user);
            TrackMapService fresh(&session);
            QCOMPARE(fresh.map("Test Oval")->sections.back(), user);
        }
        stack.undo();
        QCOMPARE(maps.map("Test Oval")->sections.size(), generated->sections.size());
        stack.undo();
        QVERIFY(!std::filesystem::exists(path));
        QVERIFY(maps.map("Test Oval") == nullptr);
        stack.redo();
        QVERIFY(std::filesystem::exists(path));
        stack.redo();
        QCOMPARE(maps.map("Test Oval")->sections.back().name, std::string("T1 entry"));
    }

    void renderServiceBindsTheMainSourcesVenueMap() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), test::shippedUnits());
        DistanceService distance(maths, rt.dispatcher());
        DataSelectionModel selection;
        RenderService render(rt.pool(), rt.dispatcher(), selection, maths, distance);
        const auto src = withVenue(test::makeSource(test::SyntheticOptions{}, rt.cache()), "Test Oval");
        maths.addSource(src);
        selection.sourcePublished(src);
        selection.setMain(test::rangeRef(*src, 1));
        project::ComponentSpec spec;
        spec.type = "track_position";
        QVERIFY(render.prepareInputs(spec, {}, selection.snapshot(), {}, nullptr, nullptr).trackMap ==
                nullptr);
        TrackMapService maps(nullptr);
        render.setTrackMaps(&maps);
        QCOMPARE(render.trackMaps(), &maps);
        QVERIFY(render.prepareInputs(spec, {}, selection.snapshot(), {}, nullptr, nullptr).trackMap ==
                nullptr);
        const auto map = shared(ovalMap("Test Oval"));
        QVERIFY(maps.setMap("Test Oval", map).ok());
        QVERIFY(render.prepareInputs(spec, {}, selection.snapshot(), {}, nullptr, nullptr).trackMap == map);
        // One StatCache per workspace (ReportModel's service).
        QVERIFY(render.statCache() != nullptr);
        QVERIFY(render.statCache() == render.statCache());
        QCOMPARE(&render.pool(), &rt.pool());
        QCOMPARE(&render.dispatcher(), &rt.dispatcher());
    }

    void generationFromGpsMatchesTheTrueCentreline() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), test::shippedUnits());
        const auto src = circuitSource(circuitDir(), "Synthetic Circuit", *test::shippedUnits(), rt.cache());
        maths.addSource(src);
        TrackMapService maps(nullptr, &maths, &rt.dispatcher());
        const CircuitTruth truth = readCircuitTruth(circuitDir());

        TrackGenerationRequest request;
        request.lap = refOf(*src, kLaps, "Lap 1");
        request.method = analysis::TrackMethod::Gps;
        std::optional<Result<analysis::GeneratedTrack>> got;
        QObject context;
        TrackGenerationStatus status;
        QVERIFY(QTest::qWaitFor(
            [&] {
                // The GPS chain heads may be maths outputs that are still being evaluated: try again.
                status = maps.generate(request, &context,
                                       [&](Result<analysis::GeneratedTrack> r) { got = std::move(r); });
                return status.state != TrackGenerationState::Pending;
            },
            20000));
        QCOMPARE(static_cast<int>(status.state), static_cast<int>(TrackGenerationState::Started));
        QVERIFY(QTest::qWaitFor([&] { return got.has_value(); }, 30000));
        QVERIFY2(got->ok(), got->ok() ? "" : got->error().toString().c_str());
        const analysis::TrackMap& map = (*got)->map;
        QCOMPARE(map.venue, std::string("Synthetic Circuit"));
        QCOMPARE(map.name, std::string("Synthetic Circuit"));
        QCOMPARE(static_cast<int>(map.generatedFrom.method), static_cast<int>(analysis::TrackMethod::Gps));
        QCOMPARE(map.generatedFrom.lap, request.lap.range.str());
        QVERIFY(map.sections.empty());
        // AC-M3-04 (a) through the GUI path: within 2 m RMS of the true centreline, length within 1 %.
        const double rms = rmsToPolyline(map.centre, truth.centre);
        QVERIFY2(rms <= 2.0, qPrintable(QStringLiteral("RMS %1 m").arg(rms)));
        QVERIFY(std::abs(map.lengthM - truth.lengthM) <= 0.01 * truth.lengthM);
        QVERIFY(!(*got)->report.diagnostics.empty()); // the 2 s dropout is reported (W-TRACK-DROPOUT)
    }

    void generationFromSpeedAndLateralG() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), test::shippedUnits());
        const auto src = circuitSource(circuitDir(), "Synthetic Circuit", *test::shippedUnits(), rt.cache());
        maths.addSource(src);
        TrackMapService maps(nullptr, &maths, &rt.dispatcher());
        const CircuitTruth truth = readCircuitTruth(circuitDir());
        TrackGenerationRequest request;
        request.lap = refOf(*src, kLaps, "Lap 1");
        request.method = analysis::TrackMethod::SpeedLatG;
        request.speed = "IMU Speed";
        std::optional<Result<analysis::GeneratedTrack>> got;
        QObject context;
        const TrackGenerationStatus status =
            maps.generate(request, &context, [&](Result<analysis::GeneratedTrack> r) { got = std::move(r); });
        QCOMPARE(static_cast<int>(status.state), static_cast<int>(TrackGenerationState::Started));
        QVERIFY(QTest::qWaitFor([&] { return got.has_value(); }, 30000));
        QVERIFY(got->ok());
        QCOMPARE(static_cast<int>((*got)->map.generatedFrom.method),
                 static_cast<int>(analysis::TrackMethod::SpeedLatG));
        // The IMU speed reads 0.8 % high (the fixture's README): the length follows it within 2 %.
        QVERIFY(std::abs((*got)->map.lengthM - truth.lengthM) <= 0.02 * truth.lengthM);
        QVERIFY((*got)->report.closureErrorM > 0.0); // reported, not gated

        // Missing inputs are named; the context going away drops the result.
        request.lateral = "No Such Channel";
        const TrackGenerationStatus missing =
            maps.generate(request, &context, [](Result<analysis::GeneratedTrack>) {});
        QCOMPARE(static_cast<int>(missing.state), static_cast<int>(TrackGenerationState::Failed));
        QVERIFY(missing.reason.contains(QStringLiteral("No Such Channel")));
        request.lap.range = model::RangeId{};
        QCOMPARE(static_cast<int>(maps.generate(request, &context, {}).state),
                 static_cast<int>(TrackGenerationState::Failed));
        TrackMapService noMaths(nullptr);
        QCOMPARE(static_cast<int>(noMaths.generate(request, &context, {}).state),
                 static_cast<int>(TrackGenerationState::Failed));
    }

    void pendingMathsInputsAreReportedThenStart() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), test::shippedUnits());
        test::SyntheticOptions o;
        o.gps = true;
        const auto src = test::makeSource(o, rt.cache());
        maths.addSource(src);
        TrackMapService maps(nullptr, &maths, &rt.dispatcher());
        TrackGenerationRequest request;
        request.lap = test::rangeRef(*src, 1);
        request.method = analysis::TrackMethod::SpeedLatG;
        request.lateral = "Throttle"; // any logged channel: the test is about Corr Speed being pending first
        QObject context;
        bool done = false;
        const TrackGenerationStatus first =
            maps.generate(request, &context, [&](Result<analysis::GeneratedTrack>) { done = true; });
        QVERIFY(first.state == TrackGenerationState::Pending || first.state == TrackGenerationState::Started);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return done || maps.generate(request, &context, [&](Result<analysis::GeneratedTrack>) {
                                       done = true;
                                   }).state == TrackGenerationState::Started;
            },
            20000));
        QVERIFY(QTest::qWaitFor([&] { return done; }, 30000));
    }

    void sourceLoaderBuildsTheTrackSectionsGroups() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), test::shippedUnits());
        DistanceService distance(maths, rt.dispatcher());
        SourceLoader loader(rt.pool(), rt.dispatcher(), test::shippedUnits());
        loader.setUseSidecars(false);
        const std::vector<std::string> ids{"circuit.whole-outing", "circuit.beacon-pair",
                                           "circuit.track-sections"};
        auto runner = analysis::RuleRunner::fromRuleIds(test::repoRoot() / "data" / "rules", ids);
        QVERIFY(runner.ok());
        loader.setRuleRunner(std::move(runner).value());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
        TrackMapService maps(nullptr);
        // An oval of exactly the synthetic lap length (2000 m): straights 685.84 m, turns of radius 100 m.
        const double radius = 100.0;
        const double straight = (2000.0 - 2.0 * std::numbers::pi * radius) / 2.0;
        QVERIFY(maps.setMap("Test Oval", shared(ovalMap("Test Oval", straight, radius))).ok());
        loader.setTrackMaps(&maps);

        const auto dir = test::scratchDir("tms-loader");
        const auto memory = withVenue(test::makeSource(test::SyntheticOptions{}, rt.cache()), "Test Oval");
        const std::filesystem::path ld = test::writeLd(*memory, dir, "oval");
        std::shared_ptr<const model::DataSource> latest;
        connect(&loader, &SourceLoader::sourcePublished, this,
                [&](std::shared_ptr<const model::DataSource> s) { latest = std::move(s); });
        connect(&loader, &SourceLoader::sourceReplaced, this,
                [&](std::shared_ptr<const model::DataSource> s) { latest = std::move(s); });
        loader.open(ld);
        const auto sectionsOf = [&](const std::string& path) -> const model::RangeGroup* {
            return latest == nullptr ? nullptr : latest->layers.front().ranges.find(path);
        };
        // Every lap (out, three flying, in) gets its five Default sections once all lap indexes exist.
        QVERIFY(QTest::qWaitFor(
            [&] {
                const model::RangeGroup* g = sectionsOf("Outings:Laps:Track Sections:Default");
                return g != nullptr && g->ranges.size() >= 5 * 4;
            },
            60000));
        QCOMPARE(TrackMapService::venueOf(*latest), std::string("Test Oval"));
        const model::RangeGroup* sections = sectionsOf("Outings:Laps:Track Sections:Default");
        const model::RangeGroup* laps = sectionsOf("Outings:Laps");
        QVERIFY(laps != nullptr && laps->ranges.size() == 5);
        // Lap index 1 (the first flying lap, 50 s from t = 40 s): Turn 1 starts at straight/2 = 342.92 m.
        const model::Range& lap = laps->ranges[1];
        const model::Range* turn1 = nullptr;
        for (const model::Range& r : sections->ranges) {
            if (r.name == "Turn 1" && r.start >= lap.start && r.start < lap.end) {
                turn1 = &r;
            }
        }
        QVERIFY(turn1 != nullptr);
        const double expected = 40.0 + lapTimeAtDistance(straight / 2.0, 2000.0, 50.0);
        const double startS = static_cast<double>(turn1->start) / 1e6;
        QVERIFY2(std::abs(startS - expected) < 0.1,
                 qPrintable(QStringLiteral("Turn 1 at %1 s, expected %2 s").arg(startS).arg(expected)));
        // The Corners/Straights category children (02 section 4.5).
        QVERIFY(sectionsOf("Outings:Laps:Track Sections:Default:Corners") != nullptr);

        // A map change re-runs phase 2: without a map the groups go.
        QVERIFY(maps.setMap("Test Oval", nullptr).ok());
        QVERIFY(QTest::qWaitFor([&] { return sectionsOf("Outings:Laps:Track Sections:Default") == nullptr; },
                                60000));
        loader.closeAll();
    }
};

QTEST_GUILESS_MAIN(TstTrackMapService)
#include "tst_track_map_service.moc"
