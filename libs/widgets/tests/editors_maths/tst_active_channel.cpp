// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The active-channel hotkeys (06 section 14.4, 03 section 11 item 10; ActiveChannelCommands) on a real
// time_distance graph over sources opened through the loader: the keys of data/keymaps/default.json resolve
// to the commands in component context; next/previous active channel wraps and skips hidden traces; hide and
// remove are spec edits; CTRL+SHIFT+M opens the Maths Editor at the defining item and gives a hint on a
// logged or built-in channel; CTRL+SHIFT+S selects the row in Channel Status (maths item or alias) or gives a
// hint; CTRL+SHIFT+F creates transient maths, never a document edit, gone when the source closes ("transient
// filter disappears on reload"); CTRL+SHIFT+O writes local maths with the DPS preserved; CTRL+SHIFT+Z ("zero
// at cursor writes sidecar with DPS preserved", "zero at cursor on a deg C channel reads 0 deg C at the
// cursor").

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "maths_editors_fixture.hpp"

#include <esses/base/uuid.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/editors_maths/active_channel_commands.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableView>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
namespace mt = esses::widgets::mathstest;

namespace {

const QList<qt::CommandContext> kGraphChain{qt::CommandContext::Graph, qt::CommandContext::Component,
                                            qt::CommandContext::Worksheet, qt::CommandContext::Global};

// A time_distance spec with one group per entry (a trace's displayUnit when given as "name|unit").
project::ComponentSpec graphSpec(const std::vector<std::vector<std::string>>& groups,
                                 const std::string& active) {
    QJsonArray gj;
    for (const std::vector<std::string>& g : groups) {
        QJsonArray traces;
        for (const std::string& entry : g) {
            const auto bar = entry.find('|');
            QJsonObject t{{QStringLiteral("channel"), QString::fromStdString(entry.substr(0, bar))}};
            if (bar != std::string::npos) {
                t.insert(QStringLiteral("displayUnit"), QString::fromStdString(entry.substr(bar + 1)));
            }
            traces.append(t);
        }
        gj.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(g.front())},
                              {QStringLiteral("scaleMode"), QStringLiteral("channel")},
                              {QStringLiteral("traces"), traces}});
    }
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("groups"), gj}}).toJson(QJsonDocument::Compact);
    project::ComponentSpec spec;
    spec.id = uuidRandom();
    spec.type = "time_distance";
    spec.rect = project::Rect{0.0, 0.0, 1.0, 1.0};
    spec.activeChannel = active;
    spec.body = JsonDoc::parse(json.toStdString()).value();
    return spec;
}

// One worksheet with one graph and the windows the commands open; spec edits are applied as the canvas would.
struct Desk {
    explicit Desk(mt::Workspace& ws, const project::ComponentSpec& spec)
        : w(ws), render(ws.rt.pool(), ws.rt.dispatcher(), *ws.selection, ws.maths, ws.distance),
          status(ws.maths, &ws.session) {
        sheet.id = uuidRandom();
        sheet.name = "Sheet";
        sheet.domain = "time";
        view = std::make_unique<qt::WorksheetViewModel>(sheet, workspace, *ws.selection, ws.distance);
        view->setSize(800);
        context = std::unique_ptr<WorksheetContext>(
            new WorksheetContext{*view, *ws.selection, ws.maths, render, commands, ws.session.scheme(),
                                 QString::fromStdString(sheet.id.str())});
        registerBuiltinComponents();
        graph.reset(ComponentWidget::create(spec, *context, nullptr));
        graph->resize(800, 400);
        QObject::connect(graph.get(), &ComponentWidget::specEditRequested, graph.get(),
                         [this](const project::ComponentSpec& next, const QString&) {
                             ++edits;
                             graph->applySpec(next);
                         });
        ActiveChannelCommands::Host host;
        host.focusedComponent = [this] { return graph.get(); };
        host.activeWorksheet = [this] { return view.get(); };
        host.mathsEditor = [this] {
            if (!editor) {
                editor = std::make_unique<MathsEditor>(w.session, w.maths, &w.loader);
            }
            editor->show();
            return editor.get();
        };
        host.channelStatus = [this] {
            if (!window) {
                window = std::make_unique<ChannelStatusWindow>(status, commands);
            }
            window->show();
            return window.get();
        };
        channels = std::make_unique<ActiveChannelCommands>(ws.session, ws.loader, ws.maths, *ws.selection,
                                                           std::move(host));
        channels->registerCommands(commands);
    }

