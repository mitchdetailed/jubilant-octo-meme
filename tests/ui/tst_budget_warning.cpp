// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M4-02 (07 section 7.2; 07 section 7.1 row 1, demo 7.5 line 3; 01 section 7.2; D-043, D-206 (c)-(e)),
// offscreen, in the real shell (MainWindow on a private configuration directory, the shipped workbook) over a
// written synthetic .ld (three channels at 1 kHz over 217 s) whose resident estimate, 4.2 MB, exceeds the
// runtime's 2 MiB budget: the open stops before publication and the Messages dock shows the Budget warning
// with the estimate, Continue and Cancel; nothing of the file is decoded while it waits (the chunk cache is
// never asked for a chunk, the source is in neither the selection nor the maths); Continue from the row
// publishes it and the status bar's Performance line shows the resident bytes against the budget; Cancel
// drops it with an Import Log row and leaves nothing behind (the same file opens at once when the budget
// allows it). The expected estimate and text are worked out by hand from the chunk layout of 01 section 7.1.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/dock_host.hpp>

#include <QLabel>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <optional>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr std::size_t kBudget = std::size_t{2} << 20; // 2 MiB ("2.0 MB")
// Three F32 channels of 217,000 samples: per channel 3 full chunks of 64 + 445,120 bytes and one of 20,392
// samples, 64 + 138,560 bytes (samples 81,600, bitmap 2,560, pyramid 6,800 entries = 54,400); 1,474,176 bytes
// per channel, 4,422,528 in all: "4.2 MB".
constexpr std::uint64_t kEstimate = 4'422'528;
const QString kWarningText =
    QStringLiteral("needs about 4.2 MB of memory once loaded, above the 2.0 MB memory "
                   "budget (0 bytes in use). Continue to open it anyway, or Cancel.");

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

// The log, written from a cache of its own so the runtime's cache starts empty.
std::filesystem::path writeLog(const QTemporaryDir& dir) {
    const ThreadRole::Scope blocking(ThreadRole::Normal); // writing reads every chunk
    store::ChunkCache scratch(std::size_t{256} << 20);
    qt::test::SyntheticOptions o;
    o.path = "budget.ld";
    o.rateHz = 1000;
    return qt::test::writeLd(*qt::test::makeSource(o, scratch), fsPath(dir.path()), "budget");
}

// The chunk cache was never asked for a chunk and holds none.
bool untouched(qt::AppRuntime& rt) {
    const store::ChunkCache::Stats s = rt.cache().stats();
    return s.chunks == 0 && s.residentBytes == 0 && s.misses == 0 && s.hits == 0;
}

// The newest Messages row with `code`, or -1.
int rowWithCode(MainWindow& w, const QString& code) {
    for (int r = w.messages().rowCount() - 1; r >= 0; --r) {
        if (w.messages().at(r).code == code) {
            return r;
        }
    }
    return -1;
}

// The Messages dock's list index of a model row.
QModelIndex listIndex(MainWindow& w, int row) {
    return w.messagesWindow().filterModel().mapFromSource(w.messages().index(row));
}

struct Window {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt = uitest::makeRuntime();
    std::filesystem::path ld;
    std::optional<MainWindow> w;

    bool start() {
        if (!config.isValid() || !logs.isValid()) {
            return false;
        }
        ld = writeLog(logs);
        rt->cache().setBudgetBytes(kBudget); // the configured budget: below the file's estimate
        w.emplace(*rt, uitest::pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        return uitest::showAndActivate(*w);
    }
    ~Window() {
        w.reset();
        rt->shutdown();
    }
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

} // namespace

class TstBudgetWarning : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void cleanup() { QCOMPARE(uitest::assertFailures(), 0); }

