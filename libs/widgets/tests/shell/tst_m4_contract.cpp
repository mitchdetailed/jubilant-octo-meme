// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The final parts of the esses_widgets M4 contract (D-206, docs/milestones/M4.md section 2): the one shell
// hook (one ShellExtension per M4 package, children of the window in the contract's order, found with
// shellExtension<T>, deleted with the window before its widgets), the planned M4 rows untouched by the stubs
// (file.reload_source and help.contents stay "planned for M4"), Help > About (wave 2's placeholder dialog),
// the build identification behind `esses --version`, DockHost::windowOf, and the signatures the packages
// implement (ComponentWidget::recheckDevicePixelRatio, MessagesWindow's M4 members, DprWatcher). The
// behaviour behind the stubs is tested by the packages that replace them.

#include "properties_shell/shell_test_support.hpp"

#include <esses/base/version.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/about_shell.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/docs_shell.hpp>
#include <esses/widgets/shell/dpr_watcher.hpp>
#include <esses/widgets/shell/followups_shell.hpp>
#include <esses/widgets/shell/hidpi_shell.hpp>
#include <esses/widgets/shell/reload_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <type_traits>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

struct Shell {
    QTemporaryDir config;
    std::unique_ptr<qt::AppRuntime> rt = runtime(2);
    std::unique_ptr<MainWindow> w;

    bool start() {
        if (!config.isValid()) {
            return false;
        }
        w = std::make_unique<MainWindow>(*rt, pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        return showAndActivate(*w);
    }
    ~Shell() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }
};

QString firstLine(const QString& text) {
    return text.section(QLatin1Char('\n'), 0, 0);
}

} // namespace

