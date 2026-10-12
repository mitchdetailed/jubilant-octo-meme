// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService and DistanceService (03 section 1, 06 section 4.5; D-016, D-140; AC-M1-08, AC-M1-10,
// AC-M1-12): the built-in corrected speed/distance module registered per source and prefetched at
// Background, settings validated against the module schema, one posted completion per evaluation, Error
// without a speed channel (the "no distance data" path), versions kept across a sidecar snapshot, enable/
// disable; the per-lap DistanceIndex and the outing DistanceChain; nothing blocks a NoBlock (GUI) thread.

#include "qtmodel_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(esses::maths::ChannelKey)
Q_DECLARE_METATYPE(esses::model::SourceId)

namespace {

std::atomic<int> gAsserts{0};
void recordingHandler(const char*, const char*, int, const char*) {
    gAsserts.fetch_add(1);
}

AppRuntimeOptions runtimeOptions(unsigned workers = 4) {
    AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

SyntheticOptions source(const char* path, bool wheels = true) {
    SyntheticOptions o;
    o.path = path;
    o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
    o.wheelSpeeds = wheels;
    return o;
}

maths::ChannelKey keyOf(const model::DataSource& src, const char* name) {
    return maths::ChannelKey::make(src.id, model::kNormalLayer, name);
}

model::OpaqueJson readJson(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto doc = model::OpaqueJson::parse(ss.str());
    return doc ? std::move(doc).value() : model::OpaqueJson{};
}

} // namespace

class TstMathsDistance : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<maths::ChannelKey>("esses::maths::ChannelKey");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    // AC-M1-10: registered on addSource and prefetched in the Background band; distance mode is populated
    // without any request.
    void addSourcePrefetchesAtBackground() {
        AppRuntime rt(runtimeOptions(2));
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        QVERIFY(maths.loadSettingsSchema(repoRoot() / "data").ok());
        const auto src = makeSource(source("m-a.ld"), rt.cache());
        // Occupy both workers so the prefetch stays queued where it can be observed.
        std::atomic<bool> release{false};
        std::atomic<int> parked{0};
        struct Release { // frees the workers on every exit path, before the runtime joins them
            std::atomic<bool>& flag;
            ~Release() { flag.store(true); }
        } releaseOnExit{release};
        for (int i = 0; i < 2; ++i) {
            rt.pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                parked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return 0;
            });
        }
        QTRY_COMPARE(parked.load(), 2);
        QSignalSpy ready(&maths, &MathsService::channelReady);
        maths.addSource(src);
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "Corr Speed"));
        QVERIFY(maths.isMathsChannel(src->id, "", "corr dist")); // case-folded, "" = Normal
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "Wheel Speed FL"));
        QCOMPARE(maths.outputs(src->id, "Normal").size(), std::size_t{2});
        const TaskPool::Stats stats = rt.pool().stats();
        QVERIFY(stats.queued[static_cast<std::size_t>(Priority::Background)] >= 1);
        QCOMPARE(stats.queued[static_cast<std::size_t>(Priority::Visible)], std::size_t{0});
        release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
        QCOMPARE(maths.status(keyOf(*src, "Corr Speed")).state, maths::MathState::Ok);
        QCOMPARE(ready.count(), 2); // one completion per output
        const auto handle = maths.require(keyOf(*src, "Corr Dist"), Priority::Visible);
        QVERIFY(handle.has_value());
        const auto desc = maths.descriptor(keyOf(*src, "Corr Dist"));
        QVERIFY(desc.has_value());
        QCOMPARE(desc->name, std::string("Corr Dist"));
        QCOMPARE(desc->source, model::ChannelSource::Maths);
        // Many requires of a ready output post nothing new.
        for (int i = 0; i < 20; ++i) {
            QVERIFY(maths.require(keyOf(*src, "Corr Speed"), Priority::Visible).has_value());
        }
        QTest::qWait(50);
        QCOMPARE(ready.count(), 2);
    }

    void settingsAreValidatedAgainstTheSchema() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        QCOMPARE(maths.validateCorrectedSpeedSettings({}).error().code, ErrorCode::NotFound); // no schema yet
        QVERIFY(maths.loadSettingsSchema(repoRoot() / "data").ok());
        QVERIFY(maths.validateCorrectedSpeedSettings({}).ok()); // the module defaults
        const std::filesystem::path examples =
            repoRoot() / "schemas" / "examples" / "builtin-esses.corrected_speed_distance-1";
        int valid = 0;
        for (const auto& e : std::filesystem::directory_iterator(examples / "valid")) {
            QVERIFY2(maths.validateCorrectedSpeedSettings(readJson(e.path())).ok(),
                     e.path().string().c_str());
            ++valid;
        }
        int invalid = 0;
        for (const auto& e : std::filesystem::directory_iterator(examples / "invalid")) {
            const Result<void> r = maths.validateCorrectedSpeedSettings(readJson(e.path()));
            QVERIFY2(!r.ok(), e.path().string().c_str());
            ++invalid;
        }
        QVERIFY(valid > 0 && invalid > 0);
        // Invalid project settings: the module registers no output and Messages gets E-CSD-SETTINGS.
        QSignalSpy messages(&maths, &MathsService::moduleMessage);
        maths.setCorrectedSpeedSettings(*model::OpaqueJson::parse(R"({"method": "bogus"})"));
        const auto src = makeSource(source("m-settings.ld"), rt.cache());
        maths.addSource(src);
        QVERIFY(messages.count() >= 1);
        QCOMPARE(messages.first().at(1).toString(), QStringLiteral("E-CSD-SETTINGS"));
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "Corr Speed"));
        QCOMPARE(maths.status(keyOf(*src, "Corr Speed")).state, maths::MathState::Error);
        // Valid settings again: the outputs come back and evaluate.
        maths.setCorrectedSpeedSettings({});
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "Corr Speed"));
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
    }

    void noSpeedChannelMeansNoDistance() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        DistanceService distance(maths, rt.dispatcher());
        const auto src = makeSource(source("m-nospeed.ld", false), rt.cache());
        QSignalSpy messages(&maths, &MathsService::moduleMessage);
        QSignalSpy none(&distance, &DistanceService::noDistance);
        maths.addSource(src);
        distance.prefetch(src->id);
        QTRY_COMPARE_WITH_TIMEOUT(distance.state(src->id), DistanceState::NoDistance, 30000);
        QCOMPARE(none.count(), 1);
        QCOMPARE(maths.status(keyOf(*src, "Corr Speed")).state, maths::MathState::Error);
        QTRY_COMPARE(maths.status(keyOf(*src, "Corr Dist")).state, maths::MathState::Error);
        QVERIFY(!messages.isEmpty());
        QVERIFY(distance.chain(rangeRef(*src, 2), Priority::Interactive) == nullptr);
        QVERIFY(distance.index(rangeRef(*src, 2), Priority::Interactive) == nullptr);
        QVERIFY(!maths.require(keyOf(*src, "Corr Dist"), Priority::Visible).has_value());
    }

    // D-140: a new snapshot sharing the columns (a sidecar commit) keeps every node version: nothing re-runs.
    void replaceSourceKeepsVersions() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        const auto src = makeSource(source("m-replace.ld"), rt.cache());
        maths.addSource(src);
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
        const maths::Version before = maths.status(keyOf(*src, "Corr Dist")).version;
        model::SidecarDocument doc = src->sidecar;
        model::RangeOverride ov;
        ov.groupPath = "Outings:Laps";
        ov.startMarker = rangeAt(*src, 1).startMarker;
        ov.trusted = false;
        doc.setRangeOverride("Normal", ov);
        const auto next =
            src->withSidecar(doc, nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction());
        QSignalSpy ready(&maths, &MathsService::channelReady);
        maths.replaceSource(next);
        QCOMPARE(maths.source(src->id), next);
        QCOMPARE(maths.status(keyOf(*src, "Corr Dist")).state, maths::MathState::Ok);
        QCOMPARE(maths.status(keyOf(*src, "Corr Dist")).version, before);
        QVERIFY(maths.require(keyOf(*src, "Corr Dist"), Priority::Visible).has_value());
        maths.removeSource(src->id);
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "Corr Dist"));
        QVERIFY(maths.source(src->id) == nullptr);
    }

    void disableAndEnableTheModule() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        const auto src = makeSource(source("m-enable.ld"), rt.cache());
        maths.addSource(src);
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
        maths.setCorrectedSpeedEnabled(false);
        QCOMPARE(maths.status(keyOf(*src, "Corr Dist")).state, maths::MathState::Disabled);
        QVERIFY(!maths.require(keyOf(*src, "Corr Dist"), Priority::Visible).has_value());
        maths.setCorrectedSpeedEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
    }

    // The per-lap index (AC-M1-08 through the service) and the outing chain (D-134).
    void distanceIndexesAndChain() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        DistanceService distance(maths, rt.dispatcher());
        const auto src = makeSource(source("m-dist.ld"), rt.cache());
        QSignalSpy ready(&distance, &DistanceService::distanceReady);
        QCOMPARE(distance.state(src->id), DistanceState::Unknown);
        maths.addSource(src);
        distance.prefetch(src->id);
        QCOMPARE(distance.state(src->id), DistanceState::Pending);
        for (std::size_t i = 0; i < 5; ++i) {
            QTRY_VERIFY_WITH_TIMEOUT(distance.index(rangeRef(*src, i), Priority::Interactive) != nullptr,
                                     30000);
        }
        QCOMPARE(distance.state(src->id), DistanceState::Ready);
        QVERIFY(ready.count() >= 5);
        const auto idx = distance.index(rangeRef(*src, 2), Priority::Interactive);
        QCOMPARE(idx->lapStart(), rangeAt(*src, 2).start);
        QVERIFY(std::abs(idx->lengthM() - 2000.0) < 2.0);
        for (const double d : {0.0, 100.0, 777.25, 1500.0, 1990.0}) {
            const double t = idx->timeAt(d);
            QVERIFY(std::abs(idx->distAt(idx->lapStart() + static_cast<Micros>(std::llround(t * 1e6))) - d) <=
                    0.25 + 1e-6);
        }
        // The chain of Lap 2: origin at its start, the whole outing (5 x 2 km) around it.
        const auto chain = distance.chain(rangeRef(*src, 2), Priority::Interactive);
        QVERIFY(chain != nullptr);
        QVERIFY(chain->complete());
        QCOMPARE(chain->origin(), rangeAt(*src, 2).start);
        QCOMPARE(chain->pieces().size(), std::size_t{5});
        QVERIFY(std::abs(chain->extent().a + 4000.0) < 4.0);
        QVERIFY(std::abs(chain->extent().b - 6000.0) < 6.0);
        QCOMPARE(chain->distAt(rangeAt(*src, 2).start), 0.0);
        QCOMPARE(distance.chain(rangeRef(*src, 2), Priority::Interactive), chain); // cached
        // The outing range: origin at the outing start.
        const auto outing = distance.chain(rangeRef(*src, 0, "Outings"), Priority::Interactive);
        QVERIFY(outing != nullptr);
        QCOMPARE(outing->origin(), Micros{0});
        QVERIFY(std::abs(outing->extent().b - 10000.0) < 10.0);
        distance.forgetSource(src->id);
        QCOMPARE(distance.state(src->id), DistanceState::Unknown);
    }

    // A lap index queued at Background (the prefetch at publication) and asked for again at Visible moves to
    // the Visible band; a request for the same range id with other bounds gets a task of its own instead of
    // joining the queued one (its task key carries the whole DistanceIndexKey).
    void queuedIndexBuildsAreRaisedAndKeyedByTheirBounds() {
        AppRuntime rt(runtimeOptions(2));
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        DistanceService distance(maths, rt.dispatcher());
        const auto src = makeSource(source("m-raise.ld"), rt.cache());
        maths.addSource(src);
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state == maths::MathState::Ok, 30000);
        QVERIFY(maths.require(keyOf(*src, "Corr Dist"), Priority::Visible).has_value());
        QTRY_VERIFY_WITH_TIMEOUT(rt.pool().queuedTasks("").empty() && rt.pool().stats().running == 0, 30000);

        std::atomic<bool> release{false};
        std::atomic<int> parked{0};
        struct Release { // frees the workers on every exit path, before the runtime joins them
            std::atomic<bool>& flag;
            ~Release() { flag.store(true); }
        } releaseOnExit{release};
        for (int i = 0; i < 2; ++i) {
            rt.pool().submit(Priority::Interactive, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                parked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return 0;
            });
        }
        QTRY_COMPARE(parked.load(), 2);

        distance.prefetch(src->id);
        const std::string prefix = "distidx/" + src->id.hex() + "/";
        const auto bandOf = [&](const std::string& part) {
            std::vector<Priority> out;
            for (const TaskPool::QueuedTask& t : rt.pool().queuedTasks(prefix)) {
                if (t.key.find(part) != std::string::npos) {
                    out.push_back(t.priority);
                }
            }
            return out;
        };
        QCOMPARE(rt.pool().queuedTasks(prefix).size(), rangeCount(*src));
        const std::string lap2 = rangeAt(*src, 2).id.str();
        QCOMPARE(bandOf(lap2), std::vector<Priority>{Priority::Background});
        QVERIFY(distance.index(rangeRef(*src, 2), Priority::Visible) == nullptr); // still queued
        QCOMPARE(bandOf(lap2), std::vector<Priority>{Priority::Visible});
        QCOMPARE(bandOf(rangeAt(*src, 3).id.str()), std::vector<Priority>{Priority::Background});

        // The same range id ending 1 s earlier in a newer snapshot.
        auto next = std::make_shared<model::DataSource>(*src);
        model::RangeGroup* laps = next->layers.front().ranges.find("Outings:Laps");
        QVERIFY(laps != nullptr);
        laps->ranges[2].end -= 1'000'000;
        ++next->version;
        maths.replaceSource(next);
        QVERIFY(distance.index(rangeRef(*next, 2), Priority::Visible) == nullptr);
        const std::vector<Priority> both = bandOf(lap2);
        QCOMPARE(both.size(), std::size_t{2}); // two tasks, one per key
        release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(distance.index(rangeRef(*next, 2), Priority::Visible) != nullptr, 30000);
        const auto idx = distance.index(rangeRef(*next, 2), Priority::Visible);
        QCOMPARE(idx->key().end, laps->ranges[2].end);
    }

    // AC-M1-12: the whole publish -> maths -> distance path runs on a NoBlock thread without a blocking wait.
    void nothingBlocksTheGuiThread() {
#if ESSES_ASSERTS_ENABLED
        const AssertHandler previous = setAssertHandler(&recordingHandler);
        gAsserts.store(0);
        {
            AppRuntimeOptions o = runtimeOptions();
            o.markGuiThreadNoBlock = true;
            AppRuntime rt(o);
            QVERIFY(ThreadRole::isNoBlock());
            MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
            DistanceService distance(maths, rt.dispatcher());
            const auto src = makeSource(source("m-noblock.ld"), rt.cache());
            maths.addSource(src);
            distance.prefetch(src->id);
            QTRY_VERIFY_WITH_TIMEOUT(distance.chain(rangeRef(*src, 2), Priority::Visible) != nullptr &&
                                         distance.chain(rangeRef(*src, 2), Priority::Visible)->complete(),
                                     30000);
            QVERIFY(maths.require(keyOf(*src, "Corr Speed"), Priority::Visible).has_value());
        }
        ThreadRole::set(ThreadRole::Normal);
        setAssertHandler(previous);
        QCOMPARE(gAsserts.load(), 0);
#else
        QSKIP("assertions are compiled out in this configuration");
#endif
    }
};

QTEST_GUILESS_MAIN(TstMathsDistance)
#include "tst_maths_distance.moc"
