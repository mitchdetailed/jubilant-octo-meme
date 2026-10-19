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
//
// AC-M4-02 (07 section 12 "Resident memory, RW-2 + 20 maths channels <= 1 GB"; 01 section 7.2; M4 budget
// package):
//   open.resident_rw2_20maths    RW-2 (bench/workloads.md: 30 min, 8 channels at 1 kHz, 32 at 500 Hz, 60 at
//                                100 Hz, 100 at 20 Hz, the maths readiness set's channel names, 90 s laps
//                                from beacons in the .ldx) written once as a .ld through the motec.ld writer,
//                                then per run in a fresh process: SourceLoader::open (the circuit rules, the
//                                built-in maths and the lap indexes as in esses), the 20 expressions of the
//                                maths readiness set as one global module, every output evaluated, every
//                                chunk of every logged channel and of every output resident, the pool idle.
//                                The row is the process's peak resident set (Windows: the peak working set;
//                                Linux and macOS: the maximum resident set size), median of --runs processes;
//                                it also records the chunk cache's resident bytes (what the budget counts),
//                                the resident set at the end and the private bytes (Windows: the commit
//                                charge, which leaves out the mapped file's pages). Limit 1 GB (2^30 bytes).
// Each run is its own process (the executable started with --resident-run), so a run's peak is its own and
// the heap of a previous run does not hide the next one's growth. --smoke shortens RW-2 to 60 s (one run).
// --first-paint-only and --resident-only run one of the two rows. RW-2 is written by this executable started
// with --write-rw2 (through the motec.ld writer that esses_qtmodel's SourceLoader exposes with
// formats::ImporterRegistry) into <build>/bench-data/open-bench-rw2[-smoke].ld on first use.

#include "bench_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h needs windows.h first (the include sorting would put it before).
// clang-format off
#include <psapi.h>
// clang-format on
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>

#include <sys/resource.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

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

// ---- AC-M4-02: RW-2 + the 20 maths channels resident ------------------------------------------------------

constexpr double kResidentLimitBytes = 1073741824.0; // 1 GB (2^30 bytes, as the Options field)
constexpr Micros kRw2Duration = 1'800'000'000;       // 30 min (bench/workloads.md RW-2)
constexpr Micros kRw2SmokeDuration = 60'000'000;
constexpr Micros kLapUs = 90'000'000;
constexpr int kResidentTimeoutMs = 20 * 60 * 1000;

// What the operating system says about this process's memory (bytes; 0 where the platform has no figure).
struct ProcessMemory {
    std::uint64_t residentBytes = 0;     // working set / resident set size now
    std::uint64_t peakResidentBytes = 0; // its maximum so far
    std::uint64_t privateBytes = 0;      // Windows: the commit charge (PrivateUsage)
    std::uint64_t peakPrivateBytes = 0;  // Windows: PeakPagefileUsage
};

ProcessMemory processMemory() {
    ProcessMemory m;
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof(pmc)) != 0) {
        m.residentBytes = static_cast<std::uint64_t>(pmc.WorkingSetSize);
        m.peakResidentBytes = static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
        m.privateBytes = static_cast<std::uint64_t>(pmc.PrivateUsage);
        m.peakPrivateBytes = static_cast<std::uint64_t>(pmc.PeakPagefileUsage);
    }
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
#if defined(Q_OS_MACOS)
        m.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss); // bytes on macOS
#else
        m.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U; // kilobytes on Linux
#endif
    }
#if defined(Q_OS_MACOS)
    mach_task_basic_info_data_t info{};
    auto count = static_cast<mach_msg_type_number_t>(sizeof(info) / sizeof(natural_t));
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        m.residentBytes = static_cast<std::uint64_t>(info.resident_size);
    }
#else
    // "<size> <resident> ..." in pages, read as every file is (esses::readFileHead).
    if (const Result<std::string> statm = readFileHead("/proc/self/statm", 256, "/proc/self/statm")) {
        unsigned long long size = 0;
        unsigned long long resident = 0;
        if (std::sscanf(statm.value().c_str(), "%llu %llu", &size, &resident) == 2) {
            m.residentBytes =
                static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        }
    }
