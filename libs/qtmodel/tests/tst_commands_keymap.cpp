// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandRegistry, KeymapBinder and the facade commands (06 sections 14.1-14.4, D-030, D-033; AC-M1-06 at the
// model level): registry semantics, the shipped data/keymaps/default.json bound without conflicts, context
// resolution along the focus chain, key normalisation, row order, rejected rows, and every qtmodel-registered
// command of hotkey batch 1 driven through its default key with the documented post-condition. The widget-
// level key tests (KEYMAP_TEST rows) live in tests/ui/keymap.

#include "qtmodel_test_support.hpp"

#include <esses/project/keymap.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <functional>
#include <set>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

constexpr Micros kSec = 1'000'000;

const QList<CommandContext> kGraphChain{CommandContext::Graph, CommandContext::Component,
                                        CommandContext::Worksheet, CommandContext::Global};
const QList<CommandContext> kDataWindowChain{CommandContext::DataWindow, CommandContext::Global};

project::Keymap shippedKeymap() {
    std::vector<std::string> log;
    auto k = project::Keymap::load(repoRoot() / "data" / "keymaps" / "default.json", log);
    if (!k) {
        qFatal("cannot load data/keymaps/default.json");
    }
    return std::move(k).value();
}

project::Keymap keymapOf(const char* json) {
    std::vector<std::string> log;
    auto k = project::Keymap::parse(json, log);
    if (!k) {
        qFatal("bad test keymap: %s", k.error().toString().c_str());
    }
    return std::move(k).value();
}

struct Shell {
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
    CommandRegistry registry;
    KeymapBinder binder{registry};
    std::shared_ptr<const model::DataSource> src;
    std::unique_ptr<WorksheetViewModel> sheet;

    Shell() {
        SyntheticOptions o;
        o.path = "keys.ld";
        o.lapSeconds = {40.0, 50.0, 48.0, 48.3, 30.0};
        src = makeSource(o, rt.cache());
        maths.addSource(src);
        distance.prefetch(src->id);
        selection.sourcePublished(src);
        project::WorksheetDef def;
        def.name = "Sheet";
        def.domain = "time";
        sheet = std::make_unique<WorksheetViewModel>(def, workspace, selection, distance);
        registerSelectionCommands(registry, selection);
        registerViewCommands(registry, [this] { return sheet.get(); });
        const QStringList rejected = binder.setKeymap(shippedKeymap());
        if (!rejected.isEmpty()) {
            qFatal("the shipped keymap has rejected rows: %s", qPrintable(rejected.join(QLatin1Char('\n'))));
        }
    }
    model::RangeRef lap(std::size_t i) const { return rangeRef(*src, i); }
    bool key(const char* k, const QList<CommandContext>& chain = kGraphChain) {
        return binder.dispatch(QString::fromLatin1(k), chain);
    }
};

} // namespace