class TstM4Contract : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    // One extension per package, as children of the window in the contract's order, deleted with the window.
    void oneExtensionPerPackage() {
        auto sh = std::make_unique<Shell>();
        QVERIFY(sh->start());
        const QList<ShellExtension*> all =
            sh->w->findChildren<ShellExtension*>(QString(), Qt::FindDirectChildrenOnly);
        QStringList keys;
        for (const ShellExtension* e : all) {
            keys << e->key();
            QCOMPARE(e->objectName(), QStringLiteral("m4.") + e->key());
        }
        QCOMPARE(keys,
                 (QStringList{QStringLiteral("budget"), QStringLiteral("reload"), QStringLiteral("hidpi"),
                              QStringLiteral("followups"), QStringLiteral("about"), QStringLiteral("docs")}));
        QVERIFY(shellExtension<BudgetShell>(*sh->w) != nullptr);
        QVERIFY(shellExtension<ReloadShell>(*sh->w) != nullptr);
        QVERIFY(shellExtension<HiDpiShell>(*sh->w) != nullptr);
        QVERIFY(shellExtension<FollowupsShell>(*sh->w) != nullptr);
        QVERIFY(shellExtension<AboutShell>(*sh->w) != nullptr);
        QVERIFY(shellExtension<DocsShell>(*sh->w) != nullptr);
        QCOMPARE(shellExtension<BudgetShell>(*sh->w)->key(), QString::fromLatin1(BudgetShell::kKey));
        QCOMPARE(shellExtension<ReloadShell>(*sh->w)->key(), QString::fromLatin1(ReloadShell::kKey));
        QCOMPARE(shellExtension<HiDpiShell>(*sh->w)->key(), QString::fromLatin1(HiDpiShell::kKey));
        QCOMPARE(shellExtension<FollowupsShell>(*sh->w)->key(), QString::fromLatin1(FollowupsShell::kKey));
        QCOMPARE(shellExtension<AboutShell>(*sh->w)->key(), QString::fromLatin1(AboutShell::kKey));
        QCOMPARE(shellExtension<DocsShell>(*sh->w)->key(), QString::fromLatin1(DocsShell::kKey));
        QCOMPARE(QString::fromLatin1(DocsShell::kCommand), QStringLiteral("help.contents"));
        QCOMPARE(QString::fromLatin1(ReloadShell::kCommand), QStringLiteral("file.reload_source"));
        QCOMPARE(QString::fromLatin1(BudgetShell::kStatusField), QStringLiteral("performance"));

        QPointer<ShellExtension> budget(shellExtension<BudgetShell>(*sh->w));
        QPointer<ShellExtension> about(shellExtension<AboutShell>(*sh->w));
        sh->w.reset();
        QVERIFY(budget.isNull());
        QVERIFY(about.isNull());
    }

    // The keymap's M4 rows stay planned until their package implements them (the generic row of 06 section
    // 14.5 keeps covering them); file.reload_source is implemented by the reload package (D-208),
    // help.contents waits for the docs package of wave 2.
    void plannedM4RowsStayPlanned() {
        Shell sh;
        QVERIFY(sh.start());
        for (const QString& id : {QStringLiteral("help.contents")}) {
            QVERIFY2(sh.w->commands().find(id) != nullptr, qPrintable(id));
            QVERIFY2(!sh.w->commands().isEnabled(id), qPrintable(id));
            QAction* action = sh.w->actions().action(id);
            QVERIFY2(action != nullptr && action->toolTip().contains(QStringLiteral("planned for M4")),
                     qPrintable(id));
        }
    }

    // Help > About (wave 2): a Help-menu command without a keymap row that opens the About dialog.
    void helpAboutOpensTheAboutDialog() {
        Shell sh;
        QVERIFY(sh.start());
        const QString id = QString::fromLatin1(AboutShell::kCommand);
        QVERIFY(sh.w->commands().find(id) != nullptr);
        QVERIFY(sh.w->commands().isEnabled(id));
        QMenu* help = sh.w->actions().menu(QStringLiteral("Help"));
        QVERIFY(help != nullptr);
        QVERIFY(help->actions().contains(sh.w->actions().action(id)));
        AboutShell* about = shellExtension<AboutShell>(*sh.w);
        QVERIFY(about->dialog() == nullptr);
        QVERIFY(sh.w->commands().invoke(id));
        QTRY_VERIFY(about->dialog() != nullptr && about->dialog()->isVisible());
        QCOMPARE(firstLine(about->dialog()->text()), firstLine(versionReport()));
        about->dialog()->reject();
        QTRY_VERIFY(about->dialog() == nullptr); // deleted on close
    }

    // `esses --version` (AC-M4-09): its first line stays "esses <version>" (app.version_console looks for
    // it); the build fields come from the stamped version and the Qt the build runs on.
    void buildIdentification() {
        QCOMPARE(firstLine(versionReport()),
                 QStringLiteral("esses ") + QString::fromStdString(std::string(versionString())));
        const BuildInfo b = buildInfo();
        QCOMPARE(b.version, QString::fromStdString(std::string(versionInfo().string)));
        QCOMPARE(b.commit, QString::fromStdString(std::string(versionInfo().gitSha)));
        QCOMPARE(b.buildDate, QString::fromStdString(std::string(versionInfo().buildDate)));
        QCOMPARE(b.appName, QString::fromStdString(std::string(versionInfo().appName)));
        QCOMPARE(b.qtRuntime, QString::fromLatin1(qVersion()));
        QCOMPARE(b.qtBuild, QStringLiteral(QT_VERSION_STR));
    }

    // DockHost::windowOf: the host window for a docked dock, the dock's own window once it floats.
    void dockWindowOf() {
        Shell sh;
        QVERIFY(sh.start());
        DockHost& docks = sh.w->docks();
        QVERIFY(docks.windowOf(QStringLiteral("no such dock")) == nullptr);
        const QString data = QStringLiteral("data");
        QCOMPARE(docks.windowOf(data), static_cast<QWidget*>(sh.w.get()));
        docks.setState(data, DockState::Floating);
        QTRY_VERIFY(docks.windowOf(data) != nullptr && docks.windowOf(data) != sh.w.get());
        QVERIFY(docks.windowOf(data)->isWindow());
        QVERIFY(docks.windowOf(data)->isAncestorOf(docks.content(data)) ||
                docks.windowOf(data) == docks.content(data));
        docks.setState(data, DockState::Docked);
        QTRY_COMPARE(docks.windowOf(data), static_cast<QWidget*>(sh.w.get()));
    }

    // The members the packages implement keep the contract's signatures.
    void packageSignatures() {
        static_assert(
            std::is_same_v<decltype(&ComponentWidget::recheckDevicePixelRatio), bool (ComponentWidget::*)()>);
        static_assert(std::is_same_v<decltype(&MessagesWindow::invokeAltAction),
                                     bool (MessagesWindow::*)(const QModelIndex&)>);
        static_assert(std::is_same_v<decltype(&MessagesWindow::showPerformance),
                                     void (MessagesWindow::*)(const qt::PerformanceSnapshot&)>);
        static_assert(std::is_same_v<decltype(&DprWatcher::refreshComponents), int (*)(QWidget&)>);
        static_assert(std::is_same_v<decltype(&ReloadShell::checkNow),
                                     std::vector<model::SourceId> (ReloadShell::*)()>);
        const DprWatcher watcher; // constructible on its own, without a window
        QVERIFY(watcher.parent() == nullptr);
    }
};

QTEST_MAIN(TstM4Contract)
#include "tst_m4_contract.moc"
