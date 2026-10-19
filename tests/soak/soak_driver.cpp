// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "soak_driver.hpp"

#include "ui_test_support.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/utf8.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/layered_component.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/reload_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QIdentityProxyModel>
#include <QSortFilterProxyModel>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QUndoStack>
#include <QWheelEvent>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

// A step's wait: the condition polled with the event loop running until it holds, a failure at the timeout.
// Not QTRY_*: a QTRY that fails keeps waiting twice its timeout more (to report a condition that came true
// late), which would turn the soak's long timeouts into hours.
#define SOAK_WAIT(cond, message, timeoutMs)                                                                  \
    do {                                                                                                     \
        const int soakTimeoutMs = (timeoutMs);                                                               \
        if (!QTest::qWaitFor([&]() { return static_cast<bool>(cond); }, soakTimeoutMs)) {                    \
            QFAIL(qPrintable(                                                                                \
                QStringLiteral("%1 (not within %2 s)").arg(QString(message)).arg(soakTimeoutMs / 1000)));    \
        }                                                                                                    \
    } while (false)
#define SOAK_WAIT_EXPR(cond, timeoutMs) SOAK_WAIT(cond, #cond, timeoutMs)

namespace esses::soak {
namespace {

namespace fs = std::filesystem;

constexpr int kStepTimeoutMs = 300'000; // a single interactive step (a key's effect, a dialog)
constexpr std::size_t kLogs = 10;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

maths::MathItem item(const std::string& name, const std::string& expr) {
    maths::MathItem i;
    i.name = name;
    i.expr = expr;
    return i;
}

std::int64_t nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

std::size_t journalEntries(const fs::path& project, std::uintmax_t* bytes = nullptr) {
    std::size_t n = 0;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(project / ".journal", ec)) {
        if (e.is_regular_file(ec)) {
            ++n;
            total += e.file_size(ec);
        }
    }
    if (bytes != nullptr) {
        *bytes = total;
    }
    return n;
}

fs::path ldxOf(fs::path p) {
    p.replace_extension(".ldx");
    return p;
}

bool writeBytes(const fs::path& to, const std::string& bytes) {
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

} // namespace

SaturdayDriver::SaturdayDriver(qt::AppRuntime& runtime, std::vector<LogFile> logs, DriverOptions options)
    : rt_(runtime), logs_(std::move(logs)), options_(std::move(options)),
      variantOnDisk_(logs_.size(), false) {}

SaturdayDriver::~SaturdayDriver() {
    delete peakTimer_;
    window_.reset();
}

widgets::MainWindow& SaturdayDriver::w() {
    return *window_;
}

void SaturdayDriver::note(const std::string& step) {
    for (auto& [name, count] : coverage_) {
        if (name == step) {
            ++count;
            return;
        }
    }
    coverage_.emplace_back(step, 1);
}

void SaturdayDriver::progress(int k, const char* step) const {
    std::fprintf(stderr, "soak %8.1f s cycle %d: %s\n", static_cast<double>(nowMs() - startMs_) / 1000.0, k,
                 step);
    std::fflush(stderr);
}

std::string SaturdayDriver::coverage() const {
    std::ostringstream out;
    for (const auto& [name, count] : coverage_) {
        out << (out.tellp() > 0 ? ", " : "") << name << " x" << count;
    }
    return out.str();
}

void SaturdayDriver::startPeakSampler() {
    peakTimer_ = new QTimer();
    peakTimer_->setInterval(50);
    QObject::connect(peakTimer_, &QTimer::timeout, peakTimer_, [this] {
        const TaskPool::Stats s = rt_.pool().stats();
        std::uint64_t tasks = s.parked + s.running;
        for (const std::size_t q : s.queued) {
            tasks += q;
        }
        poolPeak_ = std::max(poolPeak_, tasks);
        dispatcherPeak_ = std::max<std::uint64_t>(dispatcherPeak_, rt_.dispatcher().pending());
    });
    peakTimer_->start();
}

void SaturdayDriver::start() {
    startMs_ = nowMs();
    cycleStartMs_ = startMs_;
    // The originals of the logs that have a second version, for the reload rotation's way back.
    std::error_code ec;
    for (const LogFile& log : logs_) {
        if (!log.variant.empty()) {
            const fs::path dir = log.path.parent_path() / "originals";
            fs::create_directories(dir, ec);
            fs::copy_file(log.path, dir / log.path.filename(), fs::copy_options::overwrite_existing, ec);
            QVERIFY2(!ec, ec.message().c_str());
            if (fs::exists(ldxOf(log.path))) {
                fs::copy_file(ldxOf(log.path), dir / ldxOf(log.path).filename(),
                              fs::copy_options::overwrite_existing, ec);
                QVERIFY2(!ec, ec.message().c_str());
            }
        }
    }
    fs::create_directories(options_.projectDir, ec);
    QVERIFY2(!ec, ec.message().c_str());
    {
        // The project is created once, through the session of a first window.
        openWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    startPeakSampler();
}

void SaturdayDriver::openWindow() {
    const bool fresh = !fs::exists(options_.projectDir / "project.json");
    window_ =
        std::make_unique<widgets::MainWindow>(rt_, uitest::pathsFor(qs(pathToUtf8(options_.configDir))));
    widgets::MainWindow& win = *window_;
    win.setExitPrompt([] { return widgets::MainWindow::ExitAnswer::Discard; });
    widgets::MainWindow::Prompts prompts;
    prompts.file = [](const QString&, const QString&, bool) { return QString(); };
    prompts.folder = [](const QString&) { return QString(); };
    prompts.text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(); };
    prompts.choose = [](const QString&, const QStringList&) { return std::optional<int>(); };
    win.setPrompts(std::move(prompts));
    win.resize(1600, 1000);
    if (fresh) {
        const Result<void> made = win.session().newProject(options_.projectDir, "Saturday");
        QVERIFY2(made.ok(), made.ok() ? "" : made.error().toString().c_str());
    }
    QVERIFY(win.openProject(options_.projectDir));
    QVERIFY(uitest::showAndActivate(win));
    win.session().setAutosaveInterval(std::chrono::milliseconds(250)); // 2 min by default
    SOAK_WAIT(!win.sessionRestore().running() || win.recoveryDialog() != nullptr,
              "the session restore did not finish", static_cast<int>(options_.settleTimeoutMs));
    QVERIFY2(win.recoveryDialog() == nullptr,
             "a recovery prompt: the journal was not cleared by the last save");
}

