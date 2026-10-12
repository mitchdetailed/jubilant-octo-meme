// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/plot_bench (AC-M1-02, 07 section 4.2; bench/workloads.md "M1 budget rows"): a time_distance graph
// with 20 traces (four groups of five channels) whose trace area is 3,800 px wide at DPR 1, on a 30 min log
// of 20 channels at 1 kHz with 90 s laps (esses-gen-fixtures --bench), the fastest lap as Main.
//   plot.cursor_move    a cursor move: WorksheetViewModel::setCursor -> the graph's Dynamic layer rebuilt on
//                       the GUI thread -> a synchronous repaint of its damage (no Static build).   Limit 4
//                       ms.
//   plot.zoom_pan_step  a zoom or pan step (x0.5 / x2 about the cursor, pan +-10 %, F2, W): the command ->
//                       the Static build on the TaskPool (TraceBuilder::decimateMany) -> delivered through
//                       the Dispatcher -> rasterised (LayerCache::rasteriseStatic) -> a synchronous full
//                       repaint.
//                                                                                                Limit 16 ms.
// Each row is the median over --runs runs (default 5, D-042) of the per-run median; the reference machine is
// the developer's (bench/workloads.md). Prints one line per row and writes --json; --gate makes a row over
// its limit exit 1. --smoke: a 2 min file and 1 run (ctest bench.plot_bench.smoke checks the harness, not the
// budget).

#include "bench_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

using namespace esses;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr double kCursorLimitMs = 4.0;
constexpr double kZoomPanLimitMs = 16.0;

double msSince(const QElapsedTimer& t) {
    return static_cast<double>(t.nsecsElapsed()) / 1e6;
}

