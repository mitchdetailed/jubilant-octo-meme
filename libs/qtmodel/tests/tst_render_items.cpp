// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RenderService (04 sections 1 item 2 and 6.3, 01 section 5.4 rule 2; AC-M1-05) and the dock item models
// (06 sections 6-8; AC-M1-01): replace-key build delivery on the GUI thread, cancelPending, prepareInputs
// for the shipped workbook's components (logged and maths channels, display units, slots, chains) feeding
// the time_distance builder; the Data window tree (slot columns, lap time, delta to fastest, trust,
// outings), the Channels tree, and the Values rows for Main and the reference overlay in both domains.

#include "qtmodel_test_support.hpp"

#include <esses/plotcore/text_metrics.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <atomic>
#include <chrono>
#include <thread>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

constexpr Micros kSec = 1'000'000;

project::WorkbookFile shippedWorkbook() {
    std::vector<std::string> log;
    auto wb =
        project::WorkbookFile::load(repoRoot() / "data" / "workbooks" / "circuit-default.workbook.json", log);
    if (!wb) {
        qFatal("cannot load the shipped workbook");
    }
    return std::move(wb).value();
}

project::ComponentSpec componentOfType(const project::WorkbookFile& wb, const char* type) {
    for (const project::ComponentSpec& c : wb.worksheets.front().components) {
        if (c.type == type) {
            return c;
        }
    }
    return {}; // the test then fails on the missing channels
}

struct World {
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
    RenderService render{rt.pool(), rt.dispatcher(), selection, maths, distance};
    std::shared_ptr<const model::DataSource> a;
    std::shared_ptr<const model::DataSource> b;

    World() {
        SyntheticOptions oa;
        oa.path = "items-a.ld";
        oa.lapSeconds = {40.0, 50.0, 48.0, 48.3, 30.0};
        a = makeSource(oa, rt.cache());
        SyntheticOptions ob;
        ob.path = "items-b.ld";
        ob.lapSeconds = {30.0, 45.0, 44.0, 20.0};
        b = makeSource(ob, rt.cache());
        for (const auto& s : {a, b}) {
            maths.addSource(s);
            distance.prefetch(s->id);
            selection.sourcePublished(s);
        }
        selection.setSidecarCommit(
            [this](std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc) {
                auto next = current->withSidecar(std::move(doc), nullptr,
                                                 analysis::RuleRunner::circuitDefault().rebuildFunction());
                maths.replaceSource(next);
                selection.sourceReplaced(next);
            });
    }
    bool ready(const std::shared_ptr<const model::DataSource>& s) {
        const auto c = distance.chain(rangeRef(*s, 1), Priority::Interactive);
        return c != nullptr && c->complete();
    }
};

plot::ComponentBuild buildTagged(std::uint64_t tag) {
    plot::ComponentBuild b;
    b.viewVersion = tag;
    return b;
}

} // namespace

