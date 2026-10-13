// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M2 contract smoke test of the data editors (src/editors_data): ChannelEditor, MappingPanel, DetailsEditor
// and LapEditor construct over the real services offscreen, the dialogs are modeless and the Mapping panel
// blocks publication only while it shows a pending open. The owner replaces it with the 06 section 13
// behaviours.

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/editors_data/channel_editor.hpp>
#include <esses/widgets/editors_data/details_editor.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/editors_data/mapping_panel.hpp>

#include <QApplication>
#include <QTest>

using namespace esses;
using namespace esses::widgets;

class TstDataEditorsContract : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void constructOffscreen() {
        qt::AppRuntimeOptions o;
        o.workers = 1;
        o.cacheBudget = std::size_t{16} << 20;
        qt::AppRuntime rt(o);
        {
            PathOverrides po;
            const auto dir = qt::test::scratchDir("data-editors-contract");
            po.configDir = dir / "config";
            po.dataDir = qt::test::repoRoot() / "data";
            po.cacheDir = dir / "cache";
            po.logDir = dir / "logs";
            auto paths = Paths::resolve(po);
            QVERIFY(paths.ok());
            qt::ProjectSession session(*paths);
            qt::SourceLoader loader(rt.pool(), rt.dispatcher(), qt::test::shippedUnits());
            qt::DataSelectionModel selection;

            ChannelEditor channels(session, loader);
            QVERIFY(!channels.isModal());
            DetailsEditor details(session, loader);
            QVERIFY(!details.isModal());
            LapEditor laps(session, loader, selection);
            QVERIFY(!laps.isModal());

            MappingPanel panel(loader, session, qt::test::shippedUnits());
            QVERIFY(!panel.blocksPublish());
            formats::PendingResolution pending;
            pending.rows.push_back({"Oil Pres", "PSIG", std::nullopt, {1.0, 2.0}});
            panel.showPending(7, pending);
            QVERIFY(panel.blocksPublish());
            QCOMPARE(panel.pendingId(), 7);
            panel.cancel();
            QVERIFY(!panel.blocksPublish());
        }
        rt.shutdown();
    }
};

QTEST_MAIN(TstDataEditorsContract)
#include "tst_data_editors_contract.moc"
