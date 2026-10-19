// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BudgetShell (M4 budget package; D-206, D-207; 07 section 7.1 row 1, AC-M4-02, demo 7.5 line 3; 01 section
// 7.2; 06 section 2.1 "memory budget indicator", section 11): the shell side of the 0.1 memory budget.
//   * The budget: settings.json memory.chunkCacheBudgetBytes (qt::budgetFromSettings, Options > General) is
//     applied to the runtime's ChunkCache when the window comes up and whenever the setting changes; without
//     the key the runtime keeps its budget (a test runtime's small one included), and a key that goes away
//     gives the runtime's own budget back. teardown() puts the runtime's own budget back too (a runtime can
//     outlive its window: the tests).
//   * The open-time warning: SourceLoader::setBudgetGate(&runtime.cache()); SourceLoader::budgetExceeded
//     becomes one Messages row (Budget, Warning, code kWarningCode, source the file, qt::budgetWarningText)
//     whose action is kContinueCommand and whose alternate action is kCancelCommand, both with the pending id
//     (commands without a keymap row or a menu, 06 section 11 "an action is a command id with arguments");
//     the Messages dock is shown and the status bar's message area says so. Both commands are enabled while
//     an open waits; without an id (the command palette) they answer the newest waiting open, and an id that
//     no longer waits leaves a hint.
//   * Stats: a qt::PerformanceMonitor (1 s) drives the status bar's Performance field (MainWindow::
//     statusField(kStatusField), qt::performanceLine, the details as its tooltip; a click shows Messages) and
//     MessagesWindow::showPerformance (the dock's Stats line, qt::performanceDetails); Continue samples at
//     once.
// Files of the package (docs/milestones/M4.md section 3): src/shell/main_window_m4_budget.cpp, this header,
// src/docks/messages_window.cpp with src/docks/m4_budget/, src/editors_options/options_dialog.cpp (the
// budget field on General), and the qtmodel/formats roots listed there.
#pragma once

#include <esses/widgets/shell/shell_extension.hpp>

#include <QMetaObject>
#include <QPointer>
#include <QVariant>
#include <cstddef>
#include <optional>
#include <vector>

class QEvent;
class QLabel;

namespace esses::qt {
class PerformanceMonitor;
struct OpenBudgetCheck;
struct PerformanceSnapshot;
} // namespace esses::qt

namespace esses::widgets {

class BudgetShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "budget";
    static constexpr const char* kStatusField = "performance";
    static constexpr const char* kContinueCommand = "file.continue_open"; // args: [pending id]
    static constexpr const char* kCancelCommand = "file.cancel_open";     // args: [pending id]
    static constexpr const char* kWarningCode = "budget.open_estimate";   // the Messages row's code

    explicit BudgetShell(MainWindow& window);
    ~BudgetShell() override;

    // The monitor behind the Performance line (nullptr until buildWidgets).
    qt::PerformanceMonitor* monitor() const;
    // The status bar's Performance field (nullptr until buildWidgets).
    QLabel* performanceLabel() const;
    // The runtime's budget when the window came up, before any setting (0 before windowReady).
    std::size_t runtimeBudget() const noexcept { return runtimeBudget_; }

    void buildWidgets(QWidget& central) override;
    void registerCommands() override;
    void windowReady() override;
    void worksheetActivated(int index) override;
    void teardown() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyBudgetSetting();
    void showSnapshot(const qt::PerformanceSnapshot& snapshot);
    void warn(int pendingId, const qt::OpenBudgetCheck& check);
    // The pending id of a command's arguments, else the newest open still waiting; nullopt when none waits.
    std::optional<int> pendingIdOf(const QVariantList& args) const;
    void answer(const QVariantList& args, bool proceed);

    QPointer<qt::PerformanceMonitor> monitor_;
    QPointer<QLabel> label_;
    std::size_t runtimeBudget_ = 0;
    std::optional<std::size_t> applied_; // the budget the setting put on the runtime
    std::vector<int> warned_;            // pending ids announced, oldest first
    std::vector<QMetaObject::Connection> connections_;
};

} // namespace esses::widgets
