// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M3 item 23 (D-210 (a)): TrackMapService with a MathsService and a Dispatcher (the application's service)
// reads, writes and scans the trackmaps folder on the TaskPool and never on the GUI thread: map() gives
// nullptr while the file is read and mapChanged announces it; setMap() replaces the cached map at once and
// the file follows in the order of the edits (merged behind a write in flight, backed up as asked); a write
// that fails is reported, read back and drops its undo step; a project change drops the reads in flight;
// writes still queued are made when the service goes; and a generated map is handed over only once the
// venue's own file is known. Expected files come from project::TrackMapFile written by the test, never from
// the service. The synchronous path (no Dispatcher) is tests/track/tst_track_map_service.cpp.

#include "../qtmodel_test_support.hpp"
#include "../reports/report_track_fixtures.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/project/track_map_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>

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
    Result<std::string> text = readWholeFile(path);
    return text ? std::move(text).value() : std::string(); // "" when absent
}

std::string fileText(const analysis::TrackMap& map) {
    project::TrackMapFile file;
    file.map = map;
    return file.toJson();
}

std::shared_ptr<const analysis::TrackMap> shared(analysis::TrackMap map) {
    return std::make_shared<const analysis::TrackMap>(std::move(map));
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

// A runtime, the maths the service takes its pool from, and a project.
struct World {
    explicit World(const std::string& name) : dir(test::scratchDir(name)), session(pathsFor(dir / "config")) {
        QVERIFY(session.loadDefaults().ok());
        QVERIFY(session.newProject(dir / "proj", "p").ok());
    }
    AppRuntime rt{runtimeOptions()};
    MathsService maths{rt.pool(), rt.dispatcher(), test::shippedUnits()};
    std::filesystem::path dir;
    ProjectSession session;
    std::filesystem::path trackmaps() const { return dir / "proj" / "trackmaps"; }
    std::filesystem::path backups() const { return dir / "proj" / "backup"; }
};

} // namespace

class TstTrackMapIo : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void readsOnThePoolAndAnnouncesTheMap() {
        World w("tmio-read");
        project::TrackMapFile file;
        file.map = ovalMap("Test Oval");
        QVERIFY(
            file.save(w.trackmaps() / pathFromUtf8(project::TrackMapFile::fileNameFor("Test Oval"))).ok());
        TrackMapService maps(&w.session, &w.maths, &w.rt.dispatcher());
        QVERIFY(maps.asynchronous());
        QSignalSpy changed(&maps, &TrackMapService::mapChanged);

        // Not read on this thread: nullptr at once, the read in flight, nothing counted or stamped yet.
        QVERIFY(maps.map("Test Oval") == nullptr);
        QVERIFY(maps.isLoading("Test Oval"));
        QCOMPARE(maps.fileReads(), 0);
        QCOMPARE(maps.mapStamp("Test Oval"), std::uint64_t{0});
        QCOMPARE(maps.pendingIo(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 30000);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("Test Oval"));
        QVERIFY(!maps.isLoading("Test Oval"));
        const auto map = maps.map("Test Oval");
        QVERIFY(map != nullptr);
        QCOMPARE(*map, file.map);
        QCOMPARE(maps.fileReads(), 1);
        const std::uint64_t stamp = maps.mapStamp("Test Oval");
        QVERIFY(stamp != 0);
        QVERIFY(maps.map("Test Oval") == map); // cached
        QCOMPARE(maps.fileReads(), 1);
        QCOMPARE(maps.mapStamp("Test Oval"), stamp);

        // An absent venue is read on the pool too, announced, and cached as no map.
        QVERIFY(maps.map("Elsewhere") == nullptr);
        QVERIFY(maps.isLoading("Elsewhere"));
        QTRY_VERIFY_WITH_TIMEOUT(!maps.isLoading("Elsewhere"), 30000);
        QCOMPARE(changed.count(), 2);
        QVERIFY(maps.map("Elsewhere") == nullptr);
        QVERIFY(maps.mapStamp("Elsewhere") != 0);
        QVERIFY(maps.mapStamp("Elsewhere") != stamp);
        QCOMPARE(maps.fileReads(), 1);

        // A broken file: a Messages line once it has been read.
        std::ofstream(w.trackmaps() / "broken.track.json") << "{ not json";
        QSignalSpy log(&maps, &TrackMapService::logMessage);
        QVERIFY(maps.map("Broken") == nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(log.count(), 1, 30000);
        QVERIFY(maps.map("Broken") == nullptr);

        // venues(): from a scan of the folder on the pool, announced.
        QSignalSpy scanned(&maps, &TrackMapService::venuesChanged);
        static_cast<void>(maps.venues());
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 30000);
        QCOMPARE(maps.venues(), QStringList{QStringLiteral("Test Oval")});

