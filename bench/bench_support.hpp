// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared harness of the M1 budget benchmarks (07 section 4.2 AC-M1-02 and AC-M1-03, bench/workloads.md):
// the application's data path without the shell's docks (AppRuntime with the GUI thread marked NoBlock as in
// esses, SourceLoader with the circuit rules, the built-in maths, DistanceService, DataSelectionModel,
// WorkspaceViewModel, one WorksheetViewModel, RenderService) and one time_distance graph in a top-level
// window on the offscreen platform at DPR 1; the bench file written by esses-gen-fixtures --bench next to the
// executable; medians and JSON output. Plain QElapsedTimer harness: Google Benchmark is not vendored (D-147).
#pragma once

#include <esses/plotcore/component_build.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace esses::bench {

struct Stats {
    std::size_t n = 0;
    double min = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};
Stats statsOf(std::vector<double> samples);
QJsonObject toJson(const Stats& s);

// Writes `file` with esses-gen-fixtures (next to this executable) and `genArgs` unless it exists; false with
// `error` on failure.
bool ensureBenchFile(const QString& file, const QStringList& genArgs, QString& error);

// Runs the event loop until `done` (busy, so a wake-up costs no timer granularity); false after `timeoutMs`.
bool spinUntil(const std::function<bool()>& done, int timeoutMs);

// A time_distance spec: one group per entry (channel names), Channel scale, lines of width 1, key on the left
// with values.
project::ComponentSpec graphSpec(const std::vector<std::vector<std::string>>& groups);

// Distinct stroke colours of the non-trivial polylines of a Static build (one per drawn trace and slot).
int drawnTraceColours(const plot::ComponentBuild& build);

class Harness {
public:
    explicit Harness(unsigned workers = 0);
    ~Harness();
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // Creates the graph in a top-level window of `size` logical px (shown; DPR 1 offscreen).
    widgets::TimeDistanceGraph& showGraph(const project::ComponentSpec& spec, QSize size);
    // Asynchronous open; published() becomes non-null when the source is in the selection.
    void open(const std::filesystem::path& file);
    const std::shared_ptr<const model::DataSource>& published() const noexcept { return source_; }
    const QString& failure() const noexcept { return failure_; }
    bool idle() const; // the pool has nothing queued or running

    qt::AppRuntime& runtime() noexcept { return *rt_; }
    qt::DataSelectionModel& selection() noexcept { return *selection_; }
    qt::WorksheetViewModel& view() noexcept { return *view_; }
    qt::DistanceService& distance() noexcept { return *distance_; }
    qt::MathsService& maths() noexcept { return *maths_; }
    widgets::TimeDistanceGraph* graph() const noexcept { return graph_; }
    QWidget& host() noexcept { return *host_; }

private:
    std::unique_ptr<qt::AppRuntime> rt_;
    std::shared_ptr<const units::UnitRegistry> units_;
    std::unique_ptr<qt::MathsService> maths_;
    std::unique_ptr<qt::DistanceService> distance_;
    std::unique_ptr<qt::DataSelectionModel> selection_;
    std::unique_ptr<qt::WorkspaceViewModel> workspace_;
    std::unique_ptr<qt::RenderService> render_;
    std::unique_ptr<qt::CommandRegistry> commands_;
    std::unique_ptr<qt::SourceLoader> loader_;
    project::WorksheetDef sheet_;
    std::unique_ptr<qt::WorksheetViewModel> view_;
    std::unique_ptr<widgets::WorksheetContext> context_;
    std::unique_ptr<QWidget> host_;
    widgets::TimeDistanceGraph* graph_ = nullptr;
    std::shared_ptr<const model::DataSource> source_;
    QString failure_;
};

// Prints one result row and appends it to `rows` (bench/workloads.md row ids).
void report(QJsonObject& rows, const QString& id, const Stats& runMedians, double limitMs);

// The bench test presets (CMakePresets.json test-bench, .github/workflows/bench.yml): --runs defaults to
// ESSES_BENCH_REPETITIONS when set (else 5) and --json to <ESSES_BENCH_OUT>/<name>.json when that is set.
QString defaultRuns();
QString defaultJsonPath(const QString& name);
// The rows as a Google-Benchmark-style "benchmarks" array (aggregate medians, time_unit ms) for the
// comparison tooling of 08 section 6.9.
QJsonArray benchmarkEntries(const QJsonObject& rows);
// Writes `doc` (plus "benchmarks") to `path`, creating its directory; false on failure.
bool writeJson(const QString& path, QJsonObject doc, const QJsonObject& rows);

} // namespace esses::bench
