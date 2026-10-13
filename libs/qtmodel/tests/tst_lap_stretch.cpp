// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Lap-distance stretching state (D-016, D-172; 06 sections 2.1 and 13.3 item 1; 07 section 5.1): laps whose
// measured length (the unstretched DistanceIndex from Corr Dist) is within the tolerance of the track length
// get Range::stretch = track / length, published as a new snapshot (Corr Dist itself unchanged); a lap
// outside the tolerance keeps 1; the toolbar toggle (data.lap_stretch_toggle) clears and restores the factors
// and is view state (project.json, never the undo stack); the track length comes from setTrackLength or the
// source's "Venue Length" detail; the report feeds the Lap Editor. Also the Edit-menu commands edit.undo /
// edit.redo.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QSignalSpy>
#include <QTest>
#include <cmath>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

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

struct Workspace {
    AppRuntime rt;
    ProjectSession session;
    MathsService maths;
    DistanceService distance;
    SourceLoader loader;
    LapStretchService stretch;

    explicit Workspace(const std::filesystem::path& config)
        : rt([] {
              AppRuntimeOptions o;
              o.workers = 4;
              o.cacheBudget = std::size_t{256} << 20;
              return o;
          }()),
          session(pathsFor(config)), maths(rt.pool(), rt.dispatcher(), shippedUnits()),
          distance(maths, rt.dispatcher()), loader(rt.pool(), rt.dispatcher(), shippedUnits()),
          stretch(loader, distance, shippedUnits(), &session) {
        static_cast<void>(session.loadDefaults());
        static_cast<void>(maths.loadSettingsSchema(repoRoot() / "data"));
        loader.setUseSidecars(false);
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
    }

    std::shared_ptr<const model::DataSource> open(const std::filesystem::path& ld) {
        QSignalSpy published(&loader, &SourceLoader::sourcePublished);
        loader.open(ld);
        if (!QTest::qWaitFor([&] { return published.count() == 1; }, 30000)) {
            return nullptr;
        }
        return published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
    }
};

// Out lap, three flying laps measuring 102 %, 97 % and 110 % of the track, in lap.
SyntheticOptions stretchedLaps(const char* path) {
    SyntheticOptions o;
    o.path = path;
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    o.lapLengthScale = {1.0, 1.02, 0.97, 1.10, 1.0};
    o.trackLengthM = 2000.0;
    return o;
}

std::optional<double> stretchOf(const SourceLoader& loader, const model::SourceId& id, std::size_t lap) {
    const auto src = loader.source(id);
    return src == nullptr ? std::nullopt : rangeAt(*src, lap).stretch;
}

bool near(std::optional<double> v, double want) {
    return v.has_value() && std::abs(*v - want) < 0.002;
}

} // namespace