#endif
#endif
    return m;
}

// The 20 expressions of the maths readiness set (bench/workloads.md; the same module as bench/maths_bench).
maths::MathModule readinessModule() {
    const std::pair<const char*, const char*> items[] = {
        {"Wheel Speed Front", "('Wheel Speed FL' + 'Wheel Speed FR') / 2"},
        {"Brake Bias", "'Brake Pres Front' / max('Brake Pres Front' + 'Brake Pres Rear', 1) * 100"},
        {"Combined G", "hypot('G Force Lat', 'G Force Long')"},
        {"Full Throttle", "choose('Throttle Pos' > 95, 1, 0)"},
        {"Rear Slip",
         "('Wheel Speed RL' + 'Wheel Speed RR') / max('Wheel Speed FL' + 'Wheel Speed FR', 1) - 1"},
        {"Damper Vel FL", "derivative(smooth('Damper Pos FL', 5))"},
        {"Ground Speed Smooth", "smooth('Ground Speed', 0.5)"},
        {"Engine RPM Smooth", "smooth('Engine RPM', 25)"},
        {"Steer Rate", "derivative('Steered Angle')"},
        {"G Lat Filtered", "filter_lp('G Force Lat', 10, 2)"},
        {"G Long Zero Phase", "filter_lp('G Force Long', 5, -2)"},
        {"Damper FR HP", "filter_hp('Damper Pos FR', 1, 2)"},
        {"G Vert Cheby", "filter_cheby_lp('G Force Vert', 20, 4, 0.5)"},
        {"Distance", "integrate('Ground Speed')"},
        {"Lap Max Oil Temp", "running_max('Oil Temp', 'Gear' > 0, range_change(\"Outings:Laps\"))"},
        {"Brake Latch", "flip_flop('Brake Pres Front' > 20, 'Brake Pres Front' < 2)"},
        {"Throttle Shifted", "time_shift('Throttle Pos', 0.1)"},
        {"Lap Max Speed", "stat_max('Ground Speed', 'Gear' > 0, range_change(\"Outings:Laps\"))"},
        {"On Lap", "range_is(\"Outings:Laps\")"},
        {"Lap Number", "range_change(\"Outings:Laps\")"},
    };
    maths::MathModule m;
    m.id = "bench";
    for (const auto& [name, text] : items) {
        maths::MathItem item;
        item.name = name;
        item.expr = text;
        m.items.push_back(std::move(item));
    }
    return m;
}

