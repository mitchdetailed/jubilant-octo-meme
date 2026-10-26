// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The full MathsService (07 section 5.1; 03 sections 4.2-4.3, 7.3 and 11; 06 section 10; AC-M2-04, AC-M2-12):
// the project's modules, compat switches, mappings and aliases defined into every graph with only the edited
// cone re-versioned, the status model (Pending, Evaluating, Ok, Warning, Error, Disabled per item) with its
// signals, Recalculate (invalidate), local maths from the sidecar and the transient module, the GPS
// corrections registered for a layer with GPS channels, the live check against the Main source's scope and
// the debounced LiveDiagnostics feed, connectProjectSession, and the ChannelStatusModel rows: maths items and
// built-in outputs, constants, aliases, mappings and missing workbook references, with every ResolutionTrace
// kind (mapping, alias, case-fold, logged name, missing) rendered.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/live_diagnostics.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

AppRuntimeOptions runtimeOptions(unsigned workers = 4) {
    AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

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

maths::ChannelKey keyOf(const model::DataSource& src, const std::string& name) {
    return maths::ChannelKey::make(src.id, model::kNormalLayer, name);
}

maths::MathItem item(const std::string& name, const std::string& expr) {
    maths::MathItem i;
    i.name = name;
    i.expr = expr;
    return i;
}

maths::MathModule module(const std::string& id, std::vector<maths::MathItem> items) {
    maths::MathModule m;
    m.id = id;
    m.items = std::move(items);
    return m;
}

std::set<std::string> keysOf(const QSignalSpy& spy) {
    std::set<std::string> out;
    for (const QList<QVariant>& args : spy) {
        out.insert(args.at(0).value<maths::ChannelKey>().nameCf);
    }
    return out;
}

// Every task of the pool has completed and every result it posted has been delivered to this thread.
bool quiet(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0 && rt.dispatcher().pending() == 0;
}

bool hasCode(const maths::Diagnostics& diags, std::string_view code) {
    return std::any_of(diags.begin(), diags.end(),
                       [&](const maths::Diagnostic& d) { return d.code == code; });
}

// Parks every worker of the pool until released, so that queued evaluations stay observable (Evaluating).
struct Park {
    std::atomic<bool> release{false};
    std::atomic<int> parked{0};
    explicit Park(TaskPool& pool, int workers) {
        for (int i = 0; i < workers; ++i) {
            pool.submit(Priority::Interactive, TaskKey{}, CancelToken{}, [this](const CancelToken&) {
                parked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return 0;
            });
        }
    }
    ~Park() { release.store(true); }
};

const ChannelStatusModel::Row* rowNamed(const ChannelStatusModel& m, const QString& kind,
                                        const QString& channel) {
    for (const ChannelStatusModel::Row& r : m.rows()) {
        if (r.kind == kind && r.channel == channel) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

class TstMathsService : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<maths::ChannelKey>("esses::maths::ChannelKey");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    // AC-M2-04 through the service: a module edit announces (and re-versions) only the edited cone.
    void modulesDefineAndOnlyTheEditedConeChanges() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        SyntheticOptions o;
        o.path = "ms-a.ld";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        QSignalSpy changed(&maths, &MathsService::statusChanged);
        maths.setModules({module("user", {item("A", "'Throttle'[%] * 2"), item("B", "'A'[%] + 1"),
                                          item("C", "'Throttle'[%] / 2")})});
        QCOMPARE(keysOf(changed), (std::set<std::string>{"a", "b", "c"}));
        for (const char* n : {"A", "B", "C"}) {
            QCOMPARE(maths.status(keyOf(*src, n)).state, maths::MathState::Pending);
        }
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "b"));
        QSignalSpy ready(&maths, &MathsService::channelReady);
        static_cast<void>(maths.require(keyOf(*src, "B"), Priority::Visible)); // may finish at once
        QTRY_VERIFY_WITH_TIMEOUT(maths.status(keyOf(*src, "B")).state != maths::MathState::Evaluating, 30000);
        QVERIFY2(maths.status(keyOf(*src, "B")).state == maths::MathState::Ok,
                 maths.status(keyOf(*src, "B")).summary.c_str());
        // The upstream A was evaluated for B and applied with it (its own status moved on, one channelReady).
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*src, "A")).state, maths::MathState::Ok, 30000);
        const auto readyCount = [&ready](const char* nameCf) {
            return std::count_if(ready.begin(), ready.end(), [nameCf](const QList<QVariant>& args) {
                return args.at(0).value<maths::ChannelKey>().nameCf == nameCf;
            });
        };
        QCOMPARE(readyCount("a"), 1); // once per (output, version), also for an upstream applied with B
        QCOMPARE(readyCount("b"), 1);
        const maths::Version versionA = maths.status(keyOf(*src, "A")).version;
        changed.clear();
        // Setting the same modules again is a no-op; editing C re-versions C only.
        maths.setModules(maths.modules());
        QCOMPARE(changed.count(), 0);
        maths.setModules({module("user", {item("A", "'Throttle'[%] * 2"), item("B", "'A'[%] + 1"),
                                          item("C", "'Throttle'[%] / 4")})});
        QCOMPARE(keysOf(changed), (std::set<std::string>{"c"}));
        QCOMPARE(maths.status(keyOf(*src, "A")).state, maths::MathState::Ok);
        QCOMPARE(maths.status(keyOf(*src, "A")).version, versionA);
        QCOMPARE(maths.status(keyOf(*src, "B")).state, maths::MathState::Ok);
        QCOMPARE(maths.status(keyOf(*src, "C")).state, maths::MathState::Pending);
    }

    // 03 section 7.3: every state of the status model, per item, and Recalculate.
    void statusModelCoversEveryState() {
        AppRuntime rt(runtimeOptions(2));
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        SyntheticOptions o;
        o.path = "ms-b.ld";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*src, "Corr Dist")).state, maths::MathState::Ok, 30000);
        maths::MathModule off = module("off", {item("Off1", "'Throttle'[%] * 3")});
        off.enabled = false;
        maths.setModules(
            {module("user", {item("Ok1", "'Throttle'[%] * 2"), item("Warn1", "'Wheel Speed FL' * 2"),
                             item("Err1", "'Nope' * 2"), item("Dup", "1"), item("Dup", "2")}),
             off});
        QCOMPARE(maths.status(keyOf(*src, "Off1")).state, maths::MathState::Disabled);
        QCOMPARE(maths.status(keyOf(*src, "Err1")).state, maths::MathState::Error);
        QVERIFY(hasCode(maths.status(keyOf(*src, "Err1")).diags, "E-UNKNOWN-CHANNEL"));
        // Evaluating while the workers are busy; then Ok / Warning.
        {
            Park park(rt.pool(), 2);
            QTRY_COMPARE(park.parked.load(), 2);
            QSignalSpy changed(&maths, &MathsService::statusChanged);
            QVERIFY(!maths.require(keyOf(*src, "Ok1"), Priority::Visible).has_value());
            QCOMPARE(maths.status(keyOf(*src, "Ok1")).state, maths::MathState::Evaluating);
            QVERIFY(keysOf(changed).count("ok1") == 1); // Pending -> Evaluating is announced
            QVERIFY(!maths.require(keyOf(*src, "Warn1"), Priority::Visible).has_value());
        }
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*src, "Ok1")).state, maths::MathState::Ok, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*src, "Warn1")).state, maths::MathState::Warning, 30000);
        // The ignored duplicate is a Warning row; the head of "Dup" is the first definition.
        const std::vector<MathsService::StatusRow> rows = maths.statusRows();
        const auto dups =
            std::count_if(rows.begin(), rows.end(), [](const auto& r) { return r.name == "Dup"; });
        QCOMPARE(dups, 2);
        QVERIFY(std::any_of(rows.begin(), rows.end(), [](const MathsService::StatusRow& r) {
            return r.name == "Dup" && r.status.state == maths::MathState::Warning &&
                   hasCode(r.status.diags, "W-DUP-IGNORED");
        }));
        QVERIFY(std::any_of(rows.begin(), rows.end(), [](const MathsService::StatusRow& r) {
            return r.name == "Corr Dist" && r.kind == maths::ItemKind::Builtin && r.descriptor.has_value();
        }));
        // Recalculate: the module's outputs drop their columns and are evaluated again on demand.
        QSignalSpy changed(&maths, &MathsService::statusChanged);
        maths.invalidate("user");
        QVERIFY(keysOf(changed).count("ok1") == 1);
        QCOMPARE(maths.status(keyOf(*src, "Ok1")).state, maths::MathState::Pending);
        QCOMPARE(maths.status(keyOf(*src, "Corr Dist")).state, maths::MathState::Ok); // another module
        static_cast<void>(maths.require(keyOf(*src, "Ok1"), Priority::Visible));      // may finish at once
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*src, "Ok1")).state, maths::MathState::Ok, 30000);
        // Enabling the module brings its item back as Pending.
        off.enabled = true;
        std::vector<maths::MathModule> mods = maths.modules();
        mods.back() = off;
        maths.setModules(mods);
        QCOMPARE(maths.status(keyOf(*src, "Off1")).state, maths::MathState::Pending);
    }

    void compatResolverLocalAndTransient() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        SyntheticOptions o;
        o.path = "ms-c.ld";
        o.throttleLoggedName = "TPS";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        maths.setModules({module("user", {item("Pow", "-2^2"), item("Pedal2", "'Pedal' * 2")})});
        QCOMPARE(maths.status(keyOf(*src, "Pedal2")).state, maths::MathState::Error); // no 'Pedal' yet
        // The project's mappings resolve names (02 section 4.6): 'Pedal' -> 'Throttle'.
        model::MappingTable mappings;
        mappings.rows.push_back(model::MappingRow{"Pedal", "Throttle", std::nullopt, false});
        maths.setResolverTables(mappings, {});
        QCOMPARE(maths.status(keyOf(*src, "Pedal2")).state, maths::MathState::Pending);
        // The power switch re-versions the item with a sign and '^' (its parse differs), not the others
        // (03 section 2.3, D-185).
        const maths::Version before = maths.status(keyOf(*src, "Pow")).version;
        QSignalSpy changed(&maths, &MathsService::statusChanged);
        maths::MathsCompat compat;
        compat.unaryMinusAbovePower = true;
        maths.setCompat(compat);
        QVERIFY(keysOf(changed).count("pow") == 1);
        QVERIFY(keysOf(changed).count("pedal2") == 0);
        QVERIFY(!(maths.status(keyOf(*src, "Pow")).version == before));
        QVERIFY(maths.compat().unaryMinusAbovePower);
        // Local maths: set explicitly, then from the sidecar of a new snapshot (03 section 4.2).
        maths.setLocalModule(src->id, module("local", {item("Loc", "'Throttle'[%] + 1")}));
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "Loc"));
        maths.setLocalModule(src->id, std::nullopt);
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "Loc"));
        model::SidecarDocument doc = src->sidecar;
        doc.setLocalMaths("Normal", JsonDoc::parse(R"({"$schema":"urn:esses:maths-1","schemaVersion":1,)"
                                                   R"("id":"local","items":[{"kind":"expression",)"
                                                   R"("name":"FromSidecar","expr":"'Throttle'[%] * 3"}]})")
                                        .value());
        const auto withLocal =
            src->withSidecar(doc, nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction());
        maths.replaceSource(withLocal);
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "FromSidecar"));
        const std::vector<MathsService::StatusRow> rows = maths.statusRows();
        QVERIFY(std::any_of(rows.begin(), rows.end(), [](const MathsService::StatusRow& r) {
            return r.name == "FromSidecar" && r.moduleId == "local";
        }));
        // The transient module (never saved; dropped with the source).
        maths.setTransientModule(src->id, module("x", {item("Tmp", "'Throttle'[%] - 1")}));
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "Tmp"));
        const std::vector<MathsService::StatusRow> withTransient = maths.statusRows();
        QVERIFY(std::any_of(withTransient.begin(), withTransient.end(), [](const MathsService::StatusRow& r) {
            return r.name == "Tmp" && r.moduleId == "~transient";
        }));
        maths.setTransientModule(src->id, std::nullopt);
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "Tmp"));
        maths.removeSource(src->id);
        QVERIFY(maths.sources().empty());
        QVERIFY(maths.statusRows().empty());
    }

    // removeSource given a reference into the source whose last owners are the service's own (D-219): the id
    // is copied first, so every table of the source goes, the channelReady already announced too: the same
    // file added again (a reload of unchanged content: the same versions) is announced again. The source
    // frees into overwritten storage, so a read of the dead id would match nothing.
    void removeSourceOfAReferenceIntoTheRemovedSource() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        SyntheticOptions o;
        o.path = "ms-remove.ld";
        const auto original = makeSource(o, rt.cache());
        const maths::ChannelKey corrDist = keyOf(*original, "Corr Dist");
        QSignalSpy ready(&maths, &MathsService::channelReady);
        const auto announced = [&ready, &corrDist] {
            return std::count_if(ready.begin(), ready.end(), [&corrDist](const QList<QVariant>& args) {
                return args.at(0).value<maths::ChannelKey>() == corrDist;
            });
        };
        const model::DataSource* raw = nullptr;
        {
            const std::shared_ptr<const model::DataSource> copy = quarantinedCopy(*original);
            raw = copy.get();
            maths.addSource(copy); // Corr Dist prefetched
        }
        QTRY_COMPARE_WITH_TIMEOUT(announced(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(rt), 30000); // no task holds the source any more
        maths.removeSource(raw->id);
        QVERIFY(maths.sources().empty());
        QVERIFY(maths.statusRows().empty());
        maths.addSource(original);
        QTRY_COMPARE_WITH_TIMEOUT(announced(), 2, 30000);
    }

    // 03 section 9.2: the zero-fix step is registered for a layer that logs both GPS inputs.
    void gpsCorrectionsForLayersWithGps() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        QVERIFY(maths.loadSettingsSchema(repoRoot() / "data").ok());
        SyntheticOptions o;
        o.path = "ms-gps.ld";
        o.gps = true;
        const auto gps = makeSource(o, rt.cache());
        maths.addSource(gps);
        QCOMPARE(maths.outputs(gps->id, "Normal").size(), std::size_t{4});
        QVERIFY(maths.isMathsChannel(gps->id, "Normal", "GPS Latitude"));
        static_cast<void>(
            maths.require(keyOf(*gps, "GPS Latitude"), Priority::Visible)); // may finish at once
        QTRY_COMPARE_WITH_TIMEOUT(maths.status(keyOf(*gps, "GPS Latitude")).state, maths::MathState::Ok,
                                  30000);
        const std::vector<MathsService::StatusRow> rows = maths.statusRows();
        QVERIFY(std::any_of(rows.begin(), rows.end(), [](const MathsService::StatusRow& r) {
            return r.name == "GPS Latitude" && r.moduleId == "esses.gps_corrections";
        }));
        o.path = "ms-nogps.ld";
        o.gps = false;
        const auto plain = makeSource(o, rt.cache());
        maths.addSource(plain);
        QCOMPARE(maths.outputs(plain->id, "Normal").size(), std::size_t{2});
        QVERIFY(!maths.isMathsChannel(plain->id, "Normal", "GPS Latitude"));
    }

    // 03 section 11 items 2 and 12: the live check (data-less and against the Main source) and the debounce.
    void liveCheckAndDiagnosticsFeed() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        SyntheticOptions o;
        o.path = "ms-d.ld";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        maths::MathModule user = module("user", {item("Twice", "'Throttle'[%] * 2")});
        maths::MathConstant k;
        k.name = "Track Length";
        k.value.v = 2000.0;
        k.value.unit = *shippedUnits()->find("m");
        user.constants.push_back(k);
        maths.setModules({user});
        // Data-less: an unknown name is I-UNRESOLVED (Info), never an Error.
        const maths::Diagnostics dataless = maths.check("'Nope' * 2", std::nullopt);
        QVERIFY(hasCode(dataless, "I-UNRESOLVED"));
        QVERIFY(!maths::hasError(dataless));
        // Against the source: unknown channels are errors; logged channels, maths outputs, constants and
        // details resolve.
        QVERIFY(hasCode(maths.check("'Nope' * 2", src->id), "E-UNKNOWN-CHANNEL"));
        QVERIFY(!maths::hasError(maths.check("'Throttle'[%] * 2", src->id)));
        QVERIFY(!maths::hasError(maths.check("'Twice' + 'Corr Speed'[m/s]", src->id)));
        QVERIFY(!maths::hasError(maths.check("'Throttle'[%] * 'Track Length'[m]", src->id)));
        QVERIFY(!maths::hasError(maths.check("'File Name' == \"ms-d.ld\"", src->id)));
        QVERIFY(hasCode(maths.check("'Wheel Speed FL' * 2", src->id), "W-UNIT-MISSING"));
        QVERIFY(maths::hasError(maths.check("2 * (", src->id)));
        QVERIFY(maths::hasError(maths.check("nosuchfunction('Throttle')", src->id)));
        // The feed: one check 150 ms after the last keystroke, flush() at once.
        LiveDiagnostics feed(maths);
        QSignalSpy ready(&feed, &LiveDiagnostics::diagnosticsReady);
        feed.setSource(src->id);
        feed.setExpression(QStringLiteral("'Thr"));
        feed.setExpression(QStringLiteral("'Throttle"));
        feed.setExpression(QStringLiteral("'Nope' * 2"));
        QVERIFY(feed.pending());
        QCOMPARE(ready.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QVERIFY(feed.hasError());
        QVERIFY(hasCode(feed.diagnostics(), "E-UNKNOWN-CHANNEL"));
        feed.setExpression(QStringLiteral("'Throttle'[%] * 2"));
        feed.flush();
        QCOMPARE(ready.count(), 2);
        QVERIFY(!feed.hasError());
        QVERIFY(!feed.pending());
        feed.setSource(std::nullopt);
        feed.setExpression(QStringLiteral("'Nope'"));
        feed.flush();
        QVERIFY(!feed.hasError()); // data-less: I-UNRESOLVED only
    }

    // connectProjectSession: the project's documents reach the service on every change.
    void projectDocumentsReachTheService() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        const auto dir = scratchDir("ms-session");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        connectProjectSession(session, &maths, nullptr);
        SyntheticOptions o;
        o.path = "ms-e.ld";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        session.undoStack().push(new EditMathModulesCommand(
            session, {module("user", {item("P", "'Throttle'[%] * 2")})}, QStringLiteral("Modules")));
        QCOMPARE(maths.modules().size(), std::size_t{1});
        QVERIFY(maths.isMathsChannel(src->id, "Normal", "P"));
        session.undoStack().undo();
        QVERIFY(maths.modules().empty());
        QVERIFY(!maths.isMathsChannel(src->id, "Normal", "P"));
    }

    // AC-M2-12: every trace kind produced and rendered; missing workbook references shown, never dropped.
    void channelStatusRowsAndTraces() {
        AppRuntime rt(runtimeOptions());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        const auto dir = scratchDir("ms-status");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        session.setUnits(shippedUnits());
        connectProjectSession(session, &maths, nullptr);
        SyntheticOptions o;
        o.path = "ms-status.ld";
        o.throttleLoggedName = "TPS"; // renamed at load by a dictionary or mapping
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        maths::MathModule user = module("user", {item("Twice", "'Throttle'[%] * 2")});
        maths::MathConstant k;
        k.name = "Track Length";
        k.value.v = 2000.0;
        k.value.unit = *shippedUnits()->find("m");
        user.constants.push_back(k);
        session.undoStack().push(new EditMathModulesCommand(session, {user}, QStringLiteral("Modules")));
        project::Aliases aliases;
        aliases.table.rows.push_back(
            model::AliasRow{"Speed Any", {{"wheel speed fl", true}}, -1, std::nullopt});
        aliases.table.rows.push_back(model::AliasRow{"Pedal", {{"TPS", true}}, -1, std::nullopt});
        aliases.table.rows.push_back(model::AliasRow{"Nothing", {{"Nope", true}}, -1, std::nullopt});
        aliases.table.rows.push_back(model::AliasRow{"Dist", {{"Corr Dist", true}}, -1, std::nullopt});
        session.undoStack().push(new EditAliasesCommand(session, aliases, QStringLiteral("Aliases")));
        project::Mappings mappings;
        mappings.rows.push_back(project::Mappings::Row{"TPS", "Throttle", std::nullopt, false});
        mappings.rows.push_back(project::Mappings::Row{"Unused", "Elsewhere", std::nullopt, false});
        session.undoStack().push(new EditMappingsCommand(session, mappings, QStringLiteral("Mappings")));

        ChannelStatusModel model(maths, &session);
        model.refresh();
        QCOMPARE(model.columnCount(), ChannelStatusModel::kColumnCount);
        QCOMPARE(model.headerData(ChannelStatusModel::ResolvedColumn, Qt::Horizontal).toString(),
                 QStringLiteral("Resolved to"));
        // Maths items, built-in outputs, constants.
        const ChannelStatusModel::Row* twice =
            rowNamed(model, QStringLiteral("Expression"), QStringLiteral("Twice"));
        QVERIFY(twice != nullptr);
        QCOMPARE(twice->status, QStringLiteral("Pending"));
        QCOMPARE(twice->module, QStringLiteral("user"));
        const ChannelStatusModel::Row* corrDist =
            rowNamed(model, QStringLiteral("Built-in"), QStringLiteral("Corr Dist"));
        QVERIFY(corrDist != nullptr);
        QVERIFY(corrDist->resolved.contains(QStringLiteral("built-in esses.corrected_speed_distance")));
        QVERIFY(rowNamed(model, QStringLiteral("Constant"), QStringLiteral("Track Length")) != nullptr);
        // Alias rows: case-fold, logged name, missing, and an alias of a maths output.
        const ChannelStatusModel::Row* speedAny =
            rowNamed(model, QStringLiteral("Alias"), QStringLiteral("Speed Any"));
        QVERIFY(speedAny != nullptr);
        QCOMPARE(speedAny->status, QStringLiteral("OK"));
        QVERIFY(speedAny->trace.contains(QStringLiteral("alias")));
        QVERIFY(speedAny->trace.contains(QStringLiteral("case-fold")));
        const ChannelStatusModel::Row* pedal =
            rowNamed(model, QStringLiteral("Alias"), QStringLiteral("Pedal"));
        QVERIFY(pedal != nullptr);
        QVERIFY(pedal->trace.contains(QStringLiteral("logged name")) ||
                pedal->trace.contains(QStringLiteral("mapping")));
        const ChannelStatusModel::Row* nothing =
            rowNamed(model, QStringLiteral("Alias"), QStringLiteral("Nothing"));
        QVERIFY(nothing != nullptr);
        QCOMPARE(nothing->status, QStringLiteral("(missing)"));
        QCOMPARE(nothing->resolved, QStringLiteral("(missing)"));
        QVERIFY(nothing->trace.contains(QStringLiteral("missing")));
        const ChannelStatusModel::Row* dist =
            rowNamed(model, QStringLiteral("Alias"), QStringLiteral("Dist"));
        QVERIFY(dist != nullptr);
        QCOMPARE(dist->status, QStringLiteral("OK"));
        // Mapping rows: the used one per source, the unused one as a Project row.
        const ChannelStatusModel::Row* mapped =
            rowNamed(model, QStringLiteral("Mapping"), QStringLiteral("Throttle"));
        QVERIFY(mapped != nullptr);
        QVERIFY(mapped->resolved.contains(QStringLiteral("mapping")));
        QVERIFY(mapped->trace.contains(QStringLiteral("mapping")));
        QCOMPARE(mapped->unit, QStringLiteral("%"));
        const ChannelStatusModel::Row* unused =
            rowNamed(model, QStringLiteral("Mapping"), QStringLiteral("Elsewhere"));
        QVERIFY(unused != nullptr);
        QCOMPARE(unused->source, QStringLiteral("Project"));
        // The logged-name trace without the mapping: 'TPS' is the logged spelling of 'Throttle'.
        project::Mappings none;
        session.undoStack().push(new EditMappingsCommand(session, none, QStringLiteral("Mappings")));
        model.refresh();
        pedal = rowNamed(model, QStringLiteral("Alias"), QStringLiteral("Pedal"));
        QVERIFY(pedal != nullptr);
        QVERIFY(pedal->trace.contains(QStringLiteral("logged name")));
        QCOMPARE(pedal->status, QStringLiteral("OK"));
        // The shipped workbook references channels this source does not log: "(missing)", never dropped.
        const auto missing = std::count_if(model.rows().begin(), model.rows().end(), [](const auto& r) {
            return r.kind == QStringLiteral("Reference") && r.status == QStringLiteral("(missing)");
        });
        QVERIFY(missing >= 1);
        // Roles: the trace (one step per line), errors, keys, modules; rowOf finds a maths output.
        const int row = model.rowOf(keyOf(*src, "Twice"));
        QVERIFY(row >= 0);
        QCOMPARE(model.data(model.index(row, 0), ChannelStatusModel::ModuleRole).toString(),
                 QStringLiteral("user"));
        QVERIFY(!model.data(model.index(row, 0), ChannelStatusModel::KeyRole).toString().isEmpty());
        QCOMPARE(model.data(model.index(row, ChannelStatusModel::KindColumn)).toString(),
                 QStringLiteral("Expression"));
        // Updates: status changes reach the rows coalesced (10 Hz) without a reset when the rows stay.
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy data(&model, &QAbstractItemModel::dataChanged);
        static_cast<void>(maths.require(keyOf(*src, "Twice"), Priority::Visible)); // may finish at once
        QTRY_COMPARE_WITH_TIMEOUT(
            model.data(model.index(model.rowOf(keyOf(*src, "Twice")), ChannelStatusModel::StatusColumn))
                .toString(),
            QStringLiteral("OK"), 30000);
        QCOMPARE(resets.count(), 0);
        QVERIFY(data.count() >= 1);
        // An error row carries ErrorRole.
        session.undoStack().push(new EditMathModulesCommand(
            session, {module("user", {item("Bad", "'Nope'")})}, QStringLiteral("Modules")));
        QTRY_VERIFY_WITH_TIMEOUT(
            rowNamed(model, QStringLiteral("Expression"), QStringLiteral("Bad")) != nullptr, 5000);
        const int bad = model.rowOf(keyOf(*src, "Bad"));
        QVERIFY(model.data(model.index(bad, 0), ChannelStatusModel::ErrorRole).toBool());
        QVERIFY(model.data(model.index(bad, ChannelStatusModel::StatusColumn))
                    .toString()
                    .startsWith(QStringLiteral("Error: ")));
    }
};

QTEST_GUILESS_MAIN(TstMathsService)
#include "tst_maths_service.moc"
