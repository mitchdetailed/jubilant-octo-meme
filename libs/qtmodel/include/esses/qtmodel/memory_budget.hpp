// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The 0.1 memory budget in the GUI (M4 contract, D-206; 07 section 7.1 row 1, AC-M4-02; 01 sections 7.2 and
// 8.3; D-011, D-043): the user's budget setting, the open-time check SourceLoader raises, and the Stats that
// the Messages dock and the status bar's Performance line show.
//   * Setting: settings.json "memory.chunkCacheBudgetBytes" (02 section 6.9; Options > General). The default,
//     when the user never chose one, is the runtime's (ChunkCache::defaultBudgetBytes(): 25 % of RAM, at
//     least 1 GiB), so a test runtime's small budget is kept unless the setting says otherwise.
//   * Open-time check: SourceLoader::setBudgetGate (source_loader.hpp, "M4 budget"); an open whose resident
//     estimate (formats::estimateResident) exceeds the budget waits before publication, before any chunk is
//     decoded, and SourceLoader::budgetExceeded carries an OpenBudgetCheck. The shell turns it into a
//     Messages row (Budget, Warning, the estimate in the text) with Continue (action) and Cancel (alternate
//     action).
//   * Stats: PerformanceMonitor samples ChunkCache::stats(), the budget and TaskPool::stats() on the GUI
//     thread (a timer; the reads are lock-protected snapshots and never wait for a task) and announces each
//     snapshot; performanceLine() is the status-bar text, performanceDetails() the Messages dock's.
//
// Owner: the M4 budget package (docs/milestones/M4.md section 3; src/m4_budget/memory_budget.cpp, tested by
// tests/m4_budget/tst_memory_budget.cpp). The contract's (D-206): the key, kMinBudgetBytes,
// OpenBudgetCheck::exceeds, PerformanceMonitor (sampling, timer, signal). The texts are English in 0.1 and
// built in the C locale (no digit grouping of the byte counts).
#pragma once

#include <esses/base/taskpool.hpp>
#include <esses/store/chunk_cache.hpp>

#include <QObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>

class QTimer;

namespace esses::qt {

class AppRuntime;
class SettingsStore;

inline constexpr const char* kBudgetSettingKey = "memory.chunkCacheBudgetBytes";
inline constexpr std::size_t kMinBudgetBytes = std::size_t{1} << 30; // 1 GiB (D-011)

// The budget the user chose in settings.json, raised to kMinBudgetBytes; nullopt when the key is absent or
// not a positive number, an integer or a double (the runtime keeps its budget). A value above SIZE_MAX is
// SIZE_MAX.
std::optional<std::size_t> budgetFromSettings(const SettingsStore& settings);

// What SourceLoader::budgetExceeded reports for one open (07 section 7.1).
struct OpenBudgetCheck {
    QString path;                    // the file being opened
    std::uint64_t estimateBytes = 0; // formats::estimateResident(source).total()
    std::uint64_t budgetBytes = 0;   // ChunkCache::budgetBytes() at the check
    std::uint64_t residentBytes = 0; // ChunkCache::Stats::residentBytes at the check (reported, not added)
    // Final (D-206): 07 section 7.1 compares the estimate with the budget.
    bool exceeds() const noexcept { return estimateBytes > budgetBytes; }
};

// The Messages row text of a check, after the row's source (the file): "needs about 1.4 GB of memory once
// loaded, above the 1.0 GB memory budget (412 MB in use). Continue to open it anyway, or Cancel."
QString budgetWarningText(const OpenBudgetCheck& check);
// Bytes for people, 1 KB = 2^10, 1 MB = 2^20 and 1 GB = 2^30 bytes (as the Options field): "512 bytes",
// "1 byte", whole KB up to "1023 KB", "1.0 MB" to "9.9 MB" with one decimal, whole MB up to "1023 MB", then
// GB with one decimal ("1.0 GB", "1.4 GB"); each unit takes over where the smaller one would round to 1024.
QString formatBytes(std::uint64_t bytes);

struct PerformanceSnapshot {
    store::ChunkCache::Stats cache;
    std::size_t budgetBytes = 0;
    TaskPool::Stats pool;
};

// The status bar's Performance line ("Resident 412 MB of 4.0 GB", 06 section 2.1 "memory budget indicator"),
// with " (over budget)" when the resident bytes exceed the budget; empty for an empty snapshot (budget 0: the
// runtime is shut down).
QString performanceLine(const PerformanceSnapshot& snapshot);
// The Messages dock's Stats text (D-011 "Stats"), one paragraph: "Memory: 412 MB resident of the 4.0 GB
// budget (10 %), 12 MB pinned; 1,234 chunks (3 pinned); 5,678 hits, 90 misses, 0 evictions. Tasks: 2
// running, 5 queued (1 interactive, 0 visible, 4 background), 0 parked; 15 workers." Empty for an empty
// snapshot.
QString performanceDetails(const PerformanceSnapshot& snapshot);

// Samples the runtime's cache and pool on a GUI-thread timer and announces each snapshot. GUI thread only;
// the runtime must outlive it (MainWindow keeps it in its M4 budget extension).
class PerformanceMonitor final : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultIntervalMs = 1000;

    explicit PerformanceMonitor(AppRuntime& runtime, QObject* parent = nullptr);
    ~PerformanceMonitor() override;

    void setInterval(int ms); // clamped to >= 100
    int interval() const noexcept { return intervalMs_; }
    void start(); // the timer; refresh() on every tick
    void stop();
    bool running() const;
    // A snapshot now: ChunkCache::stats(), ChunkCache::budgetBytes(), TaskPool::stats() (empty once the
    // runtime is shut down).
    PerformanceSnapshot sample() const;
    // Samples now and announces it (updated()); the timer calls it.
    void refresh();
    const PerformanceSnapshot& last() const noexcept { return last_; }

Q_SIGNALS:
    void updated(const esses::qt::PerformanceSnapshot& snapshot);

private:
    AppRuntime& runtime_;
    QTimer* timer_ = nullptr;
    int intervalMs_ = kDefaultIntervalMs;
    PerformanceSnapshot last_;
};

} // namespace esses::qt
