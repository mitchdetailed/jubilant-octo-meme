// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M2 contract smoke test of the maths editors (src/editors_maths): MathsEditor and ChannelStatusWindow
// construct over the real services offscreen, the editor is modeless and the Channel Status dock shows the
// seven columns of 06 section 10 through its filter proxy. The behaviour (the 03 section 11 rows, AC-M2-12,
// the Channel Editor and the active-channel rows) is tested in tst_maths_editor, tst_channel_status_window,
// tst_channel_editor and tst_active_channel.

#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)
#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>

#include <QApplication>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTest>

using namespace esses;
using namespace esses::widgets;

class TstMathsEditorsContract : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void constructOffscreen() {
        qt::AppRuntimeOptions o;
        o.workers = 1;
        o.cacheBudget = std::size_t{16} << 20;
        qt::AppRuntime rt(o);
        {
            PathOverrides po;
            const auto dir = qt::test::scratchDir("maths-editors-contract");
            po.configDir = dir / "config";
            po.dataDir = qt::test::repoRoot() / "data";
            po.cacheDir = dir / "cache";
            po.logDir = dir / "logs";
            auto paths = Paths::resolve(po);
            QVERIFY(paths.ok());
            qt::ProjectSession session(*paths);
            qt::MathsService maths(rt.pool(), rt.dispatcher(), qt::test::shippedUnits());
            qt::CommandRegistry commands;
            qt::ChannelStatusModel status(maths, &session);
            QCOMPARE(status.columnCount(), qt::ChannelStatusModel::kColumnCount);
            QCOMPARE(status.headerData(qt::ChannelStatusModel::ResolvedColumn, Qt::Horizontal).toString(),
                     QStringLiteral("Resolved to"));

            MathsEditor editor(session, maths);
            QVERIFY(!editor.isModal());
            QVERIFY(editor.canClose());
            ChannelStatusWindow window(status, commands);
            QCOMPARE(window.filterModel().sourceModel(), &status);
            QCOMPARE(window.table().model(), &window.filterModel());
            QCOMPARE(QString(ChannelStatusWindow::kDockId), QStringLiteral("channelstatus"));
        }
        rt.shutdown();
    }
};

QTEST_MAIN(TstMathsEditorsContract)
#include "tst_maths_editors_contract.moc"