std::vector<std::string> channelNames(int count) {
    static const std::vector<std::string> kNamed = {"Ground Speed", "Engine Speed",  "Throttle Pos",
                                                    "Brake Pres",   "Steered Angle", "G Force Lat",
                                                    "G Force Long", "Damper Pos FL"};
    std::vector<std::string> out;
    for (int k = 0; k < count; ++k) {
        out.push_back(k < static_cast<int>(kNamed.size()) ? kNamed[static_cast<std::size_t>(k)]
                                                          : "Bench Channel " + std::to_string(k));
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR") && !qEnvironmentVariableIsEmpty("WINDIR")) {
        qputenv("QT_QPA_FONTDIR",
                qgetenv("WINDIR") + "\\Fonts"); // the offscreen platform's FreeType database
    }
#endif
    QApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AC-M1-02: cursor move and zoom/pan step budgets."));
    parser.addHelpOption();
    const QCommandLineOption runsOpt(QStringLiteral("runs"),
                                     QStringLiteral("Runs (default 5, or ESSES_BENCH_REPETITIONS)."),
                                     QStringLiteral("N"), bench::defaultRuns());
    const QCommandLineOption fileOpt(QStringLiteral("file"),
                                     QStringLiteral("The bench .ld (generated when absent)."),
                                     QStringLiteral("file"));
    const QCommandLineOption jsonOpt(QStringLiteral("json"), QStringLiteral("Write the results as JSON."),
                                     QStringLiteral("file"));
    const QCommandLineOption widthOpt(QStringLiteral("trace-width"),
                                      QStringLiteral("Trace area px (default 3800)."), QStringLiteral("px"),
                                      QStringLiteral("3800"));
    const QCommandLineOption gateOpt(QStringLiteral("gate"),
                                     QStringLiteral("Exit 1 when a row is over its limit."));
    const QCommandLineOption smokeOpt(QStringLiteral("smoke"), QStringLiteral("A 2 min file and one run."));
    const QCommandLineOption stagesOpt(
        QStringLiteral("stages"), QStringLiteral("Also time the stages of a Static rebuild (04 section 6.4: "
                                                 "build = decimate + layout on the pool, raster_static)."));
    parser.addOptions({runsOpt, fileOpt, jsonOpt, widthOpt, gateOpt, smokeOpt, stagesOpt});
    parser.process(app);
    const bool smoke = parser.isSet(smokeOpt);
    const int runs = smoke ? 1 : std::max(1, parser.value(runsOpt).toInt());
    const int traceWidth = std::max(200, parser.value(widthOpt).toInt());
    constexpr int kTraces = 20;
    const QString file =
        parser.isSet(fileOpt)
            ? parser.value(fileOpt)
            : QDir(QCoreApplication::applicationDirPath())
                  .filePath(smoke ? QStringLiteral("../bench-data/plot-bench-smoke.ld")
                                  : QStringLiteral("../bench-data/plot-bench-20ch-30min.ld"));
    QDir().mkpath(QFileInfo(file).absolutePath());
    QString error;
    const QStringList genArgs{
        QStringLiteral("--bench-channels"),    QString::number(kTraces),
        QStringLiteral("--bench-seconds"),     smoke ? QStringLiteral("120") : QStringLiteral("1800"),
        QStringLiteral("--bench-lap-seconds"), smoke ? QStringLiteral("30") : QStringLiteral("90")};
    if (!bench::ensureBenchFile(file, genArgs, error)) {
        err << "plot_bench: " << error << '\n';
        return 2;
    }

    widgets::registerBuiltinComponents();
    std::vector<double> cursorRunMedians;
    std::vector<double> zoomRunMedians;
    QJsonArray runRows;
    QJsonObject phases;
    std::size_t zoomSamples = 0;
    int achievedWidth = 0;
    {
        bench::Harness h;
        const std::vector<std::string> names = channelNames(kTraces);
        std::vector<std::vector<std::string>> groups(4);
        for (int k = 0; k < kTraces; ++k) {
            groups[static_cast<std::size_t>(k / 5)].push_back(names[static_cast<std::size_t>(k)]);
        }
        widgets::TimeDistanceGraph& g = h.showGraph(bench::graphSpec(groups), QSize(traceWidth + 400, 1000));
        h.open(std::filesystem::path(file.toStdWString()));
        if (!bench::spinUntil([&] { return h.published() != nullptr || !h.failure().isEmpty(); },
                              kTimeoutMs) ||
            h.published() == nullptr) {
            err << "plot_bench: cannot open " << file << ": " << h.failure() << '\n';
            return 2;
        }
        h.selection().selectFastest();
        h.view().zoomToMainRange();
        // Steady state: maths, distance indexes, summaries and the first build done; then the trace area is
        // made exactly `traceWidth` device px wide.
        if (!bench::spinUntil([&] { return h.idle() && g.staticUpToDate() && g.layout().traceWidthPx > 0; },
                              kTimeoutMs)) {
            err << "plot_bench: the first build did not settle\n";
            return 2;
        }
        // The key column width follows the component width, so converge in a few resizes.
        for (int attempt = 0; attempt < 6 && g.layout().traceWidthPx != traceWidth; ++attempt) {
            const int width = g.width() + (traceWidth - g.layout().traceWidthPx);
            h.host().resize(width, 1000);
            g.resize(width, 1000);
            static_cast<void>(bench::spinUntil([] { return false; }, 100)); // the 30 Hz size coalescing
            if (!bench::spinUntil([&] { return h.idle() && g.staticUpToDate(); }, kTimeoutMs)) {
                err << "plot_bench: the resized build did not settle\n";
                return 2;
            }
        }
        achievedWidth = g.layout().traceWidthPx;
        const Micros lap = static_cast<Micros>(h.view().state().zoom.b);

        // Phases of a step (diagnostics): the command until the Static build is delivered on the GUI thread,
        // the rasterisation of the delivered list, the synchronous repaint.
        std::vector<double> phaseDelivered;
        std::vector<double> phaseRaster;
        std::vector<double> phasePaint;
        const auto zoomStep = [&](const std::function<void()>& action, std::vector<double>& into) {
            const std::uint64_t before = h.view().state().version;
            QElapsedTimer t;
            double deliveredMs = 0.0;
            g.setBuildObserver([&](const plot::ComponentBuild&) { deliveredMs = msSince(t); });
            t.start();
            action();
            const std::uint64_t target = h.view().state().version;
            if (target == before) {
                g.setBuildObserver({});
                return; // clamped: nothing to draw
            }
            static_cast<void>(bench::spinUntil(
                [&] { return g.staticUpToDate() && g.builtViewVersion() >= target; }, kTimeoutMs));
            const double rasterisedMs = msSince(t);
            g.repaint();
            const double totalMs = msSince(t);
            g.setBuildObserver({});
            into.push_back(totalMs);
            phaseDelivered.push_back(deliveredMs);
            phaseRaster.push_back(rasterisedMs - deliveredMs);
            phasePaint.push_back(totalMs - rasterisedMs);
        };
        const auto zoomSequence = [&](std::vector<double>& into) {
            qt::WorksheetViewModel& v = h.view();
            v.setCursor(lap / 2, qt::CursorSource::Programmatic);
            const std::vector<std::function<void()>> steps = {
                [&] { v.zoomIn(); },     [&] { v.zoomIn(); },    [&] { v.zoomIn(); },
                [&] { v.panBy(0.10); },  [&] { v.panBy(0.10); }, [&] { v.panBy(-0.10); },
                [&] { v.panBy(-0.10); }, [&] { v.zoomOut(); },   [&] { v.zoomOut(); },
                [&] { v.zoomOut(); },    [&] { v.zoomFull(); },  [&] { v.zoomToMainRange(); }};
            for (const auto& step : steps) {
                zoomStep(step, into);
            }
        };
        const auto cursorSequence = [&](std::vector<double>& into) {
            const int submitted = g.staticBuildsSubmitted();
            constexpr int kMoves = 60;
            for (int i = 0; i < kMoves; ++i) {
                const Micros t = lap / 10 + (lap * 8 / 10) * i / (kMoves - 1);
                QElapsedTimer timer;
                timer.start();
                h.view().setCursor(t, qt::CursorSource::Click);
                g.repaint(g.lastDynamicDamage());
                into.push_back(msSince(timer));
            }
            if (g.staticBuildsSubmitted() != submitted) {
                err << "plot_bench: a cursor move submitted a Static build\n";
            }
        };

        std::vector<double> warm;
        zoomSequence(warm); // warm-up: every zoom level's chunks resident
        cursorSequence(warm);
        if (parser.isSet(stagesOpt)) {
            // The current view's Static build (the graph's own inputs) on the pool, waited for here (the
            // bench thread may block in this diagnostic), then rasterised into a fresh LayerCache: all
            // commands, the trace polylines only, and everything else.
            const plot::ComponentBuildInputs in = g.inputs();
            const auto fonts = std::make_shared<const qt::SchemeFonts>(*in.scheme, 1);
            ThreadRole::set(ThreadRole::Normal);
            std::vector<double> buildMs;
            std::vector<double> rasterMs;
            std::vector<double> tracesMs;
            std::vector<double> restMs;
            std::vector<double> stripsMs;
            for (int i = 0; i < 10; ++i) {
                QElapsedTimer t;
                t.start();
                auto f = h.runtime().pool().submit(
                    Priority::Visible, TaskKey{}, CancelToken{},
                    [in](const CancelToken& c) { return plot::TimeDistanceBuilder::buildStatic(in, c); });
                const Result<plot::ComponentBuild> built = std::move(f).take();
                buildMs.push_back(msSince(t));
                if (!built) {
                    err << "plot_bench: build failed: " << QString::fromStdString(built.error().toString())
                        << '\n';
                    break;
                }
                const plot::DrawList& list = built->list;
                plot::DrawList traces = list;
                plot::DrawList rest = list;
                traces.cmds.clear();
                rest.cmds.clear();
                for (const plot::DrawCmd& c : list.cmds) {
                    const bool trace = c.kind == plot::CmdKind::Polyline && c.vertexCount > 8;
                    if (trace || c.kind == plot::CmdKind::Clear || c.kind == plot::CmdKind::Scissor) {
                        traces.cmds.push_back(c);
                    }
                    if (!trace) {
                        rest.cmds.push_back(c);
                    }
                }
                qt::LayerCache lc(fonts);
                lc.resize(g.size(), 1.0);
                lc.rasteriseStatic(list); // allocation outside the timing
                t.restart();
                lc.rasteriseStatic(list);
                rasterMs.push_back(msSince(t));
                t.restart();
                lc.rasteriseStatic(traces);
                tracesMs.push_back(msSince(t));
                t.restart();
                lc.rasteriseStatic(rest);
                restMs.push_back(msSince(t));
                t.restart();
                auto stripped = h.runtime().pool().submit(
                    Priority::Visible, TaskKey{}, CancelToken{}, [&list, &fonts](const CancelToken&) {
                        return qt::RasterBackend::rasteriseStatic(list, *fonts);
                    });
                const Result<QImage> image = std::move(stripped).take();
                stripsMs.push_back(msSince(t));
                if (!image || qt::compareImages(*image, lc.staticImage(), 0).differingPixels != 0) {
                    lc.rasteriseStatic(list);
                    if (!image || qt::compareImages(*image, lc.staticImage(), 0).differingPixels != 0) {
                        err << "plot_bench: the strip raster differs from the single pass\n";
                    }
                }
            }
            ThreadRole::set(ThreadRole::NoBlock);
            out << QStringLiteral(
                       "stages (median of 10, current view): build %1 ms (decimate + layout on the pool), "
                       "raster_static %2 ms (traces only %3 ms, the rest %4 ms)\n")
                       .arg(bench::statsOf(buildMs).median, 0, 'f', 3)
                       .arg(bench::statsOf(rasterMs).median, 0, 'f', 3)
                       .arg(bench::statsOf(tracesMs).median, 0, 'f', 3)
                       .arg(bench::statsOf(restMs).median, 0, 'f', 3)
                << QStringLiteral("stages: raster_static in parallel strips on a worker %1 ms (the widget "
                                  "path, a new buffer each time)")
                       .arg(bench::statsOf(stripsMs).median, 0, 'f', 3)
                << '\n';
            out.flush();
        }
        for (int r = 0; r < runs; ++r) {
            std::vector<double> zoom;
            std::vector<double> cursor;
            zoomSequence(zoom);
            static_cast<void>(bench::spinUntil([&] { return h.idle() && g.staticUpToDate(); }, kTimeoutMs));
            cursorSequence(cursor);
            const bench::Stats zs = bench::statsOf(zoom);
            const bench::Stats cs = bench::statsOf(cursor);
            cursorRunMedians.push_back(cs.median);
            zoomRunMedians.push_back(zs.median);
            zoomSamples += zoom.size();
            out << QStringLiteral("run %1: cursor median %2 ms (p95 %3, max %4); zoom/pan median %5 ms (p95 "
                                  "%6, max %7)\n")
                       .arg(r + 1)
                       .arg(cs.median, 0, 'f', 3)
                       .arg(cs.p95, 0, 'f', 3)
                       .arg(cs.max, 0, 'f', 3)
                       .arg(zs.median, 0, 'f', 3)
                       .arg(zs.p95, 0, 'f', 3)
                       .arg(zs.max, 0, 'f', 3);
            out.flush();
            runRows.append(QJsonObject{{QStringLiteral("cursor"), bench::toJson(cs)},
                                       {QStringLiteral("zoomPan"), bench::toJson(zs)}});
        }
        // The warm-up's phases are left out (the first entries of the phase vectors).
        const auto measured = [](std::vector<double> v, std::size_t skip) {
            v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min(skip, v.size())));
            return bench::statsOf(std::move(v));
        };
        const std::size_t warmSteps = phaseDelivered.size() - zoomSamples;
        const bench::Stats pd = measured(phaseDelivered, warmSteps);
        const bench::Stats pr = measured(phaseRaster, warmSteps);
        const bench::Stats pp = measured(phasePaint, warmSteps);
        out << QStringLiteral(
                   "zoom/pan phases (medians): command -> Static build delivered %1 ms, rasterise %2 ms, "
                   "repaint %3 ms\n")
                   .arg(pd.median, 0, 'f', 3)
                   .arg(pr.median, 0, 'f', 3)
                   .arg(pp.median, 0, 'f', 3);
        phases = QJsonObject{{QStringLiteral("delivered"), bench::toJson(pd)},
                             {QStringLiteral("rasterise"), bench::toJson(pr)},
                             {QStringLiteral("repaint"), bench::toJson(pp)}};
    }

    QJsonObject rows;
    out << QStringLiteral("plot_bench: %1 traces, trace area %2 px, DPR 1, %3 run(s), %4\n")
               .arg(kTraces)
               .arg(achievedWidth)
               .arg(runs)
               .arg(QDir::toNativeSeparators(file));
    bench::report(rows, QStringLiteral("plot.cursor_move"), bench::statsOf(cursorRunMedians), kCursorLimitMs);
    bench::report(rows, QStringLiteral("plot.zoom_pan_step"), bench::statsOf(zoomRunMedians),
                  kZoomPanLimitMs);
    const QString jsonPath =
        parser.isSet(jsonOpt) ? parser.value(jsonOpt) : bench::defaultJsonPath(QStringLiteral("plot_bench"));
    if (!jsonPath.isEmpty()) {
        const QJsonObject doc{{QStringLiteral("bench"), QStringLiteral("plot_bench")},
                              {QStringLiteral("ac"), QStringLiteral("AC-M1-02")},
                              {QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture()},
                              {QStringLiteral("os"), QSysInfo::prettyProductName()},
                              {QStringLiteral("threads"), QThread::idealThreadCount()},
                              {QStringLiteral("traces"), kTraces},
                              {QStringLiteral("traceWidthPx"), achievedWidth},
                              {QStringLiteral("dpr"), 1},
                              {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                              {QStringLiteral("runs"), runRows},
                              {QStringLiteral("zoomPanPhases"), phases},
                              {QStringLiteral("rows"), rows}};
        if (!bench::writeJson(jsonPath, doc, rows)) {
            err << "plot_bench: cannot write " << jsonPath << '\n';
            return 2;
        }
    }
    if (achievedWidth != traceWidth) {
        err << "plot_bench: the trace area is " << achievedWidth << " px, not " << traceWidth << '\n';
        return smoke ? 0 : 2;
    }
    const bool pass =
        rows.value(QStringLiteral("plot.cursor_move")).toObject().value(QStringLiteral("pass")).toBool() &&
        rows.value(QStringLiteral("plot.zoom_pan_step")).toObject().value(QStringLiteral("pass")).toBool();
    return parser.isSet(gateOpt) && !pass ? 1 : 0;
}
