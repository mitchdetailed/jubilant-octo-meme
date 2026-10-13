// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/open_bench (AC-M1-03, 07 section 4.2; bench/workloads.md "M1 budget rows"): first paint of a 100 MB
// .ld (8 float channels at 1 kHz, 52 min, no beacons, esses-gen-fixtures --bench) at full extent, warm.
//   open.first_paint_100mb_8ch   SourceLoader::open -> import (mapped file, ranges) -> published -> the Main
//                                range (the whole outing) zoomed -> the graph's Static build of the eight
//                                traces on the TaskPool -> rasterised -> a synchronous repaint.   Limit 500
//                                ms.
// "Warm" (08 section 6.6): the file is in the OS cache (an untimed open first); every run has a fresh
// runtime, so the chunk cache is cold. The graph (1600 x 900, one panel per channel) exists before the open,
// as the application's worksheet does. Median of --runs runs (default 5); --gate exits 1 above the limit;
// --smoke uses a 10 MB file and one run (ctest bench.open_bench.smoke).

#include "bench_support.hpp"

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
#include <vector>

using namespace esses;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr double kFirstPaintLimitMs = 500.0;
constexpr int kChannels = 8;

struct Run {
    double firstPaintMs = 0.0;
    double publishedMs = 0.0;
    double builtMs = 0.0;
    int traces = 0;
    bool fullExtent = false;
};