class TstCommandsKeymap : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void registryBasics() {
        CommandRegistry registry;
        QSignalSpy changed(&registry, &CommandRegistry::commandsChanged);
        QSignalSpy invoked(&registry, &CommandRegistry::invoked);
        CommandSpec spec;
        spec.id = QStringLiteral("data.toggle_domain");
        spec.title = QStringLiteral("Time / Distance");
        spec.context = CommandContext::Global;
        spec.defaultKeys = {QStringLiteral("F9")};
        registry.add(spec);
        QCOMPARE(changed.count(), 1);
        QVERIFY(registry.find(spec.id) != nullptr);
        QVERIFY(!registry.isEnabled(spec.id)); // no handler: disabled
        QVERIFY(!registry.invoke(spec.id));
        int hits = 0;
        QVariantList seen;
        bool allowed = true;
        QVERIFY(registry.setHandler(
            spec.id,
            [&](const QVariantList& args) {
                ++hits;
                seen = args;
            },
            [&] { return allowed; }));
        QVERIFY(!registry.setHandler(QStringLiteral("no.such_command"), [](const QVariantList&) {}));
        QVERIFY(registry.invoke(spec.id, {QVariant(3)}));
        QCOMPARE(hits, 1);
        QCOMPARE(seen, QVariantList{QVariant(3)});
        QCOMPARE(invoked.count(), 1);
        allowed = false;
        QVERIFY(!registry.isEnabled(spec.id));
        QVERIFY(!registry.invoke(spec.id));
        QCOMPARE(hits, 1);
        QVERIFY(!registry.invoke(QStringLiteral("no.such_command")));
        QCOMPARE(registry.all().size(), 1);
        const QJsonObject dump = QJsonDocument::fromJson(registry.dumpJson().toUtf8()).object();
        const QJsonObject row = dump.value(QStringLiteral("commands")).toArray().at(0).toObject();
        QCOMPARE(row.value(QStringLiteral("id")).toString(), spec.id);
        QCOMPARE(row.value(QStringLiteral("context")).toString(), QStringLiteral("global"));
        QCOMPARE(row.value(QStringLiteral("keys")).toArray().at(0).toString(), QStringLiteral("F9"));
        for (int c = 0; c <= static_cast<int>(CommandContext::ValuesWindow); ++c) {
            const auto ctx = static_cast<CommandContext>(c);
            QCOMPARE(parseCommandContext(commandContextName(ctx)), std::optional<CommandContext>(ctx));
        }
        QVERIFY(!parseCommandContext(QStringLiteral("toolbar")));
    }

    void shippedKeymapResolvesAlongTheFocusChain() {
        CommandRegistry registry;
        KeymapBinder binder(registry);
        QSignalSpy changed(&binder, &KeymapBinder::bindingsChanged);
        QVERIFY(
            binder.setKeymap(shippedKeymap()).isEmpty()); // no same-context conflicts in the shipped table
        QCOMPARE(changed.count(), 1);
        QVERIFY(binder.bindingCount() > 100);
        QCOMPARE(binder.resolve(QStringLiteral("F9"), kGraphChain)->command,
                 QStringLiteral("data.toggle_domain"));
        QCOMPARE(binder.resolve(QStringLiteral("F9"), kDataWindowChain)->command,
                 QStringLiteral("data.toggle_domain"));
        QCOMPARE(binder.resolve(QStringLiteral("F9"), kGraphChain)->context, CommandContext::Global);
        QCOMPARE(binder.resolve(QStringLiteral("Up"), kGraphChain)->command, QStringLiteral("zoom.in"));
        QCOMPARE(binder.resolve(QStringLiteral("Up"), kGraphChain)->context, CommandContext::Graph);
        QVERIFY(!binder.resolve(QStringLiteral("Up"), kDataWindowChain)); // graph keys stay in the graph
        QCOMPARE(binder.resolve(QStringLiteral("Return"), kDataWindowChain)->command,
                 QStringLiteral("data.select_main"));
        QCOMPARE(binder.resolve(QStringLiteral("ctrl+f"), kDataWindowChain)->command,
                 QStringLiteral("data.select_fastest")); // normalised
        QCOMPARE(binder.resolve(QStringLiteral("alt+ctrl+f"), kGraphChain)->command,
                 QStringLiteral("data.select_all_fast"));
        const auto three =
            binder.resolve(QStringLiteral("3"), {CommandContext::Worksheet, CommandContext::Global});
        QCOMPARE(three->command, QStringLiteral("view.worksheet_n"));
        QCOMPARE(three->args, QVariantList{QVariant(3)});
        QCOMPARE(binder.keysFor(QStringLiteral("data.select_main")),
                 (QStringList{QStringLiteral("Return"), QStringLiteral("Enter")})); // row order
        QCOMPARE(KeymapBinder::normalise(QStringLiteral("shift+ctrl+c")), QStringLiteral("Ctrl+Shift+C"));
    }

    void conflictsAndUnknownContextsAreRejected() {
        CommandRegistry registry;
        KeymapBinder binder(registry);
        const QStringList rejected = binder.setKeymap(keymapOf(R"({
            "$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "test", "inherits": null,
            "bindings": [
              {"command": "zoom.in", "keys": ["Up"], "context": "graph"},
              {"command": "zoom.out", "keys": ["up"], "context": "graph"},
              {"command": "cursor.left", "keys": ["Up"], "context": "summary"}
            ], "unbind": []})"));
        QCOMPARE(rejected.size(), 1);
        QVERIFY(rejected.first().contains(QStringLiteral("zoom.out")));
        QCOMPARE(binder.resolve(QStringLiteral("Up"), kGraphChain)->command,
                 QStringLiteral("zoom.in")); // first wins
        QCOMPARE(binder.resolve(QStringLiteral("Up"), {CommandContext::Summary})->command,
                 QStringLiteral("cursor.left"));
        QVERIFY(binder.keysFor(QStringLiteral("zoom.out")).isEmpty());
        // A disabled or unregistered command does not consume the key.
        QVERIFY(!binder.dispatch(QStringLiteral("Up"), kGraphChain));
    }

    // The facade commands carry the ids, contexts and default keys of their keymap rows.
    void facadeCommandsMatchTheKeymap() {
        Shell s;
        const project::Keymap keymap = shippedKeymap();
        int checked = 0;
        for (const CommandSpec* spec : s.registry.all()) {
            const auto rows = keymap.rowsFor(spec->id.toStdString());
            QVERIFY2(!rows.empty(), qPrintable(spec->id));
            bool found = false;
            for (const project::KeyBinding* row : rows) {
                if (QString::fromStdString(row->context) != commandContextName(spec->context)) {
                    continue;
                }
                QStringList keys;
                for (const std::string& k : row->keys) {
                    keys << KeymapBinder::normalise(QString::fromStdString(k));
                }
                QStringList ours;
                for (const QString& k : spec->defaultKeys) {
                    ours << KeymapBinder::normalise(k);
                }
                QCOMPARE(ours, keys);
                found = true;
            }
            QVERIFY2(found, qPrintable(spec->id));
            ++checked;
        }
        QVERIFY(checked >= 29);
    }

    // One row per qtmodel-bound implemented key of batch 1: send the key, assert the documented
    // post-condition.
    void batchOneKeysDriveTheModels() {
        Shell s;
        DataSelectionModel& sel = s.selection;
        WorksheetViewModel& ws = *s.sheet;
        // Selection keys.
        QVERIFY(s.key("Ctrl+F", kDataWindowChain));
        QCOMPARE(sel.snapshot().main, s.lap(2));
        QVERIFY(s.key("Ctrl+Alt+F"));
        QCOMPARE(sel.snapshot().overlays, std::vector<model::RangeRef>{s.lap(3)});
        QVERIFY(s.key("F4"));
        QVERIFY(sel.snapshot().overlaysHidden);
        QVERIFY(s.key("Shift+F4"));
        QVERIFY(sel.snapshot().overlays.empty() && !sel.snapshot().overlaysHidden);
        QVERIFY(s.key("N", {CommandContext::Worksheet, CommandContext::Global}));
        QCOMPARE(sel.snapshot().main, s.lap(3));
        QVERIFY(s.key("P"));
        QCOMPARE(sel.snapshot().main, s.lap(2));
        sel.toggleOverlay0(s.lap(1));
        QVERIFY(s.key("Q"));
        QCOMPARE(sel.snapshot().main, s.lap(1));
        QCOMPARE(*sel.snapshot().overlay0, s.lap(2));
        QVERIFY(s.key("J"));
        QVERIFY(sel.snapshot().singleColourMain);
        QVERIFY(s.registry.find(QStringLiteral("data.single_colour_main"))->checked());
        // View keys on the active worksheet (Main = Lap 1, 50 s, starting at 40 s).
        QVERIFY(s.key("W"));
        QCOMPARE(ws.state().zoom, (plot::XSpan{0.0, 50.0 * kSec}));
        QVERIFY(s.key("Up"));
        QCOMPARE(ws.state().zoom.length(), 25.0 * kSec);
        QVERIFY(s.key("Down"));
        QCOMPARE(ws.state().zoom.length(), 50.0 * kSec);
        QVERIFY(s.key("F2", kDataWindowChain)); // works from any dock
        QCOMPARE(ws.state().zoom, (plot::XSpan{-40'000'000.0, 176'300'000.0}));
        QVERIFY(s.key("W"));
        QVERIFY(s.key("Shift+Right"));
        QCOMPARE(ws.state().zoom.a, 5.0 * kSec);
        QVERIFY(s.key("Shift+Left"));
        QCOMPARE(ws.state().zoom.a, 0.0);
        QVERIFY(s.key("F"));
        QCOMPARE(ws.state().zoom.a, 45.0 * kSec);
        QVERIFY(s.key("B"));
        QCOMPARE(ws.state().zoom.a, 0.0);
        ws.setSize(1000);
        ws.setCursor(10 * kSec, CursorSource::Click);
        QVERIFY(s.key("Right"));
        const Micros right = ws.state().cursorT;
        QVERIFY(right > 10 * kSec);
        QVERIFY(s.key("Left"));
        QCOMPARE(ws.state().cursorT, 10 * kSec);
        QVERIFY(s.key("Ctrl+Right"));
        QCOMPARE(ws.state().cursorT - 10 * kSec, 10 * (right - 10 * kSec));
        QVERIFY(s.key("Ctrl+Left"));
        QCOMPARE(ws.state().cursorT, 10 * kSec);
        QVERIFY(s.key("Space"));
        QCOMPARE(*ws.state().datumT, 10 * kSec);
        QVERIFY(s.key("D"));
        QVERIFY(!ws.state().datumT);
        QVERIFY(s.key("D"));
        QVERIFY(s.key("End"));
        QVERIFY(ws.state().cursorT > 40 * kSec);
        QVERIFY(s.key("X"));
        QCOMPARE(ws.state().cursorT, 10 * kSec);
        QVERIFY(s.key("Z"));
        QCOMPARE(ws.state().zoom.a, 10.0 * kSec);
        QVERIFY(s.key("Home"));
        QCOMPARE(ws.state().cursorT, 10 * kSec);
        ws.setCursor(30 * kSec, CursorSource::Click);
        QVERIFY(s.key("H"));
        QCOMPARE((ws.state().zoom.a + ws.state().zoom.b) / 2.0, 30.0 * kSec);
        QVERIFY(s.key("F9", kDataWindowChain));
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(s.registry.find(QStringLiteral("data.toggle_domain"))->checked());
        QVERIFY(s.key("F9"));
        QCOMPARE(ws.domain(), plot::Domain::Time);
        // Without an active worksheet the view commands are disabled and keys fall through.
        s.sheet.reset();
        QVERIFY(!s.registry.isEnabled(QStringLiteral("zoom.in")));
        QVERIFY(!s.key("Up"));
    }
};

QTEST_GUILESS_MAIN(TstCommandsKeymap)
#include "tst_commands_keymap.moc"