// --write-rw2: RW-2 in memory (the channel set and signals of bench/maths_bench's makeRw2), with SOL, a
// beacon every 90 s and EOL, written as <file> + .ldx. Returns the exit status.
int writeRw2(const QString& file, Micros duration, QTextStream& err) {
    const ThreadRole::Scope blocking(ThreadRole::Normal); // the writer reads every chunk
    const Result<Paths> paths = Paths::resolve();
    if (!paths) {
        err << "open_bench: Paths::resolve: " << QString::fromStdString(paths.error().toString()) << '\n';
        return 2;
    }
    Result<units::UnitRegistry> loaded = units::UnitRegistry::loadFiles(paths->dataDir() / "units.json");
    if (!loaded) {
        err << "open_bench: units: " << QString::fromStdString(loaded.error().toString()) << '\n';
        return 2;
    }
    const auto units = std::make_shared<const units::UnitRegistry>(std::move(loaded).value());
    store::ChunkCache cache(std::size_t{8} << 30);
    const std::filesystem::path out(file.toStdWString());
    {
        auto src = std::make_shared<model::DataSource>();
        src->id = hash128(std::string_view("open-bench-rw2"));
        src->prov.path = out;
        src->prov.importerId = "motec.ld";
        src->prov.hash = src->id;
        model::DataLayer layer;
        layer.name = std::string(model::kNormalLayer);
        layer.duration = duration;
        store::ColumnOptions options;
        options.cache = &cache;
        int index = 0;
        const auto add = [&](const std::string& name, std::uint32_t hz, std::string_view unit, double base,
                             double amplitude, double periodS) {
            const auto n = static_cast<std::size_t>(duration / 1'000'000 * static_cast<Micros>(hz));
            std::vector<float> v(n);
            const double phase = 0.37 * static_cast<double>(index++);
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(hz);
                v[i] =
                    static_cast<float>(base + amplitude * std::sin(6.283185307179586 * t / periodS + phase) +
                                       0.02 * amplitude * std::sin(37.0 * t + phase));
            }
            model::Channel ch;
            ch.desc.name = name;
            ch.desc.loggedName = name;
            ch.desc.unit = units->find(unit).value_or(units::kNoUnit);
            ch.desc.quantity = units->quantityOf(ch.desc.unit);
            ch.desc.dtype = store::Dtype::F32;
            ch.desc.rate = Rate::fromHz(hz);
            ch.desc.id = model::channelIdLogged(src->id, model::kNormalLayer, name, "motec.ld");
            ch.series = store::UniformSeries{
                Rate::fromHz(hz), 0,
                store::Column(store::InMemoryProvider::fromF32(v, {}, "rw2:" + name), options)};
            layer.addChannel(std::move(ch));
        };
        for (const char* wheel : {"FL", "FR", "RL", "RR"}) {
            add(std::string("Wheel Speed ") + wheel, 1000, "m/s", 40.0, 25.0, 90.0);
        }
        for (const char* corner : {"FL", "FR", "RL", "RR"}) {
            add(std::string("Damper Pos ") + corner, 1000, "mm", 30.0, 10.0, 0.7);
        }
        add("Engine RPM", 500, "", 7000.0, 2000.0, 9.0);
        add("Throttle Pos", 500, "", 60.0, 45.0, 11.0);
        add("Brake Pres Front", 500, "", 20.0, 25.0, 13.0);
        add("Brake Pres Rear", 500, "", 15.0, 18.0, 13.0);
        add("Steered Angle", 500, "", 0.0, 0.4, 7.0);
        add("G Force Lat", 500, "", 0.0, 2.0, 6.0);
        add("G Force Long", 500, "", 0.0, 1.5, 8.0);
        add("G Force Vert", 500, "", 1.0, 0.3, 0.9);
        for (int k = 9; k <= 32; ++k) {
            add("Ch500 " + std::to_string(k), 500, "", 10.0, 5.0, 3.0 + k);
        }
        add("Gear", 100, "", 4.0, 2.0, 15.0);
        add("Ground Speed", 100, "m/s", 40.0, 25.0, 90.0);
        add("Oil Temp", 100, "", 110.0, 5.0, 600.0);
        add("Water Temp", 100, "", 90.0, 4.0, 700.0);
        for (int k = 5; k <= 60; ++k) {
            add("Ch100 " + std::to_string(k), 100, "", 5.0, 2.0, 5.0 + k);
        }
        for (int k = 1; k <= 100; ++k) {
            add("Ch20 " + std::to_string(k), 20, "", 1.0, 0.5, 20.0 + k);
        }
        // The laps come from beacons in the .ldx, as from a logger: the open's circuit rules build them.
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0,
                                                        model::Provenance::Derived));
        // 90 s laps; a shortened RW-2 (--smoke) gets three laps so that "Outings:Laps" exists.
        const Micros lap = std::min(kLapUs, duration / 3);
        for (Micros t = lap; t < duration; t += lap) {
            layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, t, "Beacons", "Beacon",
                                                            {}, 77, 0, model::Provenance::Companion));
        }
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, duration, "", "", "", 0, 0,
                                                        model::Provenance::Derived));
        model::sortMarkers(layer.markers);
        layer.baseMarkers = layer.markers;
        src->layers.push_back(std::move(layer));
        src->details.push_back(model::Detail{"File Name", std::string(pathToUtf8(out.filename())),
                                             model::Provenance::Derived, std::nullopt});
        src->baseDetails = src->details;
        analysis::RuleRunner::circuitDefault().run(*src);

        // The motec.ld writer of the open pipeline's registry (formats::ImporterRegistry, which
        // SourceLoader's header exposes).
        const auto registry = formats::ImporterRegistry::withBuiltins(units.get());
        formats::IWriter* writer = registry->writer("motec.ld");
        if (writer == nullptr) {
            err << "open_bench: no motec.ld writer\n";
            return 2;
        }
        formats::ExportView view;
        view.source = src;
        std::error_code dirError; // a directory that cannot be made fails the write below
        std::filesystem::create_directories(out.parent_path(), dirError);
        const Result<formats::WriteReport> written = writer->write(out, view, {}, {}, {});
        if (!written) {
            err << "open_bench: cannot write " << file << ": "
                << QString::fromStdString(written.error().toString()) << '\n';
            return 2;
        }
    } // the source and its columns go before the cache
    return 0;
}