class TstLapStretch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    void lapsWithinTheToleranceAreStretched() {
        const auto dir = scratchDir("stretch-laps");
        Workspace w(dir / "config");
        AppRuntime& rt = w.rt;
        const auto written = makeSource(stretchedLaps("stretch.ld"), rt.cache());
        const auto src = w.open(writeLd(*written, dir, "stretch"));
        QVERIFY(src != nullptr);
        QVERIFY(w.stretch.enabled()); // the project default (D-016)
        QCOMPARE(w.stretch.settings().tolerance, 0.05);
        QVERIFY(!w.stretch.trackLength(src->id).has_value()); // no Venue Length detail in this file
        // Without a track length nothing is stretched.
        QTest::qWait(50);
        for (std::size_t i = 0; i < rangeCount(*src); ++i) {
            QVERIFY(!stretchOf(w.loader, src->id, i).has_value());
        }
        QSignalSpy report(&w.stretch, &LapStretchService::reportChanged);
        w.stretch.setTrackLength(src->id, 2000.0);
        QCOMPARE(*w.stretch.trackLength(src->id), 2000.0);
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 1), 2000.0 / 2040.0), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 2), 2000.0 / 1940.0), 30000);
        QVERIFY(!stretchOf(w.loader, src->id, 3).has_value()); // +10 %: outside the 5 % tolerance
        QVERIFY(report.count() >= 1);
        // The report of the Lap Editor (06 section 13.3 item 1).
        const std::vector<analysis::LapStretchRow> rows = w.stretch.report(src->id);
        QCOMPARE(rows.size(), rangeCount(*src));
        QVERIFY(rows[1].applied && rows[1].withinTolerance);
        QVERIFY(std::abs(rows[1].percent() - (2000.0 / 2040.0 - 1.0) * 100.0) < 0.2);
        QVERIFY(!rows[3].applied && !rows[3].withinTolerance);
        QCOMPARE(rows[3].stretch, 1.0);
        QVERIFY(std::abs(rows[3].lengthM - 2200.0) < 5.0);
        // The distance chain uses the stretched lengths; Corr Dist itself is never stretched.
        const auto chain = w.distance.chain(rangeRef(*w.loader.source(src->id), 1), Priority::Visible);
        QTRY_VERIFY_WITH_TIMEOUT(
            w.distance.chain(rangeRef(*w.loader.source(src->id), 1), Priority::Visible) != nullptr, 30000);
        static_cast<void>(chain);

        // The toolbar toggle (data.lap_stretch_toggle): view state in project.json, never on the undo stack.
        CommandRegistry registry;
        registerLapStretchCommand(registry, w.stretch);
        QVERIFY(registry.find(QStringLiteral("data.lap_stretch_toggle"))->checked());
        QSignalSpy toggled(&w.stretch, &LapStretchService::enabledChanged);
        QVERIFY(registry.invoke(QStringLiteral("data.lap_stretch_toggle")));
        QCOMPARE(toggled.count(), 1);
        QVERIFY(!w.stretch.enabled());
        QVERIFY(!w.session.projectFile().lapStretchSettings().enabled);
        QVERIFY(!registry.find(QStringLiteral("data.lap_stretch_toggle"))->checked());
        QCOMPARE(w.session.undoStack().count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!stretchOf(w.loader, src->id, 1).has_value(), 30000);
        QVERIFY(!stretchOf(w.loader, src->id, 2).has_value());
        QVERIFY(!w.stretch.report(src->id)[1].applied);
        QVERIFY(w.stretch.report(src->id)[1].withinTolerance); // the test alone, also while off
        QVERIFY(registry.invoke(QStringLiteral("data.lap_stretch_toggle")));
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 1), 2000.0 / 2040.0), 30000);

        // The tolerance is an options edit (undoable): 12 % takes the +10 % lap in.
        project::ProjectFile pf = w.session.projectFile();
        pf.options = JsonDoc::parse(R"({"lapStretch":{"enabled":true,"tolerancePct":12}})").value();
        w.session.undoStack().push(new SetProjectOptionsCommand(w.session, pf, QStringLiteral("Options")));
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 3), 2000.0 / 2200.0), 30000);
        w.session.undoStack().undo();
        QTRY_VERIFY_WITH_TIMEOUT(!stretchOf(w.loader, src->id, 3).has_value(), 30000);
        QVERIFY(w.stretch.enabled()); // undoing the options edit never rewinds the toggle
    }

    void venueLengthDetailIsTheTrackLength() {
        const auto dir = scratchDir("stretch-venue");
        Workspace w(dir / "config");
        SyntheticOptions o = stretchedLaps("venue.ld");
        o.venueLengthM = 2000.0;
        const auto written = makeSource(o, w.rt.cache());
        const auto src = w.open(writeLd(*written, dir, "venue"));
        QVERIFY(src != nullptr);
        QVERIFY(w.stretch.trackLength(src->id).has_value());
        QVERIFY(std::abs(*w.stretch.trackLength(src->id) - 2000.0) < 1e-6);
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 2), 2000.0 / 1940.0), 30000);
        // An explicit length wins; clearing it hands back to the detail.
        w.stretch.setTrackLength(src->id, 1900.0);
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 2), 1900.0 / 1940.0), 30000);
        w.stretch.setTrackLength(src->id, std::nullopt);
        QTRY_VERIFY_WITH_TIMEOUT(near(stretchOf(w.loader, src->id, 2), 2000.0 / 1940.0), 30000);
    }

    void withoutASessionTheServiceHoldsTheSettings() {
        const auto dir = scratchDir("stretch-nosession");
        Workspace w(dir / "config");
        LapStretchService alone(w.loader, w.distance, shippedUnits());
        QVERIFY(alone.enabled());
        QSignalSpy toggled(&alone, &LapStretchService::enabledChanged);
        alone.toggle();
        QVERIFY(!alone.enabled());
        QCOMPARE(toggled.count(), 1);
        analysis::LapStretchSettings s;
        s.enabled = true;
        s.tolerance = 0.2;
        alone.setSettings(s);
        QVERIFY(alone.enabled());
        QCOMPARE(alone.settings().tolerance, 0.2);
        QCOMPARE(toggled.count(), 2);
    }

    // edit.undo / edit.redo (06 section 14.4, D-032): enabled while the stack can undo / redo.
    void editCommandsDriveTheStack() {
        const auto dir = scratchDir("stretch-edit");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        CommandRegistry registry;
        registerEditCommands(registry, session.undoStack());
        QCOMPARE(registry.find(QStringLiteral("edit.undo"))->defaultKeys,
                 QStringList{QStringLiteral("Ctrl+Z")});
        QCOMPARE(registry.find(QStringLiteral("edit.redo"))->defaultKeys,
                 QStringList{QStringLiteral("Ctrl+Y")});
        QVERIFY(!registry.isEnabled(QStringLiteral("edit.undo")));
        QVERIFY(!registry.invoke(QStringLiteral("edit.undo")));
        project::ChannelOverrides::Entry e;
        e.dps = 1;
        session.undoStack().push(new SetOverrideCommand(session, "Gear", e));
        QVERIFY(registry.isEnabled(QStringLiteral("edit.undo")));
        QVERIFY(registry.invoke(QStringLiteral("edit.undo")));
        QVERIFY(session.overrides().channels.empty());
        QVERIFY(registry.isEnabled(QStringLiteral("edit.redo")));
        QVERIFY(registry.invoke(QStringLiteral("edit.redo")));
        QVERIFY(session.overrides().find("Gear") != nullptr);
    }
};

QTEST_GUILESS_MAIN(TstLapStretch)
#include "tst_lap_stretch.moc"