void SaturdayDriver::restart() {
    widgets::MainWindow& before = w();
    std::vector<const LogFile*> open;
    for (const LogFile& log : logs_) {
        if (sourceOf(log)) {
            open.push_back(&log);
        }
    }
    // The Main to find again, when there is one: after a Close All at the end of the previous cycle (cycles
    // 16, 36, ... are followed by a restart) nothing is open and Main is nil, and the restore, which reopens
    // the project's previous data (docs/milestones/M4.md section 10 item 33), may set a Main again (D-216
    // (h)).
    const std::optional<model::RangeRef> main =
        before.selection().hasMain() ? std::optional<model::RangeRef>(before.selection().snapshot().main)
                                     : std::nullopt;
    settle("restart: before");
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY2(before.commands().invoke(QStringLiteral("file.save_project")), "File > Save Project refused");
    QVERIFY2(before.close(), "the window refused to close");
    window_.reset();
    openWindow();
    if (QTest::currentTestFailed()) {
        return;
    }
    // The session restore reopens the previous data (06 section 18 steps 5-6: the project's recent sources,
    // every log opened in it, so at least the ones open at the exit) and then the selection.
    SOAK_WAIT(!w().sessionRestore().running() && w().loader().pendingOpens() == 0 &&
                  w().loader().pendingBudgetOpens() == 0,
              "restart: the previous data and the selection were not restored",
              static_cast<int>(options_.settleTimeoutMs));
    for (const LogFile* log : open) {
        QVERIFY2(sourceOf(*log).has_value(),
                 qPrintable(QStringLiteral("restart: %1 was not reopened").arg(qs(pathToUtf8(log->path)))));
    }
    if (main) {
        QVERIFY2(w().selection().snapshot().main == main, "restart: Main was not restored");
    }
    restartedThisCycle_ = true;
    note("restart");
}

std::optional<model::SourceId> SaturdayDriver::sourceOf(const LogFile& log) {
    for (const auto& s : w().selection().sources()) {
        if (s->prov.path == log.path) {
            return s->id;
        }
    }
    return std::nullopt;
}

std::vector<model::SourceId> SaturdayDriver::openSources(bool ldOnly) {
    std::vector<model::SourceId> out;
    for (const LogFile& log : logs_) {
        if (ldOnly && log.kind != "ld") {
            continue;
        }
        if (const auto id = sourceOf(log)) {
            out.push_back(*id);
        }
    }
    return out;
}

bool SaturdayDriver::quiet() {
    widgets::MainWindow& win = w();
    return uitest::runtimeQuiet(rt_) && win.loader().pendingOpens() == 0 &&
           win.loader().pendingReloads() == 0 && win.loader().pendingBudgetOpens() == 0 &&
           win.loader().pendingMappings() == 0 && !win.sessionRestore().running() &&
           win.trackMaps().pendingIo() == 0;
}

void SaturdayDriver::settle(const char* step) {
    SOAK_WAIT(quiet(),
              qPrintable(QStringLiteral("%1: the pool, the Dispatcher or the loader did not settle "
                                        "(pending work that never drains)")
                             .arg(QLatin1String(step))),
              static_cast<int>(options_.settleTimeoutMs));
}

bool SaturdayDriver::componentsReady() {
    for (widgets::ComponentWidget* c : w().canvas().components()) {
        if (auto* g = qobject_cast<widgets::TimeDistanceGraph*>(c)) {
            if (!g->staticUpToDate()) {
                return false;
            }
        } else if (auto* l = qobject_cast<widgets::LayeredComponent*>(c)) {
            if (!l->staticUpToDate()) {
                return false;
            }
        } else if (auto* t = qobject_cast<widgets::TrackPosition*>(c)) {
            if (!t->staticUpToDate()) {
                return false;
            }
        } else if (auto* cr = qobject_cast<widgets::ChannelReport*>(c)) {
            if (cr->model().busy() || cr->model().pendingCells() != 0) {
                return false;
            }
        } else if (auto* tr = qobject_cast<widgets::TimeReport*>(c)) {
            if (tr->model().busy() || tr->model().pendingCells() != 0) {
                return false;
            }
        }
    }
    return true;
}