// One run of open.resident_rw2_20maths in this process (--resident-run): prints one line "RESIDENT <json>".
int residentRun(const QString& file, QTextStream& out, QTextStream& err) {
    QElapsedTimer clock;
    clock.start();
    const ProcessMemory baseline = processMemory();
    std::uint64_t cacheResident = 0;
    std::uint64_t cacheChunks = 0;
    std::uint64_t loggedChunks = 0;
    std::uint64_t mathsChunks = 0;
    std::size_t loggedChannels = 0;
    ProcessMemory after;
    QString error;
    {
        bench::Harness h;
        h.open(std::filesystem::path(file.toStdWString()));
        if (!bench::spinUntil([&] { return h.published() != nullptr || !h.failure().isEmpty(); },
                              kResidentTimeoutMs) ||
            h.published() == nullptr) {
            err << "open_bench: RW-2 did not open: " << h.failure() << '\n';
            return 2;
        }
        const std::shared_ptr<const model::DataSource> src = h.published();
        const maths::MathModule module = readinessModule();
        h.maths().setModules({module});
        std::vector<maths::ChannelKey> keys;
        for (const maths::MathItem& item : module.items) {
            keys.push_back(maths::ChannelKey::make(src->id, model::kNormalLayer, item.name));
        }
        // Every output evaluated (require at Background, as a worksheet's maths prefetch would).
        std::vector<std::optional<store::Column>> outputs(keys.size());
        const bool evaluated = bench::spinUntil(
            [&] {
                bool all = true;
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (outputs[i]) {
                        continue;
                    }
                    if (h.maths().status(keys[i]).state == maths::MathState::Error) {
                        error = QStringLiteral("%1: %2").arg(
                            QString::fromStdString(keys[i].nameCf),
                            QString::fromStdString(h.maths().status(keys[i]).summary));
                        return true;
                    }
                    if (const std::optional<maths::ColumnHandle> handle =
                            h.maths().require(keys[i], Priority::Background);
                        handle.has_value()) {
                        outputs[i] = store::seriesValues(handle->series);
                    } else {
                        all = false;
                    }
                }
                return all;
            },
            kResidentTimeoutMs);
        if (!evaluated || !error.isEmpty()) {
            err << "open_bench: the maths readiness set did not evaluate: "
                << (error.isEmpty() ? QStringLiteral("timeout") : error) << '\n';
            return 2;
        }
        // Every chunk of every logged channel and of every output resident.
        std::vector<store::Column> columns;
        for (const model::Channel& c : src->layers.front().channels) {
            if (c.desc.source == model::ChannelSource::Logged) {
                columns.push_back(store::seriesValues(c.series));
                loggedChunks += static_cast<std::uint64_t>(columns.back().chunkCount());
                ++loggedChannels;
            }
        }
        for (const std::optional<store::Column>& o : outputs) {
            if (o.has_value()) {
                columns.push_back(*o);
                mathsChunks += static_cast<std::uint64_t>(o->chunkCount());
            }
        }
        const bool resident = bench::spinUntil(
            [&] {
                bool all = true;
                for (const store::Column& c : columns) {
                    for (SampleIdx k = 0; k < c.chunkCount(); ++k) {
                        if (!c.isResident(k)) {
                            c.prefetch(k, Priority::Background); // coalesced with a chunk already scheduled
                            all = false;
                        }
                    }
                }
                return all && h.idle() && h.runtime().dispatcher().pending() == 0;
            },
            kResidentTimeoutMs);
        if (!resident) {
            err << "open_bench: RW-2 and its maths did not become resident\n";
            return 2;
        }
        const store::ChunkCache::Stats stats = h.runtime().cache().stats();
        cacheResident = stats.residentBytes;
        cacheChunks = stats.chunks;
        after = processMemory();
    }
    const QJsonObject row{
        {QStringLiteral("cacheResidentBytes"), static_cast<qint64>(cacheResident)},
        {QStringLiteral("cacheChunks"), static_cast<qint64>(cacheChunks)},
        {QStringLiteral("loggedChannels"), static_cast<qint64>(loggedChannels)},
        {QStringLiteral("loggedChunks"), static_cast<qint64>(loggedChunks)},
        {QStringLiteral("mathsOutputs"), 20},
        {QStringLiteral("mathsOutputChunks"), static_cast<qint64>(mathsChunks)},
        {QStringLiteral("residentBytes"), static_cast<qint64>(after.residentBytes)},
        {QStringLiteral("peakResidentBytes"), static_cast<qint64>(after.peakResidentBytes)},
        {QStringLiteral("privateBytes"), static_cast<qint64>(after.privateBytes)},
        {QStringLiteral("peakPrivateBytes"), static_cast<qint64>(after.peakPrivateBytes)},
        {QStringLiteral("baselineResidentBytes"), static_cast<qint64>(baseline.residentBytes)},
        {QStringLiteral("seconds"), static_cast<double>(clock.elapsed()) / 1000.0}};
    out << "RESIDENT " << QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact)) << '\n';
    out.flush();
    return 0;
}