// One cold-runtime open of `file`; false with `error` on a failure or timeout.
bool openOnce(const QString& file, Run& run, QString& error) {
    bench::Harness h;
    std::vector<std::vector<std::string>> groups = {{"Ground Speed"}, {"Engine Speed"},  {"Throttle Pos"},
                                                    {"Brake Pres"},   {"Steered Angle"}, {"G Force Lat"},
                                                    {"G Force Long"}, {"Damper Pos FL"}};
    widgets::TimeDistanceGraph& g = h.showGraph(bench::graphSpec(groups), QSize(1600, 900));
    static_cast<void>(bench::spinUntil([] { return false; }, 50)); // shown and painted empty
    int traces = 0;
    double builtMs = 0.0;
    QElapsedTimer t;
    g.setBuildObserver([&](const plot::ComponentBuild& b) {
        const int n = bench::drawnTraceColours(b);
        if (n >= kChannels && traces < kChannels) {
            builtMs = static_cast<double>(t.nsecsElapsed()) / 1e6;
        }
        traces = std::max(traces, n);
    });
    t.start();
    h.open(std::filesystem::path(file.toStdWString()));
    double publishedMs = 0.0;
    const bool ok = bench::spinUntil(
        [&] {
            if (publishedMs == 0.0 && h.published() != nullptr) {
                publishedMs = static_cast<double>(t.nsecsElapsed()) / 1e6;
            }
            return !h.failure().isEmpty() || (traces >= kChannels && g.staticUpToDate());
        },
        kTimeoutMs);
    if (!ok || !h.failure().isEmpty()) {
        error = h.failure().isEmpty() ? QStringLiteral("no first paint within %1 s").arg(kTimeoutMs / 1000)
                                      : h.failure();
        return false;
    }
    g.repaint();
    run.firstPaintMs = static_cast<double>(t.nsecsElapsed()) / 1e6;
    run.publishedMs = publishedMs;
    run.builtMs = builtMs;
    run.traces = traces;
    const auto& src = h.published();
    run.fullExtent =
        src != nullptr && h.view().state().zoom.a == 0.0 &&
        std::abs(h.view().state().zoom.b - static_cast<double>(src->layers.front().duration)) < 1.0;
    g.setBuildObserver({});
    return true;
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
    parser.setApplicationDescription(QStringLiteral("AC-M1-03: first paint of a 100 MB .ld, warm."));
    parser.addHelpOption();
    const QCommandLineOption runsOpt(QStringLiteral("runs"),
                                     QStringLiteral("Runs (default 5, or ESSES_BENCH_REPETITIONS)."),
                                     QStringLiteral("N"), bench::defaultRuns());
    const QCommandLineOption fileOpt(QStringLiteral("file"),
                                     QStringLiteral("The bench .ld (generated when absent)."),
                                     QStringLiteral("file"));
    const QCommandLineOption jsonOpt(QStringLiteral("json"), QStringLiteral("Write the results as JSON."),
                                     QStringLiteral("file"));
    const QCommandLineOption gateOpt(QStringLiteral("gate"), QStringLiteral("Exit 1 when over the limit."));
    const QCommandLineOption smokeOpt(QStringLiteral("smoke"), QStringLiteral("A 10 MB file and one run."));
    parser.addOptions({runsOpt, fileOpt, jsonOpt, gateOpt, smokeOpt});
    parser.process(app);
    const bool smoke = parser.isSet(smokeOpt);
    const int runs = smoke ? 1 : std::max(1, parser.value(runsOpt).toInt());
    const QString file = parser.isSet(fileOpt)
                             ? parser.value(fileOpt)
                             : QDir(QCoreApplication::applicationDirPath())
                                   .filePath(smoke ? QStringLiteral("../bench-data/open-bench-smoke.ld")
                                                   : QStringLiteral("../bench-data/open-bench-100mb-8ch.ld"));
    QDir().mkpath(QFileInfo(file).absolutePath());
    QString error;
    if (!bench::ensureBenchFile(file,
                                {QStringLiteral("--bench-channels"), QString::number(kChannels),
                                 QStringLiteral("--bench-mb"),
                                 smoke ? QStringLiteral("10") : QStringLiteral("100")},
                                error)) {
        err << "open_bench: " << error << '\n';
        return 2;
    }

    widgets::registerBuiltinComponents();
    Run warm;
    if (!openOnce(file, warm, error)) { // the OS file cache, untimed
        err << "open_bench: " << error << '\n';
        return 2;
    }
    std::vector<double> firstPaint;
    QJsonArray runRows;
    bool allFull = true;
    for (int r = 0; r < runs; ++r) {
        Run run;
        if (!openOnce(file, run, error)) {
            err << "open_bench: " << error << '\n';
            return 2;
        }
        firstPaint.push_back(run.firstPaintMs);
        allFull = allFull && run.fullExtent && run.traces >= kChannels;
        out << QStringLiteral("run %1: first paint %2 ms (published %3 ms, Static build delivered %4 ms), %5 "
                              "traces, full extent %6\n")
                   .arg(r + 1)
                   .arg(run.firstPaintMs, 0, 'f', 1)
                   .arg(run.publishedMs, 0, 'f', 1)
                   .arg(run.builtMs, 0, 'f', 1)
                   .arg(run.traces)
                   .arg(run.fullExtent ? QStringLiteral("yes") : QStringLiteral("no"));
        out.flush();
        runRows.append(QJsonObject{{QStringLiteral("firstPaintMs"), run.firstPaintMs},
                                   {QStringLiteral("publishedMs"), run.publishedMs},
                                   {QStringLiteral("staticDeliveredMs"), run.builtMs},
                                   {QStringLiteral("traces"), run.traces},
                                   {QStringLiteral("fullExtent"), run.fullExtent}});
    }
    QJsonObject rows;
    out << QStringLiteral("open_bench: %1 (%2 MB), %3 channels, %4 run(s) after a warm-up open\n")
               .arg(QDir::toNativeSeparators(file))
               .arg(static_cast<double>(QFileInfo(file).size()) / 1e6, 0, 'f', 1)
               .arg(kChannels)
               .arg(runs);
    bench::report(rows, QStringLiteral("open.first_paint_100mb_8ch"), bench::statsOf(firstPaint),
                  kFirstPaintLimitMs);
    const QString jsonPath =
        parser.isSet(jsonOpt) ? parser.value(jsonOpt) : bench::defaultJsonPath(QStringLiteral("open_bench"));
    if (!jsonPath.isEmpty()) {
        const QJsonObject doc{{QStringLiteral("bench"), QStringLiteral("open_bench")},
                              {QStringLiteral("ac"), QStringLiteral("AC-M1-03")},
                              {QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture()},
                              {QStringLiteral("os"), QSysInfo::prettyProductName()},
                              {QStringLiteral("threads"), QThread::idealThreadCount()},
                              {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                              {QStringLiteral("bytes"), QFileInfo(file).size()},
                              {QStringLiteral("runs"), runRows},
                              {QStringLiteral("rows"), rows}};
        if (!bench::writeJson(jsonPath, doc, rows)) {
            err << "open_bench: cannot write " << jsonPath << '\n';
            return 2;
        }
    }
    if (!allFull) {
        err << "open_bench: a run did not paint the eight traces at full extent\n";
        return 2;
    }
    const bool pass = rows.value(QStringLiteral("open.first_paint_100mb_8ch"))
                          .toObject()
                          .value(QStringLiteral("pass"))
                          .toBool();
    return parser.isSet(gateOpt) && !pass ? 1 : 0;
}
