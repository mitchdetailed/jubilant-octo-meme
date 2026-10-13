// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Plumbing smoke test of tests/ui (offscreen): the full shell assembles on a runtime and shuts down in the
// order of 01 section 5.3 rule 11, and the public M1 fixture is where the walkthrough expects it. The real
// suites (tst_walkthrough_m1, tst_f9_preservation, tst_corr_dist_prefetch, tst_render_parity) are added next
// to this file.

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QFileInfo>
#include <QTest>

using namespace esses;

class UiSmoke : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void fixtureExists() {
        const QString dir = QStringLiteral(ESSES_TEST_REPO_ROOT "/fixtures/motec.ld/synthetic-acc-dialect");
        QVERIFY(QFileInfo(dir + QStringLiteral("/expected.laps.json")).exists());
    }

    void shellStartsAndStops() {
        qt::AppRuntimeOptions options;
        options.workers = 2;
        options.cacheBudget = std::size_t{64} << 20;
        qt::AppRuntime rt(options);
        auto paths = Paths::resolve();
        QVERIFY(paths.ok());
        widgets::registerBuiltinComponents();
        {
            widgets::MainWindow window(rt, *paths);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
        }
        rt.shutdown();
        QVERIFY(rt.isShutDown());
    }
};

QTEST_MAIN(UiSmoke)
#include "tst_ui_smoke.moc"