// Starts this executable with `args`; its RESIDENT line's JSON, or nullopt with `error`.
std::optional<QJsonObject> runChild(const QStringList& args, QString& error) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QCoreApplication::applicationFilePath(), args);
    if (!p.waitForFinished(kResidentTimeoutMs) || p.exitStatus() != QProcess::NormalExit ||
        p.exitCode() != 0) {
        error = QStringLiteral("%1 failed (exit %2): %3")
                    .arg(args.join(QLatin1Char(' ')))
                    .arg(p.exitCode())
                    .arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return std::nullopt;
    }
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("RESIDENT "))) {
            return QJsonDocument::fromJson(line.mid(9).trimmed().toUtf8()).object();
        }
    }
    if (args.contains(QLatin1String("--write-rw2"))) {
        return QJsonObject{};
    }
    error = QStringLiteral("%1 printed no result").arg(args.join(QLatin1Char(' ')));
    return std::nullopt;
}

double mib(double bytes) {
    return bytes / 1048576.0;
}

// The resident row: `runs` child processes over RW-2 (written first when absent). False with `error`.
bool residentRow(const QString& dir, bool smoke, int runs, QJsonObject& row, QString& error) {
    const QString rw2 = QDir(dir).filePath(smoke ? QStringLiteral("open-bench-rw2-smoke.ld")
                                                 : QStringLiteral("open-bench-rw2.ld"));
    if (!QFileInfo::exists(rw2)) {
        const Micros duration = smoke ? kRw2SmokeDuration : kRw2Duration;
        if (!runChild({QStringLiteral("--write-rw2"), rw2, QStringLiteral("--rw2-seconds"),
                       QString::number(duration / 1'000'000)},
                      error)) {
            return false;
        }
    }
    QTextStream out(stdout);
    std::vector<double> peak;
    std::vector<double> cache;
    std::vector<double> privatePeak;
    std::vector<double> finalResident;
    QJsonArray runRows;
    for (int r = 0; r < runs; ++r) {
        const std::optional<QJsonObject> one = runChild({QStringLiteral("--resident-run"), rw2}, error);
        if (!one) {
            return false;
        }
        const QJsonObject& o = *one;
        peak.push_back(o.value(QStringLiteral("peakResidentBytes")).toDouble());
        cache.push_back(o.value(QStringLiteral("cacheResidentBytes")).toDouble());
        privatePeak.push_back(o.value(QStringLiteral("peakPrivateBytes")).toDouble());
        finalResident.push_back(o.value(QStringLiteral("residentBytes")).toDouble());
        runRows.append(o);
        out << QStringLiteral(
                   "resident run %1: peak resident %2 MiB (at the end %3 MiB), chunk cache %4 MiB in %5 "
                   "chunks, peak private %6 MiB, %7 s\n")
                   .arg(r + 1)
                   .arg(mib(peak.back()), 0, 'f', 1)
                   .arg(mib(finalResident.back()), 0, 'f', 1)
                   .arg(mib(cache.back()), 0, 'f', 1)
                   .arg(o.value(QStringLiteral("cacheChunks")).toInteger())
                   .arg(mib(privatePeak.back()), 0, 'f', 1)
                   .arg(o.value(QStringLiteral("seconds")).toDouble(), 0, 'f', 1);
        out.flush();
    }
    const bench::Stats p = bench::statsOf(peak);
    const bench::Stats c = bench::statsOf(cache);
    const bench::Stats v = bench::statsOf(privatePeak);
    const bench::Stats e = bench::statsOf(finalResident);
    const bool pass = p.median <= kResidentLimitBytes;
    out << QStringLiteral(
               "%1  median of %2 runs: peak resident %3 MiB (min %4, max %5), chunk cache %6 MiB, peak "
               "private %7 MiB  limit %8 MiB  %9\n")
               .arg(QStringLiteral("open.resident_rw2_20maths"), -34)
               .arg(p.n)
               .arg(mib(p.median), 0, 'f', 1)
               .arg(mib(p.min), 0, 'f', 1)
               .arg(mib(p.max), 0, 'f', 1)
               .arg(mib(c.median), 0, 'f', 1)
               .arg(mib(v.median), 0, 'f', 1)
               .arg(mib(kResidentLimitBytes), 0, 'f', 0)
               .arg(pass ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    out.flush();
    row = QJsonObject{{QStringLiteral("n"), static_cast<qint64>(p.n)},
                      {QStringLiteral("file"), QDir::toNativeSeparators(rw2)},
                      {QStringLiteral("bytes"), QFileInfo(rw2).size()},
                      {QStringLiteral("medianPeakResidentBytes"), p.median},
                      {QStringLiteral("minPeakResidentBytes"), p.min},
                      {QStringLiteral("maxPeakResidentBytes"), p.max},
                      {QStringLiteral("medianCacheResidentBytes"), c.median},
                      {QStringLiteral("medianPeakPrivateBytes"), v.median},
                      {QStringLiteral("medianFinalResidentBytes"), e.median},
                      {QStringLiteral("limitBytes"), kResidentLimitBytes},
                      {QStringLiteral("pass"), pass},
                      {QStringLiteral("runs"), runRows}};
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
    parser.setApplicationDescription(QStringLiteral(
        "AC-M1-03: first paint of a 100 MB .ld, warm. AC-M4-02: RW-2 + 20 maths channels resident."));
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
    const QCommandLineOption firstPaintOnlyOpt(QStringLiteral("first-paint-only"),
                                               QStringLiteral("Only open.first_paint_100mb_8ch."));
    const QCommandLineOption residentOnlyOpt(QStringLiteral("resident-only"),
                                             QStringLiteral("Only open.resident_rw2_20maths."));
    const QCommandLineOption writeRw2Opt(QStringLiteral("write-rw2"),
                                         QStringLiteral("Internal: write RW-2 as <file> and exit."),
                                         QStringLiteral("file"));
    const QCommandLineOption rw2SecondsOpt(
        QStringLiteral("rw2-seconds"),
        QStringLiteral("Internal: the duration of --write-rw2 (default 1800)."), QStringLiteral("s"),
        QStringLiteral("1800"));
    const QCommandLineOption residentRunOpt(QStringLiteral("resident-run"),
                                            QStringLiteral("Internal: one resident run over <file>."),
                                            QStringLiteral("file"));
    parser.addOptions({runsOpt, fileOpt, jsonOpt, gateOpt, smokeOpt, firstPaintOnlyOpt, residentOnlyOpt,
                       writeRw2Opt, rw2SecondsOpt, residentRunOpt});
    parser.process(app);
    if (parser.isSet(writeRw2Opt)) {
        const qint64 seconds = std::clamp<qint64>(parser.value(rw2SecondsOpt).toLongLong(), 1, 6 * 3600);
        return writeRw2(parser.value(writeRw2Opt), static_cast<Micros>(seconds) * 1'000'000, err);
    }
    if (parser.isSet(residentRunOpt)) {
        return residentRun(parser.value(residentRunOpt), out, err);
    }
    const bool smoke = parser.isSet(smokeOpt);
    const int runs = smoke ? 1 : std::max(1, parser.value(runsOpt).toInt());
    const bool firstPaint = !parser.isSet(residentOnlyOpt);
    const bool residentWanted = !parser.isSet(firstPaintOnlyOpt);
    const QString dataDir =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../bench-data"));
    const QString file = parser.isSet(fileOpt)
                             ? parser.value(fileOpt)
                             : QDir(dataDir).filePath(smoke ? QStringLiteral("open-bench-smoke.ld")
                                                            : QStringLiteral("open-bench-100mb-8ch.ld"));
    QDir().mkpath(QFileInfo(file).absolutePath());
    QDir().mkpath(dataDir);
    QString error;
    QJsonObject rows;
    QJsonArray runRows;
    bool allFull = true;
    if (firstPaint) {
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
        std::vector<double> firstPaintMs;
        for (int r = 0; r < runs; ++r) {
            Run run;
            if (!openOnce(file, run, error)) {
                err << "open_bench: " << error << '\n';
                return 2;
            }
            firstPaintMs.push_back(run.firstPaintMs);
            allFull = allFull && run.fullExtent && run.traces >= kChannels;
            out << QStringLiteral(
                       "run %1: first paint %2 ms (published %3 ms, Static build delivered %4 ms), %5 "
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
        out << QStringLiteral("open_bench: %1 (%2 MB), %3 channels, %4 run(s) after a warm-up open\n")
                   .arg(QDir::toNativeSeparators(file))
                   .arg(static_cast<double>(QFileInfo(file).size()) / 1e6, 0, 'f', 1)
                   .arg(kChannels)
                   .arg(runs);
        out.flush();
        bench::report(rows, QStringLiteral("open.first_paint_100mb_8ch"), bench::statsOf(firstPaintMs),
                      kFirstPaintLimitMs);
    }
    // AC-M4-02: a byte row, kept out of the timing rows (and so out of the "benchmarks" array).
    QJsonObject memory;
    if (residentWanted) {
        QJsonObject row;
        if (!residentRow(dataDir, smoke, runs, row, error)) {
            err << "open_bench: " << error << '\n';
            return 2;
        }
        memory.insert(QStringLiteral("open.resident_rw2_20maths"), row);
    }
    const QString jsonPath =
        parser.isSet(jsonOpt) ? parser.value(jsonOpt) : bench::defaultJsonPath(QStringLiteral("open_bench"));
    if (!jsonPath.isEmpty()) {
        const QJsonObject doc{{QStringLiteral("bench"), QStringLiteral("open_bench")},
                              {QStringLiteral("ac"), QStringLiteral("AC-M1-03, AC-M4-02")},
                              {QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture()},
                              {QStringLiteral("os"), QSysInfo::prettyProductName()},
                              {QStringLiteral("threads"), QThread::idealThreadCount()},
                              {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                              {QStringLiteral("bytes"), QFileInfo(file).size()},
                              {QStringLiteral("runs"), runRows},
                              {QStringLiteral("rows"), rows},
                              {QStringLiteral("memory"), memory}};
        if (!bench::writeJson(jsonPath, doc, rows)) {
            err << "open_bench: cannot write " << jsonPath << '\n';
            return 2;
        }
    }
    if (firstPaint && !allFull) {
        err << "open_bench: a run did not paint the eight traces at full extent\n";
        return 2;
    }
    bool pass = !firstPaint || rows.value(QStringLiteral("open.first_paint_100mb_8ch"))
                                   .toObject()
                                   .value(QStringLiteral("pass"))
                                   .toBool();
    if (residentWanted) {
        pass = pass && memory.value(QStringLiteral("open.resident_rw2_20maths"))
                           .toObject()
                           .value(QStringLiteral("pass"))
                           .toBool();
    }
    return parser.isSet(gateOpt) && !pass ? 1 : 0;
}
