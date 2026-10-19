// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M4 budget package in esses_widgets (07 section 7.1 row 1, AC-M4-02; 01 section 7.2; 06 sections 2.1 and
// 11; D-011, D-206 (d)-(e)): a Messages row's alternate action (its second link, SHIFT+ENTER and
// SHIFT+double-click, the row menu), the dock's Stats line, and BudgetShell in a MainWindow (the status bar's
// Performance field and its click, the row-less Continue and Cancel commands, the gate on the loader, the
// budget setting applied at start, on change and given back at teardown). The open-time warning end to end is
// tests/ui/tst_budget_warning.cpp.

#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>

#include <QAction>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <vector>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

constexpr std::size_t kMiB = std::size_t{1} << 20;
constexpr std::size_t kGiB = std::size_t{1} << 30;

struct Shell {
    QTemporaryDir config;
    std::unique_ptr<qt::AppRuntime> rt = runtime(2); // a 128 MiB budget
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

// Every action reachable from the menu bar, submenus included.
void collect(const QMenu& menu, QList<QAction*>& out) {
    for (QAction* a : menu.actions()) {
        out << a;
        if (a->menu() != nullptr) {
            collect(*a->menu(), out);
        }
    }
}

struct Recorded {
    QString id;
    QVariantList args;
};

} // namespace