class TstRenderItems : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void submitBuildDeliversOnTheGuiThread() {
        World w;
        QObject context;
        std::vector<std::uint64_t> delivered;
        QThread* gui = QThread::currentThread();
        bool onGui = true;
        w.render.submitBuild(
            QStringLiteral("ws"), QStringLiteral("c1"), Priority::Visible,
            [](const CancelToken&) -> Result<plot::ComponentBuild> { return buildTagged(1); }, &context,
            [&](Result<plot::ComponentBuild> r) {
                onGui = onGui && QThread::currentThread() == gui;
                delivered.push_back(r ? r->viewVersion : 0);
            });
        QTRY_COMPARE(delivered.size(), std::size_t{1});
        QVERIFY(onGui);
        QCOMPARE(delivered.front(), std::uint64_t{1});
        // A destroyed context receives nothing.
        auto* doomed = new QObject;
        std::atomic<bool> built{false};
        w.render.submitBuild(
            QStringLiteral("ws"), QStringLiteral("c2"), Priority::Visible,
            [&built](const CancelToken&) -> Result<plot::ComponentBuild> {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                built.store(true);
                return buildTagged(2);
            },
            doomed, [&](Result<plot::ComponentBuild>) { delivered.push_back(2); });
        delete doomed;
        QTRY_VERIFY(built.load());
        QTest::qWait(50);
        QCOMPARE(delivered.size(), std::size_t{1});
    }

    // 01 section 5.4 rule 2: one running and one pending build per key; a new submit replaces the pending one
    // (dropped) and never cancels the running one, whose result is still delivered (an interim frame).
    void replaceKeySemantics() {
        World w;
        QObject context;
        std::vector<std::uint64_t> delivered;
        std::atomic<bool> release{false};
        const auto slow = [&release](std::uint64_t tag) {
            return [&release, tag](const CancelToken&) -> Result<plot::ComponentBuild> {
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return buildTagged(tag);
            };
        };
        const auto sink = [&delivered](Result<plot::ComponentBuild> r) {
            delivered.push_back(r ? r->viewVersion : 0);
        };
        w.render.submitBuild(QStringLiteral("ws"), QStringLiteral("graph"), Priority::Visible, slow(1),
                             &context, sink);
        QTRY_COMPARE(w.render.runningBuilds(), 1);
        for (std::uint64_t tag = 2; tag <= 5; ++tag) {
            w.render.submitBuild(QStringLiteral("ws"), QStringLiteral("graph"), Priority::Visible, slow(tag),
                                 &context, sink);
        }
        release.store(true);
        QTRY_COMPARE(delivered.size(), std::size_t{2});
        QCOMPARE(delivered, (std::vector<std::uint64_t>{1, 5})); // 2..4 were replaced while pending
        QTRY_COMPARE(w.render.runningBuilds(), 0);
        // cancelPending drops the queued build; the running one completes.
        release.store(false);
        delivered.clear();
        w.render.submitBuild(QStringLiteral("ws"), QStringLiteral("graph"), Priority::Visible, slow(6),
                             &context, sink);
        QTRY_COMPARE(w.render.runningBuilds(), 1);
        w.render.submitBuild(QStringLiteral("ws"), QStringLiteral("graph"), Priority::Visible, slow(7),
                             &context, sink);
        w.render.cancelPending(QStringLiteral("ws"), QStringLiteral("graph"));
        release.store(true);
        QTRY_COMPARE(delivered.size(), std::size_t{1});
        QTest::qWait(50);
        QCOMPARE(delivered, std::vector<std::uint64_t>{6});
    }

    void componentChannelsOfTheShippedWorkbook() {
        const project::WorkbookFile wb = shippedWorkbook();
        const auto graph = componentChannels(componentOfType(wb, "time_distance"));
        QVERIFY(graph.size() >= 5);
        QCOMPARE(graph.front().name, std::string("Corr Speed"));
        const auto values = componentChannels(componentOfType(wb, "values_list"));
        QCOMPARE(values.size(), std::size_t{9});
        QCOMPARE(values.front().displayUnit, std::string("km/h"));
        QCOMPARE(*values.front().dps, 1);
        project::ComponentSpec unknown;
        unknown.type = "gauge_numeric";
        QVERIFY(componentChannels(unknown).empty());
    }

    // AC-M1-05's shared preparation: the inputs a worker receives, resolved per slot.
    void prepareInputsResolvesSlotsAndChannels() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(w.a) && w.ready(w.b), 30000);
        w.selection.setMain(rangeRef(*w.a, 2));
        w.selection.toggleOverlay(rangeRef(*w.b, 1));
        const project::WorkbookFile wb = shippedWorkbook();
        project::ComponentSpec spec = componentOfType(wb, "time_distance");
        // Our synthetic sources log "Throttle" (the workbook's THROTTLE resolves case-insensitively), not
        // SPEED.
        WorksheetViewModel ws(wb.worksheets.front(), w.workspace, w.selection, w.distance);
        ws.setSize(800);
        const auto metrics = std::make_shared<const plot::FixedTextMetrics>();
        const auto scheme = std::make_shared<const project::Scheme>();
        const plot::ComponentBuildInputs in = w.render.prepareInputs(
            spec, ws.state(), w.selection.snapshot(), plot::LayoutContext{800, 400, 1.0F}, scheme, metrics);
        QCOMPARE(in.slots.size(), std::size_t{2});
        QCOMPARE(in.slots[0].slot.kind, plot::SlotKind::Main);
        QCOMPARE(in.slots[0].source, w.selection.source(w.a->id));
        QCOMPARE(in.slots[0].range.start, rangeAt(*w.a, 2).start);
        QCOMPARE(in.slots[0].outing.end, rangeAt(*w.a, 0, "Outings").end);
        QVERIFY(in.slots[0].chain != nullptr);
        QCOMPARE(in.slots[1].slot.number, 1);
        const auto channels = componentChannels(spec);
        QCOMPARE(in.slots[0].channels.size(), channels.size());
        const plot::ResolvedChannel& corr = in.slots[0].channels[0];
        QCOMPARE(corr.name, std::string("Corr Speed"));
        QVERIFY(corr.descriptor.has_value());
        QCOMPARE(corr.status, model::ChannelStatus::Ok);
        QVERIFY(corr.series.has_value());
        QCOMPARE(corr.displayUnit, std::string("km/h"));
        QVERIFY(std::abs(corr.toDisplay(10.0) - 36.0) < 1e-9);
        bool sawThrottle = false;
        bool sawMissing = false;
        for (const plot::ResolvedChannel& c : in.slots[0].channels) {
            sawThrottle =
                sawThrottle || (c.name == "THROTTLE" && c.descriptor && c.descriptor->name == "Throttle");
            sawMissing = sawMissing || (c.name == "SPEED" && !c.descriptor);
        }
        QVERIFY(sawThrottle);
        QVERIFY(sawMissing);
        QCOMPARE(in.units, w.maths.units());
        // The inputs drive the real builder (the same path as esses-render).
        const Result<plot::ComponentBuild> built = plot::TimeDistanceBuilder::buildStatic(in, CancelToken{});
        QVERIFY(built.ok());
        QVERIFY(!built->list.cmds.empty());
    }

    void dataWindowTree() {
        World w;
        w.selection.setMain(rangeRef(*w.a, 2));
        w.selection.toggleOverlay0(rangeRef(*w.a, 1));
        w.selection.toggleOverlay(rangeRef(*w.b, 2));
        DataWindowModel m(w.selection);
        QCOMPARE(m.rowCount(), 2);
        const QModelIndex srcA = m.index(0, 0);
        QCOMPARE(m.data(m.index(0, DataWindowModel::NameCol)).toString(), QStringLiteral("items-a.ld"));
        QCOMPARE(m.rowCount(srcA), 5); // one outing: laps directly under the source
        QCOMPARE(*m.sourceAt(srcA), w.a->id);
        const auto cell = [&](int row, int col) { return m.data(m.index(row, col, srcA)).toString(); };
        QCOMPARE(cell(2, DataWindowModel::MainCol), QStringLiteral("●"));
        QCOMPARE(cell(1, DataWindowModel::Overlay0Col), QStringLiteral("■"));
        QCOMPARE(cell(2, DataWindowModel::NameCol), QStringLiteral("Lap 2"));
        QCOMPARE(cell(2, DataWindowModel::TimeCol), QStringLiteral("0:48.000"));
        QCOMPARE(cell(1, DataWindowModel::DeltaFastestCol), QStringLiteral("+2.000"));
        QCOMPARE(cell(2, DataWindowModel::DeltaFastestCol), QStringLiteral("+0.000"));
        QCOMPARE(m.data(m.index(2, 0, srcA), DataWindowModel::SlotRole).toInt(), 100);
        QCOMPARE(m.data(m.index(1, 0, srcA), DataWindowModel::SlotRole).toInt(), 0);
        QVERIFY(m.data(m.index(0, 0, srcA), DataWindowModel::UntrustedRole).toBool()); // the out lap
        const QModelIndex srcB = m.index(1, 0);
        QCOMPARE(m.data(m.index(2, DataWindowModel::OverlayCol, srcB)).toString(), QStringLiteral("1"));
        QCOMPARE(*m.rangeAt(m.index(2, 0, srcB)), rangeRef(*w.b, 2));
        QVERIFY(!m.rangeAt(srcB));
        QCOMPARE(m.indexOf(rangeRef(*w.a, 3)).row(), 3);
        QCOMPARE(m.parent(m.index(3, 0, srcA)), srcA);
        // Slot changes repaint the slot columns.
        QSignalSpy changed(&m, &DataWindowModel::dataChanged);
        w.selection.nextRange();
        QVERIFY(changed.count() >= 1);
        QCOMPARE(cell(3, DataWindowModel::MainCol), QStringLiteral("●"));
        // The trust checkbox edits through the selection (sidecar commit) and the tree follows.
        const QModelIndex trust = m.index(1, DataWindowModel::TrustedCol, srcA);
        QVERIFY(m.flags(trust).testFlag(Qt::ItemIsUserCheckable));
        QCOMPARE(m.data(trust, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
        QVERIFY(m.setData(trust, Qt::Unchecked, Qt::CheckStateRole));
        QCOMPARE(m.data(m.index(1, DataWindowModel::TrustedCol, m.index(0, 0)), Qt::CheckStateRole).toInt(),
                 static_cast<int>(Qt::Unchecked));
        // A filter keeps the slots and hides the rest.
        w.selection.setFilter(w.a->id, FilterSpec{FilterSpec::Kind::LapNumbers, "2"});
        QCOMPARE(m.rowCount(m.index(0, 0)), 3); // Lap 2, plus Overlay 0 (Lap 1) and Main (Lap 3)
        QCOMPARE(m.headerData(DataWindowModel::MainCol, Qt::Horizontal).toString(), QStringLiteral("M"));
    }

    void channelsTree() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(w.a), 30000);
        ChannelsModel m(w.selection, w.maths);
        QCOMPARE(m.rowCount(), 2);
        const QModelIndex srcA = m.index(0, 0);
        QCOMPARE(m.rowCount(srcA), 2); // Logged, Maths
        const QModelIndex logged = m.index(0, 0, srcA);
        const QModelIndex mathsCat = m.index(1, 0, srcA);
        QCOMPARE(m.data(logged).toString(), QStringLiteral("Logged"));
        QCOMPARE(m.rowCount(logged), 3);
        QCOMPARE(m.data(m.index(0, 0, logged), ChannelsModel::ChannelNameRole).toString(),
                 QStringLiteral("Throttle"));
        QCOMPARE(m.data(m.index(0, 0, logged)).toString(), QStringLiteral("Throttle (%)"));
        QCOMPARE(m.data(m.index(1, 0, logged), ChannelsModel::ChannelNameRole).toString(),
                 QStringLiteral("Wheel Speed FL"));
        QCOMPARE(m.rowCount(mathsCat), 2);
        QCOMPARE(m.data(m.index(0, 0, mathsCat), ChannelsModel::ChannelNameRole).toString(),
                 QStringLiteral("Corr Dist"));
        QCOMPARE(m.data(m.index(0, 0, mathsCat), ChannelsModel::StatusRole).toInt(),
                 static_cast<int>(model::ChannelStatus::Ok));
        QCOMPARE(m.parent(m.index(0, 0, mathsCat)), mathsCat);
        QVERIFY(ChannelsModel::filterAccepts(QStringLiteral("Wheel Speed FL"), QStringLiteral("speed fl")));
        QVERIFY(!ChannelsModel::filterAccepts(QStringLiteral("Wheel Speed FL"), QStringLiteral("speed rr")));
        const QJsonObject mime =
            QJsonDocument::fromJson(ChannelsModel::channelsMime({QStringLiteral("Corr Speed")}, true))
                .object();
        QVERIFY(mime.value(QStringLiteral("newPanel")).toBool());
        w.selection.closeSource(w.b->id);
        QCOMPARE(m.rowCount(), 1);
    }

    // AC-M1-01's last step at the model level: the cursor moves the Values rows of Main and Overlay 1.
    void valuesFollowTheCursor() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(w.a) && w.ready(w.b), 30000);
        w.selection.setMain(rangeRef(*w.a, 2));       // 48 s lap
        w.selection.toggleOverlay(rangeRef(*w.a, 3)); // 48.3 s lap: Overlay 1, the reference
        WorksheetViewModel ws(shippedWorkbook().worksheets.front(), w.workspace, w.selection, w.distance);
        ValuesModel m(w.selection, w.maths);
        m.setChannelRequests({ChannelRequest{"Corr Speed", "km/h", 1}, ChannelRequest{"Throttle", "", 1},
                              ChannelRequest{"Nothing Here", "", std::nullopt}});
        QCOMPARE(m.rowCount(), 3);
        QCOMPARE(m.cellText(0, ValuesModel::MainCol), QString()); // no worksheet yet
        m.setWorksheet(&ws);
        ws.setCursor(0, CursorSource::Click);
        QTRY_VERIFY(m.cellText(0, ValuesModel::MainCol) != QStringLiteral("…") &&
                    !m.cellText(0, ValuesModel::MainCol).isEmpty());
        QCOMPARE(m.cellText(0, ValuesModel::UnitCol), QStringLiteral("km/h"));
        // At the lap start both laps run at L/T: 2000 m / 48 s and 2000 m / 48.3 s, in km/h.
        QVERIFY(std::abs(m.cellText(0, ValuesModel::MainCol).toDouble() - 2000.0 / 48.0 * 3.6) < 0.2);
        QVERIFY(std::abs(m.cellText(0, ValuesModel::ReferenceCol).toDouble() - 2000.0 / 48.3 * 3.6) < 0.2);
        QVERIFY(m.cellText(0, ValuesModel::DeltaCol).startsWith(QLatin1Char('+')));
        QCOMPARE(m.cellText(2, ValuesModel::MainCol), QString()); // not in the source
        QCOMPARE(m.cellText(1, ValuesModel::NameCol), QStringLiteral("Throttle"));
        // Drag the cursor: the rows change (time mode: overlays aligned at lap start).
        QSignalSpy changed(&m, &ValuesModel::dataChanged);
        const QString before = m.cellText(1, ValuesModel::MainCol);
        ws.setCursor(12 * kSec, CursorSource::Click);
        QVERIFY(changed.count() >= 1);
        QTRY_VERIFY(m.cellText(1, ValuesModel::MainCol) != QStringLiteral("…"));
        QVERIFY(m.cellText(1, ValuesModel::MainCol) != before);
        // Distance mode: Overlay 1 is read at the same lap distance, where the throttle phase is equal.
        ws.setDomain(plot::Domain::Distance);
        QTRY_VERIFY(m.cellText(1, ValuesModel::ReferenceCol) != QStringLiteral("…"));
        QVERIFY(std::abs(m.cellText(1, ValuesModel::MainCol).toDouble() -
                         m.cellText(1, ValuesModel::ReferenceCol).toDouble()) < 1.0);
        // F4 hides the reference.
        w.selection.toggleOverlaysHidden();
        QCOMPARE(m.cellText(1, ValuesModel::ReferenceCol), QString());
        // "All channels": every channel of the Main source, sorted.
        m.setAllChannels(true);
        QCOMPARE(m.rowCount(), 5);
        QCOMPARE(m.cellText(0, ValuesModel::NameCol), QStringLiteral("Corr Dist"));
    }
};

QTEST_GUILESS_MAIN(TstRenderItems)
#include "tst_render_items.moc"