    std::optional<plot::TimeDistanceBody> body() const {
        Result<plot::TimeDistanceBody> b = plot::TimeDistanceBody::parse(graph->spec().body);
        return b ? std::optional<plot::TimeDistanceBody>(std::move(b).value()) : std::nullopt;
    }
    void setActive(const std::string& channel) {
        project::ComponentSpec s = graph->spec();
        s.activeChannel = channel;
        graph->applySpec(s);
    }

    mt::Workspace& w;
    qt::WorkspaceViewModel workspace;
    qt::RenderService render;
    qt::CommandRegistry commands;
    qt::ChannelStatusModel status;
    project::WorksheetDef sheet;
    std::unique_ptr<qt::WorksheetViewModel> view;
    std::unique_ptr<WorksheetContext> context;
    std::unique_ptr<ComponentWidget> graph;
    std::unique_ptr<MathsEditor> editor;
    std::unique_ptr<ChannelStatusWindow> window;
    std::unique_ptr<ActiveChannelCommands> channels;
    int edits = 0;
};

std::optional<maths::MathModule> localMaths(mt::Workspace& ws, const model::SourceId& id) {
    const auto src = ws.loader.source(id);
    const std::optional<JsonDoc>& doc = src->sidecar.layer(model::kNormalLayer).localMaths;
    if (!doc || doc->isNull()) {
        return std::nullopt;
    }
    std::vector<std::string> log;
    Result<maths::MathModule> m = maths::MathModuleIo::fromJson(*doc, *qt::test::shippedUnits(), log);
    return m ? std::optional<maths::MathModule>(std::move(m).value()) : std::nullopt;
}

bool hasTransientRow(const qt::MathsService& maths, const model::SourceId& id, const std::string& name) {
    for (const qt::MathsService::StatusRow& r : maths.statusRows()) {
        if (r.source == id && r.moduleId == maths::MathModule::kTransientId && r.name == name) {
            return true;
        }
    }
    return false;
}

// The displayed Main value of `channel` at the worksheet's cursor ("..." while it waits).
QString valueAt(mt::Workspace& ws, qt::WorksheetViewModel& view, const qt::ChannelRequest& request);

// The display shows zero at the decimals of the channel ("0.0" or "-0.0").
bool readsZero(mt::Workspace& ws, qt::WorksheetViewModel& view, const qt::ChannelRequest& request) {
    bool ok = false;
    const double v = valueAt(ws, view, request).toDouble(&ok);
    return ok && std::abs(v) < 0.051;
}

QString valueAt(mt::Workspace& ws, qt::WorksheetViewModel& view, const qt::ChannelRequest& request) {
    qt::ValuesModel values(*ws.selection, ws.maths);
    values.setChannelRequests({request});
    values.setWorksheet(&view);
    values.refresh(plot::kDynamicOnly);
    return values.cellText(0, qt::ValuesModel::MainCol);
}

} // namespace