class TstBudgetShell : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    // The budget warning's row: Continue as the action, Cancel as the alternate action.
    void aRowRunsItsAlternateAction() {
        qt::MessagesModel model;
        qt::CommandRegistry commands;
        std::vector<Recorded> ran;
        bool cancelEnabled = true;
        for (const auto& [id, title] :
             {std::pair{QStringLiteral("file.continue_open"), QStringLiteral("Continue Opening")},
              std::pair{QStringLiteral("file.cancel_open"), QStringLiteral("Cancel Opening")}}) {
            qt::CommandSpec s;
            s.id = id;
            s.title = title;
            s.invoke = [&ran, command = id](const QVariantList& args) { ran.push_back({command, args}); };
            if (id == QStringLiteral("file.cancel_open")) {
                s.enabled = [&cancelEnabled] { return cancelEnabled; };
            }
            commands.add(std::move(s));
        }
        MessagesWindow win(model, commands);
        qt::Message plain;
        plain.text = QStringLiteral("a plain row");
        model.append(plain);
        qt::Message m;
        m.severity = qt::Message::Severity::Warning;
        m.origin = qt::Message::Origin::Budget;
        m.source = QStringLiteral("big.ld");
        m.text = QStringLiteral("needs about 4.2 MB");
        m.action = QStringLiteral("file.continue_open");
        m.actionArgs = {7};
        m.altAction = QStringLiteral("file.cancel_open");
        m.altActionArgs = {7};
        model.append(m);
        const QModelIndex row = win.filterModel().index(1, 0);
        const QString text = row.data().toString();
        QVERIFY2(text.endsWith(QStringLiteral(
                     "Budget  big.ld: needs about 4.2 MB  [Continue Opening]  [Cancel Opening]")),
                 qPrintable(text));
        const QString tip = row.data(Qt::ToolTipRole).toString();
        QVERIFY(tip.contains(QStringLiteral("Double-click or Enter: Continue Opening")));
        QVERIFY(tip.contains(QStringLiteral("Shift+double-click or Shift+Enter: Cancel Opening")));

        QVERIFY(win.invokeAction(row));
        QVERIFY(win.invokeAltAction(row));
        QCOMPARE(ran.size(), std::size_t{2});
        QCOMPARE(ran[0].id, QStringLiteral("file.continue_open"));
        QCOMPARE(ran[0].args, QVariantList{7});
        QCOMPARE(ran[1].id, QStringLiteral("file.cancel_open"));
        QCOMPARE(ran[1].args, QVariantList{7});
        // The keyboard: ENTER the action, SHIFT+ENTER the alternate one.
        win.list().setCurrentIndex(row);
        QTest::keyClick(&win.list(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(&win.list(), Qt::Key_Return);
        QCOMPARE(ran.size(), std::size_t{4});
        QCOMPARE(ran[2].id, QStringLiteral("file.cancel_open"));
        QCOMPARE(ran[3].id, QStringLiteral("file.continue_open"));
        // A row without one, an invalid index, a disabled command: nothing runs.
        QVERIFY(!win.invokeAltAction(win.filterModel().index(0, 0)));
        QVERIFY(!win.invokeAltAction(QModelIndex()));
        cancelEnabled = false;
        QVERIFY(!win.invokeAltAction(row));
        QCOMPARE(ran.size(), std::size_t{4});

        // The row menu offers both, each with its own enabled state.
        QVERIFY(win.rowMenu(win.filterModel().index(0, 0)) == nullptr);
        QMenu* menu = win.rowMenu(row);
        QVERIFY(menu != nullptr);
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().at(0)->text(), QStringLiteral("Continue Opening"));
        QCOMPARE(menu->actions().at(1)->text(), QStringLiteral("Cancel Opening"));
        QVERIFY(menu->actions().at(0)->isEnabled());
        QVERIFY(!menu->actions().at(1)->isEnabled());
        cancelEnabled = true;
        menu = win.rowMenu(row); // the previous one is replaced
        QCOMPARE(win.findChildren<QMenu*>(QStringLiteral("messagesRowMenu")).size(), 1);
        menu->actions().at(1)->trigger();
        QCOMPARE(ran.size(), std::size_t{5});
        QCOMPARE(ran[4].id, QStringLiteral("file.cancel_open"));
        QCOMPARE(ran[4].args, QVariantList{7});
    }

    void theStatsLineShowsTheDetails() {
        qt::MessagesModel model;
        qt::CommandRegistry commands;
        MessagesWindow win(model, commands);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto* stats = win.findChild<QLabel*>(QStringLiteral("messagesStats"));
        QVERIFY(stats != nullptr);
        QVERIFY(!stats->isVisible()); // nothing to show yet
        QCOMPARE(win.performanceText(), QString());
        qt::PerformanceSnapshot s;
        s.budgetBytes = 2 * kGiB;
        s.cache.residentBytes = 3 * kMiB;
        s.cache.chunks = 7;
        s.pool.workers = 2;
        win.showPerformance(s);
        QVERIFY(stats->isVisible());
        QCOMPARE(win.performanceText(),
                 QStringLiteral(
                     "Memory: 3.0 MB resident of the 2.0 GB budget (0 %), 0 bytes pinned; 7 chunks (0 "
                     "pinned); 0 hits, 0 misses, 0 evictions. Tasks: 0 running, 0 queued (0 interactive, "
                     "0 visible, 0 background), 0 parked; 2 workers."));
        win.showPerformance(qt::PerformanceSnapshot{}); // the runtime is gone
        QVERIFY(!stats->isVisible());
        QCOMPARE(win.performanceText(), QString());
    }

    // The shell's side: the Performance field, the commands, the gate.
    void theShellWiresTheBudget() {
        Shell sh;
        QVERIFY(sh.start());
        BudgetShell* b = shellExtension<BudgetShell>(*sh.w);
        QVERIFY(b != nullptr);
        QVERIFY(b->monitor() != nullptr);
        QVERIFY(b->monitor()->running());
        QCOMPARE(b->monitor()->interval(), qt::PerformanceMonitor::kDefaultIntervalMs);
        QLabel* label = b->performanceLabel();
        QVERIFY(label != nullptr);
        QCOMPARE(sh.w->statusField(QStringLiteral("performance")), label);
        QCOMPARE(label->objectName(), QStringLiteral("status.performance"));
        b->monitor()->refresh();
        QCOMPARE(label->text(), QStringLiteral("Resident %1 of 128 MB")
                                    .arg(qt::formatBytes(sh.rt->cache().stats().residentBytes)));
        QVERIFY(label->toolTip().startsWith(QStringLiteral("Memory: ")));
        QVERIFY(sh.w->messagesWindow().performanceText().contains(QStringLiteral("of the 128 MB budget")));
        QVERIFY(sh.w->loader().budgetGate() == &sh.rt->cache());
        QCOMPARE(b->runtimeBudget(), 128 * kMiB);

        // Continue and Cancel: commands without a menu or a keymap row, enabled only while an open waits.
        QList<QAction*> menuActions;
        for (QAction* top : sh.w->menuBar()->actions()) {
            if (top->menu() != nullptr) {
                collect(*top->menu(), menuActions);
            }
        }
        for (const char* id : {BudgetShell::kContinueCommand, BudgetShell::kCancelCommand}) {
            const QString cmd = QString::fromLatin1(id);
            const qt::CommandSpec* spec = sh.w->commands().find(cmd);
            QVERIFY2(spec != nullptr, id);
            QVERIFY(spec->menuPath.isEmpty());
            QVERIFY(!sh.w->commands().isEnabled(cmd));
            QVERIFY(!sh.w->commands().invoke(cmd, {1}));
            QVERIFY(!menuActions.contains(sh.w->actions().action(cmd)));
            QVERIFY(sh.w->keymap().keysFor(cmd).isEmpty());
        }
        QCOMPARE(sh.w->commands().find(QString::fromLatin1(BudgetShell::kContinueCommand))->title,
                 QStringLiteral("Continue Opening"));
        QCOMPARE(sh.w->commands().find(QString::fromLatin1(BudgetShell::kCancelCommand))->title,
                 QStringLiteral("Cancel Opening"));

        // A worksheet switch clears the status fields; the Performance line comes back.
        if (sh.w->worksheetCount() > 1) {
            const QString line = label->text();
            sh.w->selectWorksheet(sh.w->activeWorksheetIndex() == 0 ? 1 : 0);
            QCOMPARE(label->text(), qt::performanceLine(b->monitor()->last()));
            QVERIFY(!line.isEmpty());
        }

        // A click on the field shows the Messages dock (06 section 2.1).
        const QString messages = QString::fromLatin1(MessagesWindow::kDockId);
        sh.w->docks().setState(messages, DockState::Closed);
        QVERIFY(!sh.w->docks().isVisible(messages));
        QTest::mouseClick(label, Qt::LeftButton);
        QTRY_VERIFY(sh.w->docks().isVisible(messages));
    }

    // settings.json memory.chunkCacheBudgetBytes: applied at start, on every change (raised to 1 GiB), the
    // runtime's own budget back when the key goes and at teardown.
    void theSettingSetsTheBudget() {
        Shell sh;
        QVERIFY(sh.config.isValid());
        const QString key = QString::fromLatin1(qt::kBudgetSettingKey);
        {
            qt::SettingsStore before(fsPath(sh.config.path()) / "settings.json");
            before.setValue(key, qint64{3} * qint64{1073741824});
            QVERIFY(before.saveNow().ok());
        }
        QVERIFY(sh.start());
        BudgetShell* b = shellExtension<BudgetShell>(*sh.w);
        QVERIFY(b != nullptr);
        QCOMPARE(b->runtimeBudget(), 128 * kMiB);
        QCOMPARE(sh.rt->cache().budgetBytes(), 3 * kGiB);
        QCOMPARE(b->performanceLabel()->text(),
                 QStringLiteral("Resident %1 of 3.0 GB")
                     .arg(qt::formatBytes(b->monitor()->last().cache.residentBytes)));
        sh.w->settings().setValue(key, qint64{536870912}); // 0.5 GiB: the floor
        QCOMPARE(sh.rt->cache().budgetBytes(), kGiB);
        sh.w->settings().setValue(key, QVariant()); // removed: the runtime's own budget
        QCOMPARE(sh.rt->cache().budgetBytes(), 128 * kMiB);
        sh.w->settings().setValue(key, 2.0 * 1073741824.0);
        QCOMPARE(sh.rt->cache().budgetBytes(), 2 * kGiB);
        QVERIFY(b->performanceLabel()->text().endsWith(QStringLiteral(" of 2.0 GB")));
        sh.w.reset();
        QCOMPARE(sh.rt->cache().budgetBytes(), 128 * kMiB); // given back with the window
    }
};

QTEST_MAIN(TstBudgetShell)
#include "tst_budget_shell.moc"
