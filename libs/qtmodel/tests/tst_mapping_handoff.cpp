// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's Mapping-panel hand-off (05 sections 2 and 8.1, 06 section 13.8; D-028, D-029; AC-M2-11 at
// the model level): with the hand-off on, a CSV with two unknown units ends task A with mappingRequired()
// instead of publishing (nothing waits); the panel's decision, recorded as one undoable "Resolve mapping"
// macro (the analysis name as a project Mapping, the unit map saved for the importer), submits task B and the
// source publishes with the units resolved; with that unit map in the project a second file opens silently;
// Cancel drops the pending open, Open anyway publishes unitless; without the hand-off the open is headless
// (unitless with open.unit_unknown); the user's text profiles register beside the built-in ones.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/registry.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QSignalSpy>
#include <QTest>
#include <fstream>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)
Q_DECLARE_METATYPE(esses::formats::PendingResolution)

namespace {

// Two unknown units (zorks, blips) and one the registry knows (05 section 8.1).
constexpr std::string_view kCsv = "Time,Boost,Coolant,Speed\n"
                                  "s,zorks,blips,km/h\n"
                                  "0,100,20,36\n"
                                  "0.5,150,21,72\n"
                                  "1.0,200,22,108\n"
                                  "1.5,210,23,110\n";

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

// The CSV with a last row of its own: identical content would be the same source (its quick hash), which a
// second open only reports as already open.
std::filesystem::path writeCsv(const std::filesystem::path& dir, const std::string& name) {
    static int serial = 0;
    const std::filesystem::path p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out << kCsv << "2.0," << 220 + (++serial) << ",24,111\n";
    return p;
}

void writeText(const std::filesystem::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

formats::UnitMappingProfile completeMap() {
    formats::UnitMappingProfile m;
    m.importer = std::string(formats::kGenericCsvImporterId);
    m.rules.push_back(
        {formats::UnitMappingProfile::Match::NameAndUnit, "Boost", "zorks", "Pressure", "kPa", ""});
    m.rules.push_back(
        {formats::UnitMappingProfile::Match::UnitOnly, "", "blips", "Temperature", "deg C", ""});
    return m;
}

const model::Channel* channel(const model::DataSource& src, std::string_view name) {
    const model::DataLayer* l = src.layer(model::kNormalLayer);
    return l == nullptr ? nullptr : l->find(name);
}

bool logged(const QSignalSpy& spy, std::string_view code) {
    for (const QList<QVariant>& args : spy) {
        for (const formats::ImportLogEntry& e : args.at(1).value<std::vector<formats::ImportLogEntry>>()) {
            if (e.code == code) {
                return true;
            }
        }
    }
    return false;
}

struct Workspace {
    AppRuntime rt;
    ProjectSession session;
    MathsService maths;
    SourceLoader loader;
    DataSelectionModel selection;

    explicit Workspace(const std::filesystem::path& config)
        : rt([] {
              AppRuntimeOptions o;
              o.workers = 4;
              o.cacheBudget = std::size_t{256} << 20;
              return o;
          }()),
          session(pathsFor(config)), maths(rt.pool(), rt.dispatcher(), shippedUnits()),
          loader(rt.pool(), rt.dispatcher(), shippedUnits()), selection(&session) {
        static_cast<void>(session.loadDefaults());
        loader.setUseSidecars(false);
        loader.setMathsService(&maths);
        connectSourceLoader(loader, selection, nullptr);
        connectProjectSession(session, &maths, &loader);
    }
};

} // namespace

class TstMappingHandoff : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
        qRegisterMetaType<formats::PendingResolution>("esses::formats::PendingResolution");
    }

    void headlessOpenIsUnitless() {
        const auto dir = scratchDir("map-headless");
        Workspace w(dir / "config");
        QVERIFY(!w.loader.mappingHandOff());
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy required(&w.loader, &SourceLoader::mappingRequired);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        w.loader.open(writeCsv(dir, "a.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QCOMPARE(required.count(), 0);
        QVERIFY(logged(logs, "open.unit_unknown"));
    }

    // AC-M2-11: two unknown units block publish until resolved; the decision is undoable project edits; with
    // the saved unit map the next file opens silently.
    void unknownUnitsWaitForTheMappingPanel() {
        const auto dir = scratchDir("map-resolve");
        Workspace w(dir / "config");
        w.loader.setMappingHandOff(true);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy required(&w.loader, &SourceLoader::mappingRequired);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        w.loader.open(writeCsv(dir, "b.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 30000);
        QCOMPARE(published.count(), 0);
        QCOMPARE(w.loader.pendingOpens(), 0); // task A ended: no worker waits for the user
        QCOMPARE(w.loader.pendingMappings(), 1);
        const int id = required.first().at(0).toInt();
        const formats::PendingResolution* pending = w.loader.pendingMapping(id);
        QVERIFY(pending != nullptr);
        QCOMPARE(pending->rows.size(), std::size_t{2});
        QCOMPARE(pending->rows[0].loggedName, std::string("Boost"));
        QCOMPARE(pending->rows[0].vendorUnit, std::string("zorks"));
        QCOMPARE(pending->rows[1].vendorUnit, std::string("blips"));
        QCOMPARE(pending->importerId, std::string(formats::kGenericCsvImporterId));
        QVERIFY(pending->path.find("b.csv") != std::string::npos);

        formats::MappingDecision decision;
        decision.unitMap = completeMap();
        decision.saveUnitMap = true;
        decision.mappings.push_back(model::MappingRow{"Boost", "Boost Pressure", std::nullopt, false});
        applyMappingDecision(w.session, w.loader, id, decision);
        QCOMPARE(w.session.undoStack().count(), 1);
        QCOMPARE(w.session.undoStack().undoText(), QStringLiteral("Resolve mapping"));
        QCOMPARE(w.session.unitMaps().count("generic.csv"), std::size_t{1});
        QCOMPARE(w.session.mappings().rows.size(), std::size_t{1});
        QCOMPARE(w.loader.pendingMappings(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QVERIFY(logged(logs, "open.mapping_resolved"));
        const auto src = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
        const model::Channel* boost = channel(*src, "Boost Pressure");
        QVERIFY(boost != nullptr);
        QCOMPARE(boost->desc.loggedName, std::string("Boost"));
        QCOMPARE(shippedUnits()->unit(boost->desc.unit).symbol, std::string("Pa"));
        QVERIFY(channel(*src, "Coolant")->desc.statusText.empty());

        // The project's unit map reaches the loader (connectProjectSession): a second file opens silently.
        required.clear();
        published.clear();
        w.loader.open(writeCsv(dir, "c.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QCOMPARE(required.count(), 0);
        const auto second = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
        QVERIFY(channel(*second, "Boost Pressure") != nullptr); // the project Mapping renamed it at load

        // Undo removes both project edits; the mapping panel would be asked again.
        w.session.undoStack().undo();
        QCOMPARE(w.session.unitMaps().count("generic.csv"), std::size_t{0});
        QVERIFY(w.session.mappings().rows.empty());
        published.clear();
        w.loader.open(writeCsv(dir, "d.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 30000);
        QCOMPARE(published.count(), 0);
    }

    void cancelDropsTheOpenAndOpenAnywayPublishesUnitless() {
        const auto dir = scratchDir("map-cancel");
        Workspace w(dir / "config");
        w.loader.setMappingHandOff(true);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy failed(&w.loader, &SourceLoader::openFailed);
        QSignalSpy required(&w.loader, &SourceLoader::mappingRequired);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        w.loader.open(writeCsv(dir, "e.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 30000);
        w.loader.cancelMapping(required.first().at(0).toInt());
        QCOMPARE(w.loader.pendingMappings(), 0);
        QTest::qWait(50);
        QCOMPARE(published.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(logged(logs, "open.mapping_cancelled"));
        QCOMPARE(w.session.undoStack().count(), 0);
        // A cancelled id is gone: resolving it does nothing.
        w.loader.resolveMapping(required.first().at(0).toInt(), {});
        QCOMPARE(w.loader.pendingOpens(), 0);

        w.loader.open(writeCsv(dir, "f.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 2, 30000);
        formats::MappingDecision anyway;
        anyway.openAnyway = true;
        applyMappingDecision(w.session, w.loader, required.at(1).at(0).toInt(), anyway);
        QCOMPARE(w.session.undoStack().count(), 0); // nothing to record
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        const auto src = published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
        QVERIFY(channel(*src, "Boost") != nullptr);
        QVERIFY(!channel(*src, "Boost")->desc.statusText.empty()); // Unit unknown: "zorks"
        // SessionRestore waits for a pending Mapping panel before it restores the selection.
        w.loader.open(writeCsv(dir, "g.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 3, 30000);
        SessionRestore restore(w.session, w.loader, w.selection);
        QSignalSpy finished(&restore, &SessionRestore::finished);
        QCOMPARE(restore.start({}), 0); // no project: no previous data
        QTest::qWait(50);
        QCOMPARE(finished.count(), 0);
        w.loader.cancelMapping(required.at(2).at(0).toInt()); // its Import Log row re-checks
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    }

    // D-190: an open waiting for the Mapping panel or in flight when the project changes is cancelled: it
    // never publishes, and its mapping cannot be resolved into the next project's documents.
    void projectChangeCancelsOpens() {
        const auto dir = scratchDir("map-project-change");
        Workspace w(dir / "config");
        QVERIFY(w.session.newProject(dir / "A", "A").ok());
        w.loader.setMappingHandOff(true);
        QSignalSpy published(&w.loader, &SourceLoader::sourcePublished);
        QSignalSpy required(&w.loader, &SourceLoader::mappingRequired);
        QSignalSpy logs(&w.loader, &SourceLoader::importLog);
        w.loader.open(writeCsv(dir, "h.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 30000);
        const int waiting = required.first().at(0).toInt();
        w.loader.open(writeCsv(dir, "i.csv")); // in flight: its result needs this thread's event loop
        QCOMPARE(w.loader.pendingOpens(), 1);
        QVERIFY(w.session.newProject(dir / "B", "B").ok());
        QCOMPARE(w.loader.pendingOpens(), 0);
        QCOMPARE(w.loader.pendingMappings(), 0);
        QVERIFY(logged(logs, "open.mapping_cancelled"));
        QVERIFY(logged(logs, "open.cancelled"));
        formats::MappingDecision decision;
        decision.unitMap = completeMap();
        decision.saveUnitMap = true;
        decision.mappings.push_back(model::MappingRow{"Boost", "Boost Pressure", std::nullopt, false});
        applyMappingDecision(w.session, w.loader, waiting, decision);
        QCOMPARE(w.session.undoStack().count(), 0);
        QVERIFY(w.session.mappings().rows.empty());
        QVERIFY(w.session.unitMaps().empty());
        QTest::qWait(500);
        QCOMPARE(published.count(), 0);
        QCOMPARE(required.count(), 1);
        // Opens in the new project work as before.
        w.loader.open(writeCsv(dir, "j.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 2, 30000);
    }

    void userTextProfilesRegister() {
        const auto dir = scratchDir("map-profiles");
        Workspace w(dir / "config");
        std::ifstream in(repoRoot() / "data" / "profiles-text" / "generic.csv.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto replace = [&text](const std::string& from, const std::string& to) {
            const auto at = text.find(from);
            QVERIFY(at != std::string::npos);
            text.replace(at, from.size(), to);
        };
        replace("\"id\": \"generic.csv\"", "\"id\": \"user.csv\"");
        replace("\"confidence\": 30", "\"confidence\": 10");
        std::filesystem::create_directories(dir / "config" / "profiles-text");
        writeText(dir / "config" / "profiles-text" / "user.csv.json", text);
        writeText(dir / "config" / "profiles-text" / "broken.json", "{ nope");
        QVERIFY(w.loader.registry().find("text.profile:user.csv") == nullptr);
        const std::vector<std::string> log =
            w.loader.registerTextProfiles(repoRoot() / "data", dir / "config");
        QVERIFY(w.loader.registry().find("text.profile:user.csv") != nullptr);
        QVERIFY(w.loader.registry().find(formats::kGenericCsvImporterId) != nullptr); // built in, kept
        // The shipped profiles are skipped silently; the broken user file is logged.
        QCOMPARE(log.size(), std::size_t{1});
        QVERIFY(log.front().find("broken.json") != std::string::npos);
        // A second registration of the user profile is refused with a log line.
        const std::vector<std::string> again =
            w.loader.registerTextProfiles(repoRoot() / "data", dir / "config");
        QVERIFY(std::any_of(again.begin(), again.end(), [](const std::string& l) {
            return l.find("already registered") != std::string::npos;
        }));
    }
};

QTEST_GUILESS_MAIN(TstMappingHandoff)
#include "tst_mapping_handoff.moc"