class TstActiveChannel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { mt::registerMetaTypes(); }

    // The rows of data/keymaps/default.json bind to the registered commands in the graph's context chain.
    void keysResolveToTheCommands() {
        mt::Workspace ws("ac-keys");
        ws.openSynthetic("ac-keys");
        Desk d(ws, graphSpec({{"Wheel Speed FL", "Wheel Speed FR"}, {"Throttle"}}, "Wheel Speed FL"));
        qt::KeymapBinder binder(d.commands);
        static_cast<void>(binder.setKeymap(ws.session.keymap()));
        const std::vector<std::pair<const char*, const char*>> rows{
            {"channel.next_active", "PgDown"},
            {"channel.prev_active", "PgUp"},
            {"channel.hide", "Ctrl+H"},
            {"channel.remove", "Ctrl+Delete"},
            {"channel.edit_maths", "Ctrl+Shift+M"},
            {"channel.status", "Ctrl+Shift+S"},
            {"channel.filter", "Ctrl+Shift+F"},
            {"channel.scale_offset", "Ctrl+Shift+O"},
            {"channel.zero_at_cursor", "Ctrl+Shift+Z"}};
        QCOMPARE(ActiveChannelCommands::commandIds().size(), static_cast<qsizetype>(rows.size()));
        for (const auto& [id, key] : rows) {
            const qt::CommandSpec* spec = d.commands.find(QString::fromLatin1(id));
            QVERIFY2(spec != nullptr, id);
            QCOMPARE(spec->context, qt::CommandContext::Component);
            QCOMPARE(spec->defaultKeys, QStringList{QString::fromLatin1(key)});
            const std::optional<qt::KeyResolution> r = binder.resolve(QString::fromLatin1(key), kGraphChain);
            QVERIFY2(r.has_value(), key);
            QCOMPARE(r->command, QString::fromLatin1(id));
            QVERIFY2(d.commands.isEnabled(QString::fromLatin1(id)), id);
        }
        // PgDown through the binder moves the square.
        QVERIFY(binder.dispatch(QStringLiteral("PgDown"), kGraphChain));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FR"));
        // Nothing focused: every command is disabled.
        d.graph.reset();
        for (const QString& id : ActiveChannelCommands::commandIds()) {
            QVERIFY(!d.commands.isEnabled(id));
            QVERIFY(!d.commands.invoke(id));
        }
    }

    void navigationHideAndRemove() {
        mt::Workspace ws("ac-nav");
        ws.openSynthetic("ac-nav");
        Desk d(ws, graphSpec({{"Wheel Speed FL", "Wheel Speed FR"}, {"Throttle"}}, "Wheel Speed FL"));
        QVERIFY(d.commands.invoke(QStringLiteral("channel.next_active")));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FR"));
        QVERIFY(d.commands.invoke(QStringLiteral("channel.next_active")));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Throttle"));
        QVERIFY(d.commands.invoke(QStringLiteral("channel.next_active"))); // wraps across groups
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FL"));
        QVERIFY(d.commands.invoke(QStringLiteral("channel.prev_active")));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Throttle"));
        QCOMPARE(d.edits, 4);
        // Hide: the trace becomes invisible and the square moves to a visible trace.
        QVERIFY(d.channels->hideActive());
        std::optional<plot::TimeDistanceBody> body = d.body();
        QVERIFY(body.has_value());
        QVERIFY(!body->groups[1].traces[0].visible);
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FL"));
        // Navigation skips hidden traces.
        QVERIFY(d.channels->nextActive(1));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FR"));
        QVERIFY(d.channels->nextActive(1));
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FL"));
        // Remove: the trace leaves; the neighbouring visible trace becomes active; an emptied group goes.
        d.setActive("Wheel Speed FR");
        QVERIFY(d.channels->removeActive());
        body = d.body();
        QCOMPARE(body->groups.size(), std::size_t{2});
        QCOMPARE(body->groups[0].traces.size(), std::size_t{1});
        QCOMPARE(d.graph->spec().activeChannel, std::string("Wheel Speed FL"));
        QVERIFY(d.channels->removeActive());
        body = d.body();
        QCOMPARE(body->groups.size(), std::size_t{1});
        QCOMPARE(body->groups[0].traces[0].channel, std::string("Throttle"));
        QCOMPARE(d.graph->spec().activeChannel, std::string());
    }

    // CTRL+SHIFT+M and CTRL+SHIFT+S.
    void editMathsAndStatus() {
        mt::Workspace ws("ac-maths");
        ws.setModules({mt::module("user", {mt::item("Twice", "'Throttle'[%] * 2")})});
        project::Aliases aliases;
        aliases.table.rows.push_back(
            model::AliasRow{"Speed Any", {{"Wheel Speed FL", true}}, -1, std::nullopt});
        ws.session.undoStack().push(
            new qt::EditAliasesCommand(ws.session, aliases, QStringLiteral("Aliases")));
        ws.openSynthetic("ac-maths");
        Desk d(ws, graphSpec({{"Twice", "Throttle", "Corr Speed", "Speed Any"}}, "Twice"));
        QSignalSpy hints(d.channels.get(), &ActiveChannelCommands::hint);
        QVERIFY(d.commands.invoke(QStringLiteral("channel.edit_maths")));
        QVERIFY(d.editor && d.editor->isVisible());
        QCOMPARE(d.editor->currentModule()->moduleId, QStringLiteral("user"));
        QCOMPARE(d.editor->currentItemName(), QStringLiteral("Twice"));
        QCOMPARE(hints.count(), 0);
        // A logged channel only gets a hint; nothing is created.
        d.setActive("Throttle");
        const int stack = ws.session.undoStack().count();
        QVERIFY(!d.channels->editMaths());
        QCOMPARE(hints.count(), 1);
        QVERIFY(d.channels->lastHint().contains(QStringLiteral("logged")));
        QCOMPARE(ws.session.undoStack().count(), stack);
        d.setActive("Corr Speed");
        QVERIFY(!d.channels->editMaths());
        QVERIFY(d.channels->lastHint().contains(QStringLiteral("built-in")));
        // Channel Status: the maths item's row, an alias's row, a hint for a logged channel.
        d.setActive("Twice");
        QVERIFY(d.channels->showStatus());
        QVERIFY(d.window && d.window->isVisible());
        QCOMPARE(d.status.rows()[static_cast<std::size_t>(d.window->currentSourceRow())].channel,
                 QStringLiteral("Twice"));
        d.setActive("Speed Any");
        QVERIFY(d.channels->showStatus());
        const auto& row = d.status.rows()[static_cast<std::size_t>(d.window->currentSourceRow())];
        QCOMPARE(row.channel, QStringLiteral("Speed Any"));
        QCOMPARE(row.kind, QStringLiteral("Alias"));
        d.setActive("Throttle");
        QVERIFY(!d.channels->showStatus());
        QVERIFY(d.channels->lastHint().contains(QStringLiteral("logged")));
    }

    // CTRL+SHIFT+F: transient maths, gone when the source closes ("transient filter disappears on reload").
    void filterIsTransient() {
        mt::Workspace ws("ac-filter");
        const auto src = ws.openSynthetic("ac-filter");
        Desk d(ws, graphSpec({{"Throttle"}}, "Throttle"));
        ActiveChannelCommands::FilterChoice choice;
        choice.points = 5;
        d.channels->setFilterPrompt([&choice](const QString& channel) {
            return channel == QStringLiteral("Throttle")
                       ? std::optional<ActiveChannelCommands::FilterChoice>(choice)
                       : std::nullopt;
        });
        const int stack = ws.session.undoStack().count();
        QVERIFY(d.commands.invoke(QStringLiteral("channel.filter")));
        std::optional<maths::MathModule> t = d.channels->transientModule(src->id);
        QVERIFY(t.has_value());
        QCOMPARE(t->items.size(), std::size_t{1});
        QCOMPARE(t->items[0].kind, maths::ItemKind::Filter);
        QCOMPARE(*t->items[0].window.points, 5);
        QVERIFY(hasTransientRow(ws.maths, src->id, "Throttle"));
        QCOMPARE(ws.session.undoStack().count(), stack); // never a document edit
        QVERIFY(!localMaths(ws, src->id).has_value());   // never in the sidecar
        // Again with seconds: the same item changes.
        choice.points.reset();
        choice.seconds = 0.5;
        QVERIFY(d.channels->filterActive());
        t = d.channels->transientModule(src->id);
        QCOMPARE(t->items.size(), std::size_t{1});
        QCOMPARE(*t->items[0].window.seconds, 0.5);
        QVERIFY(!t->items[0].window.points.has_value());
        // A cancelled prompt does nothing.
        d.channels->setFilterPrompt([](const QString&) { return std::nullopt; });
        QVERIFY(!d.channels->filterActive());
        // Close and reopen: the filter is gone.
        QSignalSpy closed(&ws.loader, &qt::SourceLoader::sourceClosed);
        ws.loader.close(src->id);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QVERIFY(!d.channels->transientModule(src->id).has_value());
        QSignalSpy published(&ws.loader, &qt::SourceLoader::sourcePublished);
        ws.loader.open(ws.dir / "ac-filter.ld");
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QVERIFY(!hasTransientRow(ws.maths, src->id, "Throttle"));
    }

    // CTRL+SHIFT+O: a Scale & Offset item in the sidecar's local maths, DPS preserved, one undo step.
    void scaleOffsetWritesLocalMaths() {
        mt::Workspace ws("ac-so");
        const auto src = ws.openSynthetic("ac-so");
        project::ChannelOverrides::Entry e;
        e.dps = 3;
        ws.session.undoStack().push(new qt::SetOverrideCommand(ws.session, "Throttle", e));
        Desk d(ws, graphSpec({{"Throttle"}}, "Throttle"));
        QString askedUnit;
        d.channels->setScaleOffsetPrompt([&askedUnit](const QString&, const QString& unit) {
            askedUnit = unit;
            return std::optional<ActiveChannelCommands::ScaleOffsetChoice>({2.0, 1.0, QStringLiteral("%")});
        });
        const int stack = ws.session.undoStack().count();
        QVERIFY(d.commands.invoke(QStringLiteral("channel.scale_offset")));
        QCOMPARE(askedUnit, QStringLiteral("ratio")); // the stored unit (no trace display unit)
        QCOMPARE(ws.session.undoStack().count(), stack + 1);
        const std::optional<maths::MathModule> local = localMaths(ws, src->id);
        QVERIFY(local.has_value());
        QCOMPARE(local->items.size(), std::size_t{1});
        const maths::MathItem& item = local->items[0];
        QCOMPARE(item.kind, maths::ItemKind::ScaleOffset);
        QCOMPARE(item.name, std::string("Throttle"));
        QCOMPARE(item.scale, 2.0);
        QCOMPARE(item.offset.v, 1.0);
        QCOMPARE(item.offset.unit, *qt::test::shippedUnits()->find("%"));
        QCOMPARE(item.display.dps, 3); // the channel's decimals (its override), preserved
        QVERIFY(ws.maths.isMathsChannel(src->id, "Normal", "Throttle"));
        ws.session.undoStack().undo();
        QVERIFY(!localMaths(ws, src->id).has_value());
        // An unknown offset unit is refused with a hint.
        d.channels->setScaleOffsetPrompt([](const QString&, const QString&) {
            return std::optional<ActiveChannelCommands::ScaleOffsetChoice>(
                {1.0, 1.0, QStringLiteral("nosuchunit")});
        });
        QVERIFY(!d.channels->scaleOffsetActive());
        QVERIFY(d.channels->lastHint().contains(QStringLiteral("nosuchunit")));
    }

    // CTRL+SHIFT+Z on a temperature stored in K and shown in deg C: 0 deg C at the cursor, DPS preserved.
    void zeroAtCursorReadsZeroInTheDisplayUnit() {
        mt::Workspace ws("ac-zero");
        qt::test::SyntheticOptions o;
        o.path = "ac-zero.ld";
        const auto base = qt::test::makeSource(o, ws.rt.cache());
        std::vector<float> temp(mt::sampleCount(*base));
        for (std::size_t i = 0; i < temp.size(); ++i) {
            temp[i] = 353.15F + 0.01F * static_cast<float>(i % 1000); // 80 deg C and up
        }
        const auto withTemp = mt::withChannel(*base, "Oil Temp", "K", temp, ws.rt.cache(), 1);
        const auto src = ws.open(*withTemp, "ac-zero");
        QVERIFY(ws.selection->hasMain());
        Desk d(ws, graphSpec({{"Oil Temp|deg C"}}, "Oil Temp"));
        d.view->setCursor(10'000'000, qt::CursorSource::Programmatic);
        qt::ChannelRequest request;
        request.name = "Oil Temp";
        request.displayUnit = "deg C";
        QTRY_VERIFY_WITH_TIMEOUT(valueAt(ws, *d.view, request) != QStringLiteral("..."), 30000);
        const QString before = valueAt(ws, *d.view, request);
        bool ok = false;
        const double shown = before.toDouble(&ok);
        QVERIFY2(ok, qPrintable(before));
        QVERIFY(shown > 79.0 && shown < 100.0); // displayed in deg C, not K
        const int stack = ws.session.undoStack().count();
        QVERIFY(d.commands.invoke(QStringLiteral("channel.zero_at_cursor")));
        QCOMPARE(ws.session.undoStack().count(), stack + 1);
        std::optional<maths::MathModule> local = localMaths(ws, src->id);
        QVERIFY(local.has_value());
        const maths::MathItem& item = local->items.front();
        QCOMPARE(item.kind, maths::ItemKind::ScaleOffset);
        QCOMPARE(item.offset.unit, *qt::test::shippedUnits()->find("deg C"));
        QCOMPARE(item.offset.v, -shown);
        QCOMPARE(item.display.dps, 1); // the channel's own decimals, preserved
        // The channel now reads 0 deg C at the cursor (a delta offset: not -273.15).
        QTRY_VERIFY_WITH_TIMEOUT(readsZero(ws, *d.view, request), 30000);
        // A second zero at another cursor adjusts the same item.
        d.view->setCursor(12'000'000, qt::CursorSource::Programmatic);
        QTRY_VERIFY_WITH_TIMEOUT(valueAt(ws, *d.view, request) != QStringLiteral("..."), 30000);
        QVERIFY(!readsZero(ws, *d.view, request));
        QVERIFY(d.channels->zeroAtCursor());
        local = localMaths(ws, src->id);
        QCOMPARE(local->items.size(), std::size_t{1});
        QTRY_VERIFY_WITH_TIMEOUT(readsZero(ws, *d.view, request), 30000);
    }
};

QTEST_MAIN(TstActiveChannel)
#include "tst_active_channel.moc"
