// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/plot_bench (AC-M1-02, 07 section 4.2; bench/workloads.md "M1 budget rows"): a time_distance graph
// with 20 traces (four groups of five channels) whose trace area is 3,800 px wide at DPR 1, on a 30 min log
// of 20 channels at 1 kHz with 90 s laps (esses-gen-fixtures --bench), the fastest lap as Main.
//   plot.cursor_move    a cursor move: WorksheetViewModel::setCursor -> the graph's Dynamic layer rebuilt
//                       on the GUI thread -> a synchronous repaint of its damage (no Static build).
//                       Limit 4 ms.
//   plot.zoom_pan_step  a zoom or pan step (x0.5 / x2 about the cursor, pan +-10 %, F2, W): the command ->
//                       the Static build on the TaskPool (TraceBuilder::decimateMany) -> delivered through
//                       the Dispatcher -> rasterised (LayerCache::rasteriseStatic) -> a synchronous full
//                       repaint. Limit 16 ms.
//   plot.animation_frame_10x  (M3, AC-M3-10) one animation frame at 10x: the cursor advanced by 10 x 33 ms
//                       as WorksheetViewModel's animation tick does (CursorSource::Animation, a Cursor-only
//                       change) -> the graph's Dynamic layer -> a synchronous repaint of its damage; no
//                       Static build. Limit 4 ms.
//   plot.variance_f3_no_overlay  (M3, AC-M3-05) what F3 (the variance panel) adds to the Static build
//                       while no overlay is selected: the graph's own inputs built on the pool with
//                       variance.show off and on, alternately, 10 pairs per run; the row is the median of
//                       the per-pair differences (on - off), and a build that makes any VarianceTable fails
//                       the row. Limit 1 ms (timing noise).
//   plot.relayout_resize_20tr  (M4, 07 section 12 "Full relayout / resize <= 50 ms, coalesced to 30 Hz";
//   D-217)
//                       the graph's window resized alternately to 3,000 x 800 and back to its 4,200 x 1,000
//                       (12 steps a run): resize() -> the size change on the 30 Hz coalescer's leading edge
//                       (each step starts once the previous step's throttle has run out) -> the full relayout
//                       and Static build on the pool -> delivered and rasterised -> a synchronous repaint.
//                       Limit 50 ms.
//   values.refresh_250ch  (M4, 07 section 12 "Values window refresh, 250 channels <= 2 ms"; D-217) the Values
//                       window (ValuesWindow over qt::ValuesModel, "All channels" of a 250-channel log, Main
//                       a lap) at 600 x 900 logical px: 60 cursor moves, each WorksheetViewModel::setCursor
//                       -> the model's refresh of the visible rows -> dataChanged -> a synchronous repaint of
//                       the table. Limit 2 ms. Its log (250 channels at 1 kHz, 60 s, 30 s laps) is written
//                       beside the other on first use.
// Each row is the median over --runs runs (default 5, D-042) of the per-run median; the reference machine is
// the developer's (bench/workloads.md). Prints one line per row and writes --json; --gate makes a row over
// its limit exit 1. --smoke: a 2 min file and 1 run (ctest bench.plot_bench.smoke checks the harness, not the
// budget).

#include "bench_support.hpp"

#include <esses/base/json_doc.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/docks/values_window.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTableView>
#include <QTextStream>
#include <QThread>
#include <QWindow>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