        // A project change drops the cache and the reads in flight: a read started now never lands.
        QVERIFY(maps.map("Late") == nullptr);
        QVERIFY(maps.isLoading("Late"));
        const int before = changed.count();
        w.session.closeProject();
        QVERIFY(!maps.isLoading("Late"));
        QCOMPARE(maps.pendingIo(), 0);
        QVERIFY(changed.count() > before); // the venues the cache held
        QTRY_VERIFY_WITH_TIMEOUT(quiet(w.rt), 30000);
        QCOMPARE(maps.mapStamp("Late"), std::uint64_t{0});
        QVERIFY(maps.map("Test Oval") == nullptr); // no project open now
    }

    void writesOnThePoolInTheOrderOfTheEdits() {
        World w("tmio-write");
        TrackMapService maps(&w.session, &w.maths, &w.rt.dispatcher());
        QSignalSpy changed(&maps, &TrackMapService::mapChanged);
        const std::filesystem::path path = maps.pathFor("Test Oval");

        // Three edits in one turn: the cache follows each at once; the first write is in flight, the next
        // two are merged behind it (the newest map, backed up because one of them asked).
        const auto first = shared(ovalMap("Test Oval"));
        analysis::TrackMap second = *first;
        second.sections.front().name = "Main Straight";
        analysis::TrackMap third = *first;
        third.sections.front().name = "Back Straight";
        QVERIFY(maps.setMap("Test Oval", first, true).ok());
        QVERIFY(maps.map("Test Oval") == first);
        QVERIFY(maps.setMap("Test Oval", shared(second), true).ok());
        QVERIFY(maps.setMap("Test Oval", shared(third)).ok());
        QCOMPARE(changed.count(), 3);
        QCOMPARE(maps.map("Test Oval")->sections.front().name, std::string("Back Straight"));
        QCOMPARE(maps.pendingIo(), 2); // one in flight, one held
        QTRY_COMPARE_WITH_TIMEOUT(maps.pendingIo(), 0, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(w.rt), 30000);
        QCOMPARE(readText(path), fileText(third));
        // The first write had nothing to back up; the merged one backed up the first map's file.
        QCOMPARE(filesIn(w.backups()), std::size_t{1});

        // A removal on the pool: the cache at once, the file moved to backup/ once it lands.
        QVERIFY(maps.setMap("Test Oval", nullptr).ok());
        QVERIFY(maps.map("Test Oval") == nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(maps.pendingIo(), 0, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(w.rt), 30000);
        QVERIFY(!std::filesystem::exists(path));
        QCOMPARE(filesIn(w.backups()), std::size_t{2});

        // An invalid map is still refused at once, and nothing is queued.
        analysis::TrackMap bad = *first;
        bad.sections.front().endM = bad.sections.front().startM + 5.0;
        QCOMPARE(maps.setMap("Test Oval", shared(bad)).error().code, ErrorCode::Invalid);
        QCOMPARE(maps.pendingIo(), 0);

        // A fresh synchronous service reads what was written.
        QVERIFY(maps.setMap("Test Oval", first).ok());
        QTRY_COMPARE_WITH_TIMEOUT(maps.pendingIo(), 0, 30000);
        TrackMapService again(&w.session);
        QVERIFY(!again.asynchronous());
        QVERIFY(again.map("Test Oval") != nullptr);
        QCOMPARE(*again.map("Test Oval"), *first);
    }

    // D-205 on the pool: a file under the venue's name that holds another venue's map is never overwritten.
    // The write fails when it lands: a Messages row, the venue read back from its file (no map of its own),
    // and the command that carried the edit is failed and obsolete, so the stack drops it (D-203 (n)).
    void aFailedWriteIsReportedReadBackAndDropsItsStep() {
        World w("tmio-fail");
        TrackMapService maps(&w.session, &w.maths, &w.rt.dispatcher());
        QVERIFY(maps.setMap("Pouhon", shared(ovalMap("Pouhon"))).ok());
        QTRY_COMPARE_WITH_TIMEOUT(maps.pendingIo(), 0, 30000);
        const std::filesystem::path squatted = maps.pathFor("Raidillon");
        std::filesystem::copy_file(maps.pathFor("Pouhon"), squatted);
        const std::string before = readText(squatted);

        QSignalSpy log(&maps, &TrackMapService::logMessage);
        QUndoStack& stack = w.session.undoStack();
        const int count = stack.count();
        auto* command = new TrackMapCommand(maps, "Raidillon", nullptr, shared(ovalMap("Raidillon")),
                                            QStringLiteral("Generate"));
        stack.push(command);
        QVERIFY(!command->failed()); // queued, not refused
        QCOMPARE(stack.count(), count + 1);
        QVERIFY(maps.map("Raidillon") != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(command->failed(), 30000);
        QVERIFY(command->isObsolete());
        QCOMPARE(command->lastError().value_or(Error{}).code, ErrorCode::Io);
        QTRY_VERIFY_WITH_TIMEOUT(maps.pendingIo() == 0 && quiet(w.rt), 30000);
        const auto reported = [&log] {
            for (const QList<QVariant>& line : log) {
                const QString text = line.at(0).toString();
                if (text.contains(QStringLiteral("could not be written")) &&
                    text.contains(QStringLiteral("Raidillon"))) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(reported());
        QVERIFY(maps.map("Raidillon") == nullptr); // read back: the file is not this venue's map
        QCOMPARE(readText(squatted), before);
        stack.undo(); // the obsolete step goes without running
        QCOMPARE(stack.count(), count);
        QVERIFY(maps.map("Raidillon") == nullptr);
        QCOMPARE(readText(squatted), before);
    }

    // Edits still queued when the service is destroyed are written then, newest last (D-190's rule).
    void queuedWritesAreMadeWhenTheServiceGoes() {
        World w("tmio-flush");
        const auto first = shared(ovalMap("Test Oval"));
        analysis::TrackMap edited = *first;
        edited.sections.front().name = "Main Straight";
        std::filesystem::path path;
        {
            TrackMapService maps(&w.session, &w.maths, &w.rt.dispatcher());
            path = maps.pathFor("Test Oval");
            QVERIFY(maps.setMap("Test Oval", first).ok());
            QVERIFY(maps.setMap("Test Oval", shared(edited)).ok()); // held behind the first
            QCOMPARE(maps.pendingIo(), 2);
        } // no event loop ran: neither write was delivered
        QCOMPARE(readText(path), fileText(edited));
        QTRY_VERIFY_WITH_TIMEOUT(quiet(w.rt), 30000);
        QCOMPARE(readText(path), fileText(edited)); // the older write, if it ran late, did not win
    }

    // The shell's auto-generation generates when mapFor() is null and writes the result when it is still
    // null: a generated map must reach it only once the venue's own file has been read. Here the cache is
    // dropped (as a project reopen does) after the generation has finished on the pool but before its result
    // was delivered: the hand-over waits for the venue's file and sees its map.
    void aGeneratedMapWaitsForTheVenuesOwnFile() {
        World w("tmio-generate");
        const std::filesystem::path circuit = test::repoRoot() / "fixtures" / "track" / "synthetic-circuit";
        const auto src = circuitSource(circuit, "Synthetic Circuit", *test::shippedUnits(), w.rt.cache());
        w.maths.addSource(src);
        project::TrackMapFile file;
        file.map = ovalMap("Synthetic Circuit");
        QVERIFY(
            file.save(w.trackmaps() / pathFromUtf8(project::TrackMapFile::fileNameFor("Synthetic Circuit")))
                .ok());
        TrackMapService maps(&w.session, &w.maths, &w.rt.dispatcher());
        TrackGenerationRequest request;
        request.lap = refOf(*src, kLaps, "Lap 1");
        request.method = analysis::TrackMethod::Gps;
        std::optional<bool> mapKnown; // at the hand-over: whether the venue's map was there
        QObject context;
        TrackGenerationStatus status;
        QVERIFY(QTest::qWaitFor(
            [&] {
                status = maps.generate(request, &context, [&](Result<analysis::GeneratedTrack> r) {
                    QVERIFY(r.ok());
                    mapKnown = maps.map("Synthetic Circuit") != nullptr;
                });
                return status.state != TrackGenerationState::Pending;
            },
            30000));
        QCOMPARE(static_cast<int>(status.state), static_cast<int>(TrackGenerationState::Started));
        // The generation finishes on the pool; its result waits in the Dispatcher (no event loop here).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!poolIdle(w.rt) && std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
        QVERIFY(poolIdle(w.rt));
        QVERIFY(w.rt.dispatcher().pending() > 0);
        QVERIFY(!mapKnown.has_value());
        maps.reload(); // nothing cached, nothing being read
        QVERIFY(!maps.isLoading("Synthetic Circuit"));
        QTRY_VERIFY_WITH_TIMEOUT(mapKnown.has_value(), 30000);
        QVERIFY(*mapKnown);
    }
};

QTEST_GUILESS_MAIN(TstTrackMapIo)
#include "tst_track_map_io.moc"