void SaturdayDriver::waitComponentsReady(const char* step) {
    SOAK_WAIT(
        quiet() && componentsReady(),
        qPrintable(QStringLiteral("%1: the worksheet's components were not drawn").arg(QLatin1String(step))),
        static_cast<int>(options_.settleTimeoutMs));
}

void SaturdayDriver::cycle(int k) {
    const std::int64_t begin = nowMs();
    restartedThisCycle_ = false;
    if (k % 4 == 1) {
        progress(k, "restart");
        restart();
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    progress(k, "open");
    openMissing(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    progress(k, "sample");
    sample(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    QUndoStack& stack = w().session().undoStack();
    const int startIndex = stack.index();
    progress(k, "selection");
    selection(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    progress(k, "worksheets");
    worksheets(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    progress(k, "edits");
    edits(k, startIndex);
    if (QTest::currentTestFailed()) {
        return;
    }
    QCOMPARE(w().session().undoStack().index(), startIndex);
    progress(k, "reload");
    reloadOne(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    progress(k, "close");
    closeSome(k);
    if (QTest::currentTestFailed()) {
        return;
    }
    w().messages().clear(); // the engineer clears the Messages dock
    settle("end of cycle");
    lastCycleS_ = static_cast<double>(nowMs() - begin) / 1000.0;
    progress(k, "done");
}

void SaturdayDriver::openMissing(int k) {
    std::vector<const LogFile*> missing;
    for (const LogFile& log : logs_) {
        if (!sourceOf(log)) {
            missing.push_back(&log);
        }
    }
    // One open through the memory-budget gate (AC-M4-02 behaviour): Continue on cycles 0, 3, 6, ..., Cancel
    // and a second open on cycles 1, 4, 7, ... When everything is open (a restart reopened it all), the last
    // log is closed first so that the gate has a file to hold.
    if (missing.empty() && k % 3 != 2) {
        const LogFile& last = logs_.back();
        if (const std::optional<model::SourceId> id = sourceOf(last)) {
            w().loader().close(*id);
            SOAK_WAIT(!sourceOf(last).has_value(), "open: the log closed for the budget gate stayed open",
                      kStepTimeoutMs);
            missing.push_back(&last);
        }
    }
    if (missing.empty()) {
        return;
    }
    if (k % 3 != 2) {
        const LogFile& gated = *missing.back();
        missing.pop_back();
        openWithBudgetGate(gated, k % 3 == 0);
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    widgets::MainWindow& win = w();
    for (const LogFile* log : missing) {
        win.openLog(log->path);
    }
    // Until nothing is in flight; a log that failed to open or waits for the Mapping panel fails here at
    // once.
    SOAK_WAIT(win.loader().pendingOpens() == 0 || win.loader().pendingMappings() > 0,
              "open: the opens did not finish", static_cast<int>(options_.settleTimeoutMs));
    QVERIFY2(win.loader().pendingMappings() == 0, "open: a log waits for the Mapping panel");
    SOAK_WAIT(win.selection().sources().size() == kLogs,
              qPrintable(QStringLiteral("open: %1 of %2 logs open (the newest Messages row: %3)")
                             .arg(win.selection().sources().size())
                             .arg(kLogs)
                             .arg(win.messages().rowCount() > 0
                                      ? win.messages().at(win.messages().rowCount() - 1).text
                                      : QString())),
              kStepTimeoutMs);
    note("open");
}

void SaturdayDriver::openWithBudgetGate(const LogFile& log, bool proceed) {
    settle("budget: before");
    if (QTest::currentTestFailed()) {
        return;
    }
    widgets::MainWindow& win = w();
    store::ChunkCache& cache = rt_.cache();
    const std::size_t budget = cache.budgetBytes();
    const std::size_t sources = win.selection().sources().size();
    // The gate compares the file's own estimate with the budget (D-207): 1 MiB is below every log's estimate.
    // The pool is settled, so nothing is inserted while the budget is low (no over-budget rows from the
    // cache).
    cache.setBudgetBytes(std::size_t{1} << 20);
    win.openLog(log.path);
    SOAK_WAIT(win.loader().pendingBudgetOpens() == 1 || win.loader().pendingOpens() == 0,
              "budget: the open neither waited nor finished", kStepTimeoutMs);
    const bool held = win.loader().pendingBudgetOpens() == 1;
    cache.setBudgetBytes(budget);
    QVERIFY2(held,
             qPrintable(QStringLiteral("budget: %1 was not held at the gate").arg(qs(pathToUtf8(log.path)))));
    int row = -1;
    for (int r = win.messages().rowCount() - 1; r >= 0 && row < 0; --r) {
        if (win.messages().at(r).code == QLatin1String(widgets::BudgetShell::kWarningCode)) {
            row = r;
        }
    }
    QVERIFY2(row >= 0, "budget: no warning row in Messages");
    const QModelIndex index = win.messagesWindow().filterModel().mapFromSource(win.messages().index(row));
    QVERIFY(index.isValid());
    if (proceed) {
        QVERIFY(win.messagesWindow().invokeAction(index)); // Continue Opening
        SOAK_WAIT(win.selection().sources().size() == sources + 1, "budget: Continue did not open the file",
                  static_cast<int>(options_.settleTimeoutMs));
        note("budget-continue");
    } else {
        QVERIFY(win.messagesWindow().invokeAltAction(index)); // Cancel Opening
        QCOMPARE(win.loader().pendingBudgetOpens(), 0);
        QCOMPARE(win.selection().sources().size(), sources);
        win.openLog(log.path); // and again, with the budget back
        SOAK_WAIT(win.selection().sources().size() == sources + 1,
                  "budget: the file did not open after Cancel", static_cast<int>(options_.settleTimeoutMs));
        note("budget-cancel");
    }
}

void SaturdayDriver::sample(int k) {
    settle("sample: open");
    if (QTest::currentTestFailed()) {
        return;
    }
    w().selectWorksheet(0);
    waitComponentsReady("sample");
    if (QTest::currentTestFailed()) {
        return;
    }
    CycleSample s;
    s.cycle = k;
    s.elapsedS = static_cast<double>(nowMs() - startMs_) / 1000.0;
    s.restarted = restartedThisCycle_;
    const ProcessMemory mem = sampleProcessMemory();
    s.residentBytes = mem.residentBytes;
    s.heapBytes = mem.heapBytes;
    const store::ChunkCache::Stats cache = rt_.cache().stats();
    s.cacheResidentBytes = cache.residentBytes;
    s.cacheChunks = cache.chunks;
    s.poolPeak = poolPeak_;
    s.dispatcherPeak = dispatcherPeak_;
    poolPeak_ = 0;
    dispatcherPeak_ = 0;
    s.widgets = static_cast<int>(QApplication::allWidgets().size());
    s.windowObjects = static_cast<int>(w().findChildren<QObject*>().size());
    s.sources = static_cast<int>(w().selection().sources().size());
    s.undoSteps = w().session().undoStack().count();
    s.cycleS = lastCycleS_;
    samples_.push_back(s);
    qInfo("soak cycle %d at %.0f s: resident %.1f MiB, heap %.1f MiB (%s), cache %.1f MiB in %llu chunks, "
          "peak tasks %llu, peak dispatcher %llu, widgets %d, window objects %d, undo steps %d, last cycle "
          "%.1f s",
          k, s.elapsedS, static_cast<double>(s.residentBytes) / 1048576.0,
          static_cast<double>(s.heapBytes) / 1048576.0, mem.heapSource,
          static_cast<double>(s.cacheResidentBytes) / 1048576.0,
          static_cast<unsigned long long>(s.cacheChunks), static_cast<unsigned long long>(s.poolPeak),
          static_cast<unsigned long long>(s.dispatcherPeak), s.widgets, s.windowObjects, s.undoSteps,
          s.cycleS);
}

void SaturdayDriver::selection(int k) {
    widgets::MainWindow& win = w();
    qt::DataSelectionModel& sel = win.selection();
    const std::vector<model::SourceId> lds = openSources(true);
    QVERIFY2(lds.size() >= 3, "selection: fewer than three .ld sources open");
    const auto lapOf = [&sel](const model::SourceId& id, std::size_t pick) -> std::optional<model::RangeRef> {
        const std::vector<model::RangeRef> laps = sel.displayedRanges(id);
        if (laps.size() < 3) {
            return std::nullopt;
        }
        return laps[1 + pick % (laps.size() - 2)]; // a flying lap
    };
    const auto n = static_cast<std::size_t>(k);
    const std::optional<model::RangeRef> main = lapOf(lds[n % lds.size()], n);
    const std::optional<model::RangeRef> zero = lapOf(lds[(n + 1) % lds.size()], n + 1);
    const std::optional<model::RangeRef> extra = lapOf(lds[(n + 2) % lds.size()], n + 2);
    QVERIFY2(main && zero && extra, "selection: a session has fewer than three laps");
    std::optional<model::RangeRef> csvRange;
    for (const LogFile& log : logs_) {
        if (log.kind != "ld" && !csvRange) {
            if (const auto id = sourceOf(log)) {
                const std::vector<model::RangeRef> ranges = sel.displayedRanges(*id);
                if (!ranges.empty()) {
                    csvRange = ranges.front();
                }
            }
        }
    }
    sel.clearOverlays();
    if (k % 2 == 0) {
        // The Data window's keys (F8, ENTER, CTRL+ENTER, ALT+ENTER).
        uitest::press(win, Qt::Key_F8);
        SOAK_WAIT_EXPR(win.docks().focusedDockId() == QStringLiteral("data"), kStepTimeoutMs);
        win.dataWindow().highlight(*main);
        uitest::press(win, Qt::Key_Return);
        win.dataWindow().highlight(*zero);
        uitest::press(win, Qt::Key_Return, Qt::ControlModifier);
        win.dataWindow().highlight(*extra);
        uitest::press(win, Qt::Key_Return, Qt::AltModifier);
        if (csvRange) {
            win.dataWindow().highlight(*csvRange);
            uitest::press(win, Qt::Key_Return, Qt::AltModifier);
        }
        note("selection-keys");
    } else {
        sel.setMain(*main);
        sel.toggleOverlay0(*zero);
        sel.toggleOverlay(*extra);
        if (csvRange) {
            sel.toggleOverlay(*csvRange);
        }
        note("selection-model");
    }
    QVERIFY2(sel.snapshot().main == main, "selection: Main is not the chosen lap");
    QVERIFY2(sel.snapshot().overlay0 == zero, "selection: Overlay 0 is not the chosen lap");
    QVERIFY(!sel.snapshot().overlays.empty());

    // The worksheet keys with a component focused: N, P, Q twice, J twice; F4 twice; CTRL+F.
    widgets::ComponentWidget* focus = nullptr;
    for (widgets::ComponentWidget* c : win.canvas().components()) {
        focus = focus != nullptr ? focus : c;
    }
    QVERIFY2(focus != nullptr, "selection: the worksheet has no component");
    win.canvas().focusComponent(focus);
    focus->setFocus(Qt::OtherFocusReason);
    uitest::press(win, Qt::Key_N);
    uitest::press(win, Qt::Key_P);
    QVERIFY2(sel.snapshot().main == main, "selection: N then P did not come back to Main");
    uitest::press(win, Qt::Key_Q);
    QVERIFY2(sel.snapshot().main == zero, "selection: Q did not swap Main and Overlay 0");
    uitest::press(win, Qt::Key_Q);
    uitest::press(win, Qt::Key_J);
    uitest::press(win, Qt::Key_J);
    uitest::press(win, Qt::Key_F4);
    uitest::press(win, Qt::Key_F4);
    QVERIFY(!sel.snapshot().singleColourMain);
    // A lap-number filter on Main's source, then all laps again.
    qt::FilterSpec filter;
    filter.kind = qt::FilterSpec::Kind::LapNumbers;
    filter.laps = "2-4";
    sel.setFilter(main->src, filter);
    sel.setFilter(main->src, qt::FilterSpec{});
    uitest::press(win, Qt::Key_F, Qt::ControlModifier); // the fastest lap becomes Main
    QVERIFY(sel.hasMain());
    note("selection-keys-worksheet");
}

void SaturdayDriver::exerciseGraph(int k) {
    widgets::MainWindow& win = w();
    widgets::TimeDistanceGraph* g = uitest::graphOf(win);
    if (g == nullptr) {
        return;
    }
    win.canvas().focusComponent(g);
    g->setFocus(Qt::OtherFocusReason);
    const std::vector<std::pair<int, Qt::KeyboardModifiers>> keys{
        {Qt::Key_Up, Qt::NoModifier},         {Qt::Key_Up, Qt::NoModifier},
        {Qt::Key_Right, Qt::ShiftModifier},   {Qt::Key_Right, Qt::ShiftModifier},
        {Qt::Key_Right, Qt::NoModifier},      {Qt::Key_Right, Qt::NoModifier},
        {Qt::Key_Right, Qt::ControlModifier}, {Qt::Key_D, Qt::NoModifier},
        {Qt::Key_Right, Qt::NoModifier},      {Qt::Key_Space, Qt::NoModifier},
        {Qt::Key_X, Qt::NoModifier},          {Qt::Key_Left, Qt::ShiftModifier},
        {Qt::Key_Down, Qt::NoModifier},       {Qt::Key_H, Qt::NoModifier},
        {Qt::Key_F3, Qt::NoModifier},         {Qt::Key_M, Qt::NoModifier},
        {Qt::Key_End, Qt::NoModifier},        {Qt::Key_Home, Qt::NoModifier},
        {Qt::Key_W, Qt::NoModifier},          {Qt::Key_F, Qt::NoModifier},
        {Qt::Key_B, Qt::NoModifier},          {Qt::Key_F3, Qt::NoModifier},
        {Qt::Key_M, Qt::NoModifier},          {Qt::Key_D, Qt::NoModifier},
        {Qt::Key_F2, Qt::NoModifier},
    };
    for (const auto& [key, mods] : keys) {
        uitest::press(win, key, mods);
    }
    // A wheel step in and out at the centre, a click there (the cursor), and the domain toggled twice (F9).
    const QPointF centre(g->width() / 2.0, g->height() / 2.0);
    for (const int delta : {120, -120}) {
        QWheelEvent wheel(centre, g->mapToGlobal(centre), QPoint(), QPoint(0, delta), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(g, &wheel);
    }
    uitest::sendMouse(g, QEvent::MouseButtonPress, centre, Qt::LeftButton, Qt::LeftButton);
    uitest::sendMouse(g, QEvent::MouseButtonRelease, centre, Qt::LeftButton, Qt::NoButton);
    if (k % 2 == 0) {
        uitest::press(win, Qt::Key_F9);
        uitest::press(win, Qt::Key_F9);
        note("graph-f9");
    }
    QVERIFY2(uitest::waitQuiet(*g, static_cast<int>(options_.settleTimeoutMs)),
             "graph: the Static layer did not settle");
    note("graph-keys");
}

void SaturdayDriver::trackMapDock() {
    widgets::MainWindow& win = w();
    const QString dock = QStringLiteral("trackmap");
    const QString command = QStringLiteral("view.track_map_window");
    if (!win.docks().isVisible(dock)) {
        QVERIFY(win.commands().invoke(command));
    }
    SOAK_WAIT(win.docks().isVisible(dock), "track map: the dock did not show", kStepTimeoutMs);
    if (widgets::TrackPosition* map = win.trackMapWindow().trackPosition()) {
        win.trackMapWindow().fit();
        SOAK_WAIT(quiet() && map->staticUpToDate(), "track map: the dock's map was not drawn",
                  static_cast<int>(options_.settleTimeoutMs));
    }
    QVERIFY(win.commands().invoke(command));
    SOAK_WAIT(!win.docks().isVisible(dock), "track map: the dock did not hide", kStepTimeoutMs);
    note("track-map-dock");
}

void SaturdayDriver::worksheets(int k) {
    widgets::MainWindow& win = w();
    const int count = win.worksheetCount();
    QVERIFY2(count > 0, "worksheets: the workbook has none");
    for (int i = 0; i < count; ++i) {
        if (k % 2 == 1 && i > 0) {
            uitest::press(win, Qt::Key_F12); // view.next_worksheet
        } else {
            win.selectWorksheet(i);
        }
        SOAK_WAIT_EXPR(win.activeWorksheetIndex() == i, kStepTimeoutMs);
        waitComponentsReady(qPrintable(QStringLiteral("worksheet %1").arg(win.worksheetTabs().tabText(i))));
        if (QTest::currentTestFailed()) {
            return;
        }
        exerciseGraph(k);
        if (QTest::currentTestFailed()) {
            return;
        }
        for (widgets::ComponentWidget* c : win.canvas().components()) {
            if (auto* cr = qobject_cast<widgets::ChannelReport*>(c)) {
                QVERIFY(!cr->toTsv().isEmpty());
                note("channel-report");
            } else if (auto* tr = qobject_cast<widgets::TimeReport*>(c)) {
                QVERIFY(!tr->toTsv().isEmpty());
                note("time-report");
            }
        }
        waitComponentsReady(qPrintable(QStringLiteral("worksheet %1 after the keys").arg(i)));
        if (QTest::currentTestFailed()) {
            return;
        }
        note("worksheet");
    }
    win.selectWorksheet(0);
    trackMapDock();
    if (QTest::currentTestFailed()) {
        return;
    }
    for (const char* dock : {"view.values_window", "view.channels_window"}) {
        QVERIFY(win.commands().invoke(QString::fromLatin1(dock)));
        QVERIFY(win.commands().invoke(QString::fromLatin1(dock)));
    }
    win.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&win));
    waitComponentsReady("worksheets: back on worksheet 0");
}

void SaturdayDriver::edits(int k, int startIndex) {
    widgets::MainWindow& win = w();
    QUndoStack& stack = win.session().undoStack();
    // The cycle's earlier undo steps (component toggles of the worksheet sweep) are undone with these.
    const std::vector<maths::MathModule> modulesBefore = win.session().mathsModules();
    const std::optional<model::RangeRef> main = win.selection().snapshot().main;
    QVERIFY(main.has_value());
    const model::SourceId mainSource = main->src;
    widgets::TimeDistanceGraph* g = uitest::graphOf(win);
    QVERIFY2(g != nullptr, "edits: worksheet 0 has no time/distance graph");
    const QStringList channelsBefore = g->channelNames();
    std::uintmax_t journalBefore = 0;
    static_cast<void>(journalEntries(options_.projectDir, &journalBefore));

    // A module of two items (global maths), its first channel on the graph: evaluated and drawn.
    std::vector<maths::MathModule> modules = modulesBefore;
    maths::MathModule soak;
    soak.id = "soak";
    soak.items.push_back(item("Soak Gain", "'THROTTLE'[%] * " + std::to_string(1 + k % 5)));
    soak.items.push_back(item("Soak Speed Delta", "'Corr Speed'[km/h] - 'SPEED'[km/h]"));
    modules.push_back(soak);
    stack.push(new qt::EditMathModulesCommand(win.session(), modules, QStringLiteral("Soak maths")));
    g->addChannel(QStringLiteral("Soak Gain"), true);
    // The workbook edit may rebuild the worksheet's widgets: the graph is looked up again at every use.
    const maths::ChannelKey key = maths::ChannelKey::make(mainSource, model::kNormalLayer, "Soak Gain");
    const auto state = [&win, &key] { return win.maths().status(key).state; };
    const auto evaluated = [&state] {
        return state() == maths::MathState::Ok || state() == maths::MathState::Warning;
    };
    const auto drawn = [&win] {
        widgets::TimeDistanceGraph* graph = uitest::graphOf(win);
        return graph != nullptr && graph->staticUpToDate();
    };
    const auto failedText = [&win, &key] {
        return QStringLiteral("edits: the maths item failed: %1").arg(qs(win.maths().status(key).summary));
    };
    SOAK_WAIT((evaluated() && quiet() && drawn()) || state() == maths::MathState::Error,
              "edits: the maths item was not evaluated", static_cast<int>(options_.settleTimeoutMs));
    QVERIFY2(state() != maths::MathState::Error, qPrintable(failedText()));
    QVERIFY(uitest::graphOf(win) != nullptr &&
            uitest::graphOf(win)->channelNames().contains(QStringLiteral("Soak Gain")));
    const maths::Version first = win.maths().status(key).version;
    // The expression changed: the item and what draws it are recalculated.
    modules.back().items.front().expr = "'THROTTLE'[%] * " + std::to_string(7 + k % 5);
    stack.push(new qt::EditMathModulesCommand(win.session(), modules, QStringLiteral("Soak maths edit")));
    SOAK_WAIT((evaluated() && !(win.maths().status(key).version == first) && quiet() && drawn()) ||
                  state() == maths::MathState::Error,
              "edits: the changed item was not recalculated", static_cast<int>(options_.settleTimeoutMs));
    QVERIFY2(state() != maths::MathState::Error, qPrintable(failedText()));
    note("maths-recalc");
    // A lap's trust, off and on again (sidecar edits).
    win.selection().setTrusted(*main, false);
    win.selection().setTrusted(*main, true);
    // The Maths Editor, opened and closed (every third cycle).
    if (k % 3 == 0) {
        QVERIFY(win.commands().invoke(QStringLiteral("tools.maths_editor")));
        SOAK_WAIT_EXPR(win.mathsEditor().isVisible(), kStepTimeoutMs);
        win.mathsEditor().close();
        win.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&win));
        note("maths-editor");
    }
    settle("edits");
    if (QTest::currentTestFailed()) {
        return;
    }
    const int top = stack.index();
    QVERIFY(top > startIndex);
    // The documents differ from disk and from the journal's last entry: autosave appends one (02
    // section 6.8).
    SOAK_WAIT(
        [&] {
            std::uintmax_t now = 0;
            return journalEntries(options_.projectDir, &now) > 0 && now != journalBefore;
        }(),
        "edits: autosave did not write the journal", kStepTimeoutMs);
    note("autosave-journal");

    // Undo to the cycle's start, redo everything, undo again.
    while (stack.index() > startIndex) {
        stack.undo();
    }
    QVERIFY(win.session().mathsModules() == modulesBefore);
    SOAK_WAIT(uitest::graphOf(win) != nullptr &&
                  !uitest::graphOf(win)->channelNames().contains(QStringLiteral("Soak Gain")),
              "edits: undo did not take the channel off the graph", kStepTimeoutMs);
    while (stack.canRedo()) {
        stack.redo();
    }
    QCOMPARE(stack.index(), top);
    SOAK_WAIT(uitest::graphOf(win) != nullptr &&
                  uitest::graphOf(win)->channelNames().contains(QStringLiteral("Soak Gain")),
              "edits: redo did not put the channel back on the graph", kStepTimeoutMs);
    settle("edits: redo");
    if (QTest::currentTestFailed()) {
        return;
    }
    while (stack.index() > startIndex) {
        stack.undo();
    }
    QVERIFY(win.session().mathsModules() == modulesBefore);
    SOAK_WAIT_EXPR(uitest::graphOf(win) != nullptr && uitest::graphOf(win)->channelNames() == channelsBefore,
                   kStepTimeoutMs);
    note("undo-redo");
    // Save: the documents are written and the journal cleared.
    QVERIFY(win.commands().invoke(QStringLiteral("file.save_project")));
    SOAK_WAIT(journalEntries(options_.projectDir) == 0, "edits: save did not clear the journal",
              kStepTimeoutMs);
    settle("edits: saved");
}

void SaturdayDriver::reloadOne(int k) {
    widgets::MainWindow& win = w();
    // Asides of earlier reloads, closed with their sources by now (a removal that fails is tried again
    // later).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs_.front().path.parent_path(), ec)) {
        if (e.path().extension().string().rfind(".aside", 0) == 0) {
            std::error_code ignored;
            fs::remove(e.path(), ignored);
        }
    }
    std::vector<std::size_t> lds;
    for (std::size_t i = 0; i < logs_.size(); ++i) {
        if (logs_[i].kind == "ld") {
            lds.push_back(i);
        }
    }
    QVERIFY(!lds.empty());
    const std::size_t index = lds[(static_cast<std::size_t>(k) * 3) % lds.size()];
    const LogFile& log = logs_[index];
    const std::optional<model::SourceId> before = sourceOf(log);
    QVERIFY2(before.has_value(), "reload: the log is not open");
    const std::size_t open = win.selection().sources().size();
    // The replacement: the other version when the log has one, else the same bytes as a newer file.
    fs::path from = log.path;
    const bool swap = !log.variant.empty();
    if (swap) {
        from =
            variantOnDisk_[index] ? log.path.parent_path() / "originals" / log.path.filename() : log.variant;
    }
    const Result<std::string> ld = readWholeFile(from);
    QVERIFY2(ld.ok(), "reload: cannot read the replacement");
    const Result<std::string> ldx = readWholeFile(ldxOf(from));
    // An atomic writer: the open file renamed aside (its share mode allows it), the new file under its name,
    // a modification time after the old one's.
    fs::path aside = log.path;
    aside += ".aside" + std::to_string(++asideCounter_);
    QVERIFY2(QTest::qWaitFor(
                 [&] {
                     fs::rename(log.path, aside, ec);
                     return !ec;
                 },
                 10'000),
             qPrintable(QStringLiteral("reload: the open log cannot be renamed aside: %1")
                            .arg(QString::fromStdString(ec.message()))));
    QVERIFY(writeBytes(log.path, ld.value()));
    if (ldx.ok()) {
        QVERIFY(writeBytes(ldxOf(log.path), ldx.value()));
    }
    fs::last_write_time(log.path, fs::last_write_time(aside, ec) + std::chrono::seconds(2), ec);
    QVERIFY2(!ec, ec.message().c_str());
    if (swap) {
        variantOnDisk_[index] = !variantOnDisk_[index];
    }
    // Coming back to the application marks it stale; the reload reopens it with its selection.
    Q_EMIT qApp->applicationStateChanged(Qt::ApplicationActive); // the re-stats run on the pool (D-217)
    SOAK_WAIT(win.loader().isStale(*before) && win.loader().fileChecksPending() == 0,
              "reload: the replaced log is not stale", static_cast<int>(options_.settleTimeoutMs));
    QVERIFY(win.commands().isEnabled(QString::fromLatin1(widgets::ReloadShell::kCommand)));
    bool fromMessages = false;
    if (k % 2 == 0) {
        QSortFilterProxyModel& list = win.messagesWindow().filterModel();
        for (int r = list.rowCount() - 1; r >= 0 && !fromMessages; --r) {
            const QModelIndex i = list.index(r, 0);
            if (list.data(i, qt::MessagesModel::CodeRole).toString() == QLatin1String("open.file_changed")) {
                QVERIFY(win.messagesWindow().invokeAction(i));
                fromMessages = true;
            }
        }
        QVERIFY2(fromMessages, "reload: no Messages row with Reload File");
    } else {
        QVERIFY(win.commands().invoke(QString::fromLatin1(widgets::ReloadShell::kCommand),
                                      {QString::fromStdString(before->hex())}));
    }
    SOAK_WAIT(win.loader().pendingReloads() == 0 && win.selection().sources().size() == open &&
                  sourceOf(log).has_value(),
              "reload: the log did not reopen", static_cast<int>(options_.settleTimeoutMs));
    const std::optional<model::SourceId> after = sourceOf(log);
    QVERIFY(after.has_value());
    QVERIFY(!win.loader().isStale(*after));
    QVERIFY(win.selection().hasMain());
    note(std::string(swap ? "reload-new-version" : "reload-same-bytes") +
         (fromMessages ? "-messages" : "-command"));
    settle("reload");
}

void SaturdayDriver::closeSome(int k) {
    widgets::MainWindow& win = w();
    if (k % 5 == 1) {
        QVERIFY(win.commands().invoke(QStringLiteral("file.close_all")));
        SOAK_WAIT(win.selection().sources().empty(), "close: Close All left sources open", kStepTimeoutMs);
        note("close-all");
        return;
    }
    const auto n = static_cast<std::size_t>(k);
    for (const std::size_t pick : {n % kLogs, (n + 3) % kLogs, (n + 7) % kLogs}) {
        const LogFile& log = logs_[pick % logs_.size()];
        const std::optional<model::SourceId> id = sourceOf(log);
        if (!id) {
            continue;
        }
        const std::size_t open = win.selection().sources().size();
        if (pick % 2 == 0) {
            // DELETE on the source's row of the Data window.
            uitest::press(win, Qt::Key_F8);
            SOAK_WAIT_EXPR(win.docks().focusedDockId() == QStringLiteral("data"), kStepTimeoutMs);
            QIdentityProxyModel& view = win.dataWindow().viewModel();
            QModelIndex row;
            for (int r = 0; r < view.rowCount() && !row.isValid(); ++r) {
                const QModelIndex i = view.index(r, qt::DataWindowModel::NameCol);
                if (win.dataWindow().model().sourceAt(view.mapToSource(i)) == id) {
                    row = i;
                }
            }
            QVERIFY2(row.isValid(), "close: the source has no Data-window row");
            win.dataWindow().tree().setCurrentIndex(row);
            win.dataWindow().tree().setFocus(Qt::OtherFocusReason);
            uitest::press(win, Qt::Key_Delete);
            note("close-data-window");
        } else {
            win.loader().close(*id);
            note("close-loader");
        }
        SOAK_WAIT(win.selection().sources().size() == open - 1 && !sourceOf(log),
                  "close: the source did not close", kStepTimeoutMs);
    }
}

void SaturdayDriver::finish() {
    if (window_ == nullptr) {
        return;
    }
    widgets::MainWindow& win = w();
    settle("finish: before");
    if (QTest::currentTestFailed()) {
        return;
    }
    if (!win.selection().sources().empty()) { // Close All is disabled with nothing open
        QVERIFY(win.commands().invoke(QStringLiteral("file.close_all")));
        SOAK_WAIT(win.selection().sources().empty(), "finish: Close All left sources open", kStepTimeoutMs);
    }
    settle("finish: closed");
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(win.commands().invoke(QStringLiteral("file.save_project")));
    QVERIFY(win.close());
    window_.reset();
    SOAK_WAIT(uitest::runtimeQuiet(rt_), "finish: the runtime did not settle after the window closed",
              static_cast<int>(options_.settleTimeoutMs));
    // With every log closed and the window gone, nothing may hold a chunk: the cache gives every byte back.
    const store::ChunkCache::Stats cache = rt_.cache().stats();
    QVERIFY2(cache.pinnedChunks == 0,
             qPrintable(QStringLiteral("finish: %1 chunks still pinned").arg(cache.pinnedChunks)));
    QVERIFY2(cache.residentBytes == 0,
             qPrintable(QStringLiteral("finish: %1 bytes in %2 chunks still resident after every log closed")
                            .arg(cache.residentBytes)
                            .arg(cache.chunks)));
}

} // namespace esses::soak