using namespace esses;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr double kCursorLimitMs = 4.0;
constexpr double kZoomPanLimitMs = 16.0;
constexpr double kAnimationLimitMs = 4.0;   // AC-M3-10: CPU per frame at 10x
constexpr double kAnimationRate = 10.0;     // the bench rate of AC-M3-10 (a Data menu rate, D-203 (g))
constexpr Micros kAnimationTickUs = 33'333; // WorksheetViewModel's 30 Hz tick
constexpr double kVarianceLimitMs = 1.0;    // AC-M3-05: "F3 costs 0 ms" without an overlay, within noise
constexpr int kVariancePairs = 10;
constexpr double kRelayoutLimitMs = 50.0; // 07 section 12: full relayout / resize
constexpr int kRelayoutSteps = 12;
constexpr double kValuesLimitMs = 2.0; // 07 section 12: Values window refresh, 250 channels
constexpr int kValuesChannels = 250;
constexpr int kValuesMoves = 60;

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
    parser.setApplicationDescription(QStringLiteral(
        "AC-M1-02, AC-M3-05, AC-M3-10: cursor move, zoom/pan step, F3 and animation budgets."));
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
    const QCommandLineOption valuesOnlyOpt(QStringLiteral("values-only"),
                                           QStringLiteral("Only values.refresh_250ch (with its phases)."));
    parser.addOptions({runsOpt, fileOpt, jsonOpt, widthOpt, gateOpt, smokeOpt, stagesOpt, valuesOnlyOpt});
    parser.process(app);
    const bool smoke = parser.isSet(smokeOpt);
    const bool valuesOnly = parser.isSet(valuesOnlyOpt);
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
    std::vector<double> animationRunMedians;
    std::vector<double> zoomRunMedians;
    std::vector<double> varianceRunMedians;
    std::vector<double> relayoutRunMedians;
    std::vector<double> valuesRunMedians;
    int varianceTablesBuilt = 0;
    QJsonArray runRows;
    QJsonObject phases;
    std::size_t zoomSamples = 0;
    int achievedWidth = valuesOnly ? traceWidth : 0;
    if (!valuesOnly) {
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

        // AC-M3-10: frames of a 10x animation over the Main lap, each a Cursor-only change.
        const auto animationSequence = [&](std::vector<double>& into) {
            const int submitted = g.staticBuildsSubmitted();
            constexpr int kFrames = 60;
            const auto step = static_cast<Micros>(kAnimationRate * static_cast<double>(kAnimationTickUs));
            h.view().setCursor(lap / 10, qt::CursorSource::Programmatic);
            for (int i = 0; i < kFrames; ++i) {
                QElapsedTimer timer;
                timer.start();
                h.view().setCursor(h.view().state().cursorT + step, qt::CursorSource::Animation);
                g.repaint(g.lastDynamicDamage());
                into.push_back(msSince(timer));
            }
            if (g.staticBuildsSubmitted() != submitted) {
                err << "plot_bench: an animation frame submitted a Static build\n";
            }
        };

        // AC-M3-05: the graph's current inputs (Main only) built on the pool with the variance panel off and
        // on. The bench thread blocks on the build here, as in --stages.
        const auto withVariance = [](plot::ComponentBuildInputs in, bool show) {
            QJsonObject body =
                QJsonDocument::fromJson(QByteArray::fromStdString(in.spec.body.compact())).object();
            QJsonObject display = body.value(QStringLiteral("display")).toObject();
            QJsonObject variance = display.value(QStringLiteral("variance")).toObject();
            variance.insert(QStringLiteral("show"), show);
            display.insert(QStringLiteral("variance"), variance);
            body.insert(QStringLiteral("display"), display);
            Result<JsonDoc> doc =
                JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
            if (doc.ok()) {
                in.spec.body = std::move(doc).value();
            }
            return in;
        };
        const auto varianceSequence = [&](std::vector<double>& into) {
            const plot::ComponentBuildInputs off = withVariance(g.inputs(), false);
            const plot::ComponentBuildInputs on = withVariance(g.inputs(), true);
            ThreadRole::set(ThreadRole::Normal);
            const auto timed = [&h, &varianceTablesBuilt](const plot::ComponentBuildInputs& in) {
                QElapsedTimer t;
                t.start();
                auto f = h.runtime().pool().submit(
                    Priority::Visible, TaskKey{}, CancelToken{}, [in](const CancelToken& c) {
                        plot::TdStaticInfo info;
                        const Result<plot::ComponentBuild> built =
                            plot::TimeDistanceBuilder::buildStatic(in, plot::TdLocalState{}, c, &info);
                        return built.ok() ? info.varianceTables : -1;
                    });
                const Result<int> taken = std::move(f).take();
                const int tables = taken.ok() ? taken.value() : -1;
                const double ms = msSince(t);
                if (tables != 0) {
                    varianceTablesBuilt += tables < 0 ? 1 : tables; // a failed build counts against the row
                }
                return ms;
            };
            for (int i = 0; i < kVariancePairs; ++i) {
                const double offMs = timed(off);
                const double onMs = timed(on);
                into.push_back(onMs - offMs);
            }
            ThreadRole::set(ThreadRole::NoBlock);
        };

        // 07 section 12's full relayout (D-217): the window resized between the bench size and 3,000 x 800
        // and back (an even count ends at the bench size); a step starts once the previous one's 30 Hz
        // throttle has run out, so its size arrives on the coalescer's leading edge.
        const QSize benchSize = g.size();
        const auto relayoutSequence = [&](std::vector<double>& into) {
            const QSize sizes[2] = {benchSize, QSize(3000, 800)};
            for (int i = 0; i < kRelayoutSteps; ++i) {
                const QSize next = sizes[(i + 1) % 2];
                static_cast<void>(bench::spinUntil(
                    [&] { return !g.sizeThrottleActive() && h.idle() && g.staticUpToDate(); }, kTimeoutMs));
                QElapsedTimer t;
                t.start();
                h.host().resize(next);
                g.resize(next);
                static_cast<void>(bench::spinUntil(
                    [&] { return g.staticUpToDate() && g.layers().staticImage().size() == g.size(); },
                    kTimeoutMs));
                g.repaint();
                into.push_back(msSince(t));
            }
        };

        std::vector<double> warm;
        relayoutSequence(warm);
        zoomSequence(warm); // warm-up: every zoom level's chunks resident
        varianceSequence(warm);
        cursorSequence(warm);
        animationSequence(warm);
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
            std::vector<double> animation;
            animationSequence(animation);
            std::vector<double> variance;
            varianceSequence(variance);
            const bench::Stats vs = bench::statsOf(variance);
            varianceRunMedians.push_back(vs.median);
            std::vector<double> relayout;
            relayoutSequence(relayout);
            const bench::Stats rs = bench::statsOf(relayout);
            relayoutRunMedians.push_back(rs.median);
            out << QStringLiteral("run %1: full relayout / resize median %2 ms (p95 %3, max %4)\n")
                       .arg(r + 1)
                       .arg(rs.median, 0, 'f', 3)
                       .arg(rs.p95, 0, 'f', 3)
                       .arg(rs.max, 0, 'f', 3);
            const bench::Stats zs = bench::statsOf(zoom);
            const bench::Stats cs = bench::statsOf(cursor);
            const bench::Stats as = bench::statsOf(animation);
            cursorRunMedians.push_back(cs.median);
            animationRunMedians.push_back(as.median);
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
            out << QStringLiteral("run %1: animation frame at x%2 median %3 ms (p95 %4, max %5)\n")
                       .arg(r + 1)
                       .arg(kAnimationRate)
                       .arg(as.median, 0, 'f', 3)
                       .arg(as.p95, 0, 'f', 3)
                       .arg(as.max, 0, 'f', 3);
            out << QStringLiteral(
                       "run %1: the variance panel (F3) without an overlay adds median %2 ms to the "
                       "Static build (min %3, max %4)\n")
                       .arg(r + 1)
                       .arg(vs.median, 0, 'f', 3)
                       .arg(vs.min, 0, 'f', 3)
                       .arg(vs.max, 0, 'f', 3);
            out.flush();
            runRows.append(QJsonObject{{QStringLiteral("cursor"), bench::toJson(cs)},
                                       {QStringLiteral("zoomPan"), bench::toJson(zs)},
                                       {QStringLiteral("animation"), bench::toJson(as)},
                                       {QStringLiteral("varianceNoOverlay"), bench::toJson(vs)}});
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

    // 07 section 12's Values window refresh (D-217): a 250-channel log, the Values window with "All
    // channels".
    {
        const QString valuesFile = QDir(QFileInfo(file).absolutePath())
                                       .filePath(smoke ? QStringLiteral("plot-bench-250ch-smoke.ld")
                                                       : QStringLiteral("plot-bench-250ch-1min.ld"));
        if (!bench::ensureBenchFile(
                valuesFile,
                {QStringLiteral("--bench-channels"), QString::number(kValuesChannels),
                 QStringLiteral("--bench-seconds"), smoke ? QStringLiteral("20") : QStringLiteral("60"),
                 QStringLiteral("--bench-lap-seconds"), smoke ? QStringLiteral("8") : QStringLiteral("30")},
                error)) {
            err << "plot_bench: " << error << '\n';
            return 2;
        }
        bench::Harness hv; // no graph: the row is the Values window's alone
        hv.open(std::filesystem::path(valuesFile.toStdWString()));
        if (!bench::spinUntil([&] { return hv.published() != nullptr || !hv.failure().isEmpty(); },
                              kTimeoutMs) ||
            hv.published() == nullptr) {
            err << "plot_bench: cannot open " << valuesFile << ": " << hv.failure() << '\n';
            return 2;
        }
        hv.selection().selectFastest();
        hv.view().zoomToMainRange();
        qt::ValuesModel model(hv.selection(), hv.maths());
        widgets::ValuesWindow values(model);
        values.setAllChannels(true);
        values.setWorksheet(&hv.view());
        values.resize(600, 900);
        values.show();
        if (!bench::spinUntil(
                [&] { return values.windowHandle() != nullptr && values.windowHandle()->isExposed(); },
                kTimeoutMs)) {
            err << "plot_bench: the Values window was not shown\n";
            return 2;
        }
        const Micros lapV = static_cast<Micros>(hv.view().state().zoom.b);
        const auto settled = [&] {
            for (int row = 0; row < model.rowCount(); ++row) {
                if (model.cellText(row, qt::ValuesModel::MainCol).contains(QChar(0x2026))) {
                    return false; // a cell still waits for its chunk
                }
            }
            return hv.idle();
        };
        std::vector<double> modelMs;   // the phases (diagnostics): the cursor move with the model's refresh,
        std::vector<double> paintMs;   // and the table's repaint (the visible rows computed in it),
        std::vector<double> repaintMs; // and the same repaint again, nothing changed (the painting alone)
        const auto valuesSequence = [&](std::vector<double>& into) {
            for (int i = 0; i < kValuesMoves; ++i) {
                const Micros t = lapV / 10 + (lapV * 8 / 10) * i / (kValuesMoves - 1);
                QElapsedTimer timer;
                timer.start();
                hv.view().setCursor(t, qt::CursorSource::Click);
                const double refreshed = msSince(timer);
                values.table().viewport()->repaint();
                const double total = msSince(timer);
                into.push_back(total);
                modelMs.push_back(refreshed);
                paintMs.push_back(total - refreshed);
                QElapsedTimer again;
                again.start();
                values.table().viewport()->repaint();
                repaintMs.push_back(msSince(again));
            }
        };
        std::vector<double> warmValues;
        for (int pass = 0; pass < 3; ++pass) { // every visible row's chunks resident
            valuesSequence(warmValues);
            static_cast<void>(bench::spinUntil(settled, kTimeoutMs));
        }
        if (model.rowCount() < kValuesChannels) {
            err << "plot_bench: the Values window lists " << model.rowCount() << " channels, not "
                << kValuesChannels << '\n';
            return 2;
        }
        modelMs.clear();
        paintMs.clear();
        repaintMs.clear();
        for (int r = 0; r < runs; ++r) {
            std::vector<double> samples;
            valuesSequence(samples);
            const bench::Stats s = bench::statsOf(samples);
            valuesRunMedians.push_back(s.median);
            out << QStringLiteral(
                       "run %1: Values window refresh (%2 channels) median %3 ms (p95 %4, max %5)\n")
                       .arg(r + 1)
                       .arg(model.rowCount())
                       .arg(s.median, 0, 'f', 3)
                       .arg(s.p95, 0, 'f', 3)
                       .arg(s.max, 0, 'f', 3);
            out.flush();
        }
        out << QStringLiteral(
                   "Values window phases (medians): cursor move and the model's refresh %1 ms, repaint %2 ms "
                   "(%3 ms of it painting: the same repaint again; %4 rows on screen)\n")
                   .arg(bench::statsOf(modelMs).median, 0, 'f', 3)
                   .arg(bench::statsOf(paintMs).median, 0, 'f', 3)
                   .arg(bench::statsOf(repaintMs).median, 0, 'f', 3)
                   .arg(values.table().rowAt(values.table().viewport()->height() - 1) + 1);
    }

    QJsonObject rows;
    out << QStringLiteral("plot_bench: %1 traces, trace area %2 px, DPR 1, %3 run(s), %4\n")
               .arg(kTraces)
               .arg(achievedWidth)
               .arg(runs)
               .arg(QDir::toNativeSeparators(file));
    if (!valuesOnly) {
        bench::report(rows, QStringLiteral("plot.cursor_move"), bench::statsOf(cursorRunMedians),
                      kCursorLimitMs);
        bench::report(rows, QStringLiteral("plot.zoom_pan_step"), bench::statsOf(zoomRunMedians),
                      kZoomPanLimitMs);
        bench::report(rows, QStringLiteral("plot.animation_frame_10x"), bench::statsOf(animationRunMedians),
                      kAnimationLimitMs);
        bench::report(rows, QStringLiteral("plot.variance_f3_no_overlay"), bench::statsOf(varianceRunMedians),
                      kVarianceLimitMs);
        bench::report(rows, QStringLiteral("plot.relayout_resize_20tr"), bench::statsOf(relayoutRunMedians),
                      kRelayoutLimitMs);
    }
    bench::report(rows, QStringLiteral("values.refresh_250ch"), bench::statsOf(valuesRunMedians),
                  kValuesLimitMs);
    if (varianceTablesBuilt != 0) { // AC-M3-05: no VarianceTable without an overlay
        err << "plot_bench: " << varianceTablesBuilt << " VarianceTable(s) built without an overlay\n";
        QJsonObject row = rows.value(QStringLiteral("plot.variance_f3_no_overlay")).toObject();
        row.insert(QStringLiteral("pass"), false);
        row.insert(QStringLiteral("varianceTables"), varianceTablesBuilt);
        rows.insert(QStringLiteral("plot.variance_f3_no_overlay"), row);
    }
    const QString jsonPath =
        parser.isSet(jsonOpt) ? parser.value(jsonOpt) : bench::defaultJsonPath(QStringLiteral("plot_bench"));
    if (!jsonPath.isEmpty()) {
        const QJsonObject doc{
            {QStringLiteral("bench"), QStringLiteral("plot_bench")},
            {QStringLiteral("ac"), QStringLiteral("AC-M1-02, AC-M3-05, AC-M3-10, AC-M4-05")},
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
    if (valuesOnly) {
        return parser.isSet(gateOpt) && !rows.value(QStringLiteral("values.refresh_250ch"))
                                             .toObject()
                                             .value(QStringLiteral("pass"))
                                             .toBool()
                   ? 1
                   : 0;
    }
    const bool pass =
        rows.value(QStringLiteral("plot.cursor_move")).toObject().value(QStringLiteral("pass")).toBool() &&
        rows.value(QStringLiteral("plot.zoom_pan_step")).toObject().value(QStringLiteral("pass")).toBool() &&
        rows.value(QStringLiteral("plot.animation_frame_10x"))
            .toObject()
            .value(QStringLiteral("pass"))
            .toBool() &&
        rows.value(QStringLiteral("plot.variance_f3_no_overlay"))
            .toObject()
            .value(QStringLiteral("pass"))
            .toBool() &&
        rows.value(QStringLiteral("plot.relayout_resize_20tr"))
            .toObject()
            .value(QStringLiteral("pass"))
            .toBool() &&
        rows.value(QStringLiteral("values.refresh_250ch")).toObject().value(QStringLiteral("pass")).toBool();
    return parser.isSet(gateOpt) && !pass ? 1 : 0;
}
