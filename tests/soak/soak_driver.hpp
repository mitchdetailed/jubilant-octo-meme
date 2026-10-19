// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Saturday driver (07 section 2.1, section 7.1 "Soak", AC-M4-03; D-213): one race engineer's day in the
// real shell, cycle after cycle, with the ten logs of soak_data.hpp. A cycle is one "hour" of the day:
//   1. restart (cycles 1, 5, 9, ...): save the project, close the window (exit), open a new window on the
//   project;
//      the session restore reopens the previous data and the selection;
//   2. open the logs that are not open, so ten are; on cycles 0, 3, 6, ... one open waits at the
//   memory-budget
//      gate (the cache's budget lowered for it) and Continue answers it, on cycles 1, 4, 7, ... Cancel does
//      and the file opens again with the budget restored;
//   3. settle, worksheet 0, settle: the cycle's sample (the state every cycle starts from);
//   4. selection: Main, Overlay 0 and overlays from the Data window's keys (even cycles) or the selection
//   model
//      (odd cycles) across the sources, N, P, Q, F4, J, CTRL+F, a lap-number filter set and cleared;
//   5. every worksheet: switched to (keys F12 on odd cycles), every component drawn; on a time/distance graph
//   the
//      zoom, pan, cursor and datum keys, F3, M, F9 twice, a wheel step and a click; reports filled and
//      copied; the Track Map dock shown and drawn, then hidden; the Values and Channels docks toggled;
//   6. edits: a maths module of two items pushed and its channel added to the graph (recalculated and drawn),
//      the expression changed (recalculated again), a lap's trust toggled, the Maths Editor opened and
//      closed; undo to the cycle's start, redo, the journal written by autosave, undo again, save (journal
//      cleared);
//   7. reload: one .ld replaced on disk by a rename-over writer (another version or the same bytes), the
//      application activated, the stale source reloaded from the Messages row or File > Reload File;
//   8. close: three sources (Data window DELETE, the loader, Data window DELETE) or, on cycles 1, 6, 11, ...,
//      Close All; Messages cleared; settle.
// Every wait is on a state (the pool settled with every posted result delivered, a model's busy flag, a
// component's Static layer current), with a generous timeout; nothing waits for a fixed time. A step that
// cannot complete fails the test with the step's name.
#pragma once

#include "soak_data.hpp"
#include "soak_metrics.hpp"

#include <esses/base/paths.hpp>
#include <esses/model/ids.hpp>

#include <QString>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace esses::qt {
class AppRuntime;
} // namespace esses::qt

namespace esses::widgets {
class MainWindow;
} // namespace esses::widgets

namespace esses::soak {

// One sample per cycle, taken in the state of step 3.
struct CycleSample {
    int cycle = 0;
    double elapsedS = 0.0;
    bool restarted = false; // the window was new this cycle (excluded from the object and heap series)
    std::uint64_t residentBytes = 0;
    std::uint64_t heapBytes = 0;
    std::uint64_t cacheResidentBytes = 0;
    std::uint64_t cacheChunks = 0;
    std::uint64_t poolPeak = 0; // the most queued + parked + running tasks seen during the previous cycle
    std::uint64_t dispatcherPeak = 0; // the most closures queued on the Dispatcher during the previous cycle
    int widgets = 0;                  // QApplication::allWidgets()
    int windowObjects = 0;            // QObjects under the main window
    int sources = 0;
    int undoSteps = 0;
    double cycleS = 0.0; // the previous cycle's wall time
};

struct DriverOptions {
    std::filesystem::path configDir;
    std::filesystem::path projectDir;
    std::size_t settleTimeoutMs = 900'000; // the longest a step may take to settle (15 min under ASan)
};

class SaturdayDriver {
public:
    SaturdayDriver(qt::AppRuntime& runtime, std::vector<LogFile> logs, DriverOptions options);
    ~SaturdayDriver();
    SaturdayDriver(const SaturdayDriver&) = delete;
    SaturdayDriver& operator=(const SaturdayDriver&) = delete;

    // A new project in projectDir and a window on it (QVERIFY-style: check QTest::currentTestFailed()).
    void start();
    // One cycle (k from 0).
    void cycle(int k);
    // Close every log, check the cache gives every chunk back, close the window.
    void finish();

    const std::vector<CycleSample>& samples() const noexcept { return samples_; }
    // The steps run so far, per kind ("budget-continue", "reload-variant", ...), for the summary.
    std::string coverage() const;

private:
    widgets::MainWindow& w();
    void openWindow();
    void restart();
    void openMissing(int k);
    void openWithBudgetGate(const LogFile& log, bool proceed);
    void sample(int k);
    void selection(int k);
    void worksheets(int k);
    void exerciseGraph(int k);
    void trackMapDock();
    void edits(int k, int startIndex);
    void reloadOne(int k);
    void closeSome(int k);
    void settle(const char* step);
    void waitComponentsReady(const char* step);
    bool componentsReady();
    bool quiet();
    std::optional<model::SourceId> sourceOf(const LogFile& log);
    std::vector<model::SourceId> openSources(bool ldOnly);
    void note(const std::string& step);
    // "soak <seconds> s cycle <k>: <step>" on stderr (the job's log; the Qt Test log is its own file).
    void progress(int k, const char* step) const;
    void startPeakSampler();

    qt::AppRuntime& rt_;
    std::vector<LogFile> logs_;
    DriverOptions options_;
    std::unique_ptr<widgets::MainWindow> window_;
    std::vector<CycleSample> samples_;
    std::vector<std::pair<std::string, int>> coverage_;
    std::vector<bool> variantOnDisk_; // per log: the variant's bytes are under the log's name
    QTimer* peakTimer_ = nullptr;
    std::uint64_t poolPeak_ = 0;
    std::uint64_t dispatcherPeak_ = 0;
    bool restartedThisCycle_ = false;
    std::int64_t startMs_ = 0;
    std::int64_t cycleStartMs_ = 0;
    double lastCycleS_ = 0.0;
    int asideCounter_ = 0;
};

} // namespace esses::soak