    // The warning with the estimate before any chunk is decoded; Continue; the Performance line.
    void aFileAboveTheBudgetWaitsForContinue() {
        Window x;
        QVERIFY(x.start());
        MainWindow& w = *x.w;
        BudgetShell* budget = shellExtension<BudgetShell>(w);
        QVERIFY(budget != nullptr && budget->performanceLabel() != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
        QVERIFY(untouched(*x.rt));

        w.openLog(x.ld);
        QTRY_COMPARE_WITH_TIMEOUT(w.loader().pendingBudgetOpens(), 1, kTimeoutMs);
        const int row = rowWithCode(w, QString::fromLatin1(BudgetShell::kWarningCode));
        QVERIFY(row >= 0);
        const qt::Message m = w.messages().at(row);
        QCOMPARE(m.severity, qt::Message::Severity::Warning);
        QCOMPARE(m.origin, qt::Message::Origin::Budget);
        QVERIFY(fsPath(m.source).filename() == x.ld.filename());
        QCOMPARE(m.text, kWarningText); // the estimate, the budget and what is in use
        QCOMPARE(m.action, QStringLiteral("file.continue_open"));
        QCOMPARE(m.altAction, QStringLiteral("file.cancel_open"));
        QCOMPARE(m.actionArgs.size(), 1);
        QCOMPARE(m.altActionArgs, m.actionArgs);
        const qt::OpenBudgetCheck* check = w.loader().pendingBudgetCheck(m.actionArgs.front().toInt());
        QVERIFY(check != nullptr);
        QCOMPARE(check->estimateBytes, kEstimate);
        QCOMPARE(check->budgetBytes, std::uint64_t{kBudget});
        QVERIFY(w.docks().isVisible(QString::fromLatin1(MessagesWindow::kDockId))); // shown with the row
        QVERIFY(w.lastHint().contains(QStringLiteral("above the memory budget")));
        const QString shown = listIndex(w, row).data().toString();
        QVERIFY2(shown.endsWith(QStringLiteral("[Continue Opening]  [Cancel Opening]")), qPrintable(shown));

        // Waiting: nothing of the file is decoded, published or registered with the maths.
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
        QVERIFY(untouched(*x.rt));
        QVERIFY(w.selection().sources().empty());
        QVERIFY(w.maths().sources().empty());
        QCOMPARE(w.loader().pendingOpens(), 0);
        QVERIFY(w.commands().isEnabled(QString::fromLatin1(BudgetShell::kContinueCommand)));

        // Continue: the row's action, from the dock.
        QVERIFY(w.messagesWindow().invokeAction(listIndex(w, row)));
        QCOMPARE(w.loader().pendingBudgetOpens(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kTimeoutMs);
        QVERIFY(!w.commands().isEnabled(QString::fromLatin1(BudgetShell::kContinueCommand)));
        QVERIFY(!w.messagesWindow().invokeAction(listIndex(w, row))); // answered: the link does nothing now
        QCOMPARE(w.selection().sources().size(), std::size_t{1});

        // The Performance line: the resident bytes against the budget.
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
        budget->monitor()->refresh();
        const std::size_t resident = x.rt->cache().stats().residentBytes;
        QVERIFY(resident > 0); // the shipped worksheets and the built-in maths read the file now
        QCOMPARE(budget->performanceLabel()->text(),
                 QStringLiteral("Resident %1 of 2.0 MB%2")
                     .arg(qt::formatBytes(resident),
                          resident > kBudget ? QStringLiteral(" (over budget)") : QString()));
        QCOMPARE(w.statusField(QString::fromLatin1(BudgetShell::kStatusField)), budget->performanceLabel());
        QVERIFY(w.messagesWindow().performanceText().startsWith(
            QStringLiteral("Memory: %1 resident of the 2.0 MB budget").arg(qt::formatBytes(resident))));
    }

    // Cancel (the row's alternate action) drops the open cleanly.
    void cancelDropsTheOpenCleanly() {
        Window x;
        QVERIFY(x.start());
        MainWindow& w = *x.w;
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
        w.openLog(x.ld);
        QTRY_COMPARE_WITH_TIMEOUT(w.loader().pendingBudgetOpens(), 1, kTimeoutMs);
        const int row = rowWithCode(w, QString::fromLatin1(BudgetShell::kWarningCode));
        QVERIFY(row >= 0);
        QVERIFY(w.messagesWindow().invokeAltAction(listIndex(w, row)));
        QCOMPARE(w.loader().pendingBudgetOpens(), 0);
        const int cancelled = rowWithCode(w, QStringLiteral("open.budget_cancelled"));
        QVERIFY(cancelled > row);
        QCOMPARE(w.messages().at(cancelled).origin, qt::Message::Origin::Import);
        QCOMPARE(w.messages().at(cancelled).severity, qt::Message::Severity::Info);
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
        QVERIFY(untouched(*x.rt));
        QVERIFY(w.selection().sources().empty());
        QVERIFY(w.maths().sources().empty());
        QVERIFY(!w.commands().isEnabled(QString::fromLatin1(BudgetShell::kCancelCommand)));

        // Nothing is left behind: with room in the budget the same file opens at once, without a warning.
        const int rows = w.messages().rowCount();
        x.rt->cache().setBudgetBytes(std::size_t{512} << 20);
        w.openLog(x.ld);
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kTimeoutMs);
        QCOMPARE(w.loader().pendingBudgetOpens(), 0);
        for (int r = rows; r < w.messages().rowCount(); ++r) {
            QVERIFY(w.messages().at(r).code != QLatin1String(BudgetShell::kWarningCode));
        }
        QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*x.rt), kTimeoutMs);
    }
};

QTEST_MAIN(TstBudgetWarning)
#include "tst_budget_warning.moc"
