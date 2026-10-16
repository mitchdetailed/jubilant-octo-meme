// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/formats_bench (AC-M2-13, 07 section 12 "CSV 300 MB (100 columns) -> first paint <= 10 s", 05 section
// 1.3 rule 2 "probe <= 2 s on the 300 MB CSV"): the text engine on a 300 MB, 100-channel CSV, warm (the file
// is in the OS cache after an untimed first run; every run has a fresh chunk cache and task pool).
//   formats.csv_probe_300mb_100col             IImporter::probe of the legacy-layout file (pass 1)  <= 2,000
//   ms formats.csv_first_data_300mb_100col        ImporterRegistry::open (units to base, whole-outing
//                                              ranges) then every chunk of 8 channels resident: all
//                                              their chunks prefetched at Visible on the pool (as a
//                                              graph's first build requests them), then waited for  <= 10,000
//                                              ms
//   formats.csv_generic_first_data_300mb_100col the same through the generic profile on a plain table
//                                              (the full pass-1 scan of sparse "timestamped")      <= 10,000
//                                              ms
// "First data" is the formats share of first paint: the application's first paint adds the graph's build and
// raster of the eight traces, which open_bench measures at about 90 ms for 8 x 3 M samples
// (bench/workloads.md). The files are written on first use next to the executable (<exe>/../bench-data/),
// deterministically. Qt-free: std::chrono, nlohmann-json for --json (a Google-Benchmark-style "benchmarks"
// array plus the rows, as the Qt benches write). --smoke uses 2 MB files and one run (ctest
// bench.formats_bench.smoke).

#include <esses/base/env.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/registry.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace esses;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kChannels = 100;
constexpr int kDrawn = 8;
constexpr double kProbeLimitMs = 2000.0;
constexpr double kFirstDataLimitMs = 10000.0;

struct Stats {
    std::size_t n = 0;
    double min = 0, median = 0, p95 = 0, max = 0;
};

Stats statsOf(std::vector<double> v) {
    Stats s;
    s.n = v.size();
    if (v.empty()) {
        return s;
    }
    std::sort(v.begin(), v.end());
    const auto at = [&v](double q) {
        const double pos = q * static_cast<double>(v.size() - 1);
        const auto lo = static_cast<std::size_t>(std::floor(pos));
        const auto hi = static_cast<std::size_t>(std::ceil(pos));
        return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
    };
    s.min = v.front();
    s.median = at(0.5);
    s.p95 = at(0.95);
    s.max = v.back();
    return s;
}

double ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

void appendFixed(std::string& out, double v, int decimals) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    out.append(buf, r.ptr);
}

// A deterministic CSV of about `megabytes` MB: Time plus 100 channels at 100 Hz, smooth signals with two
// decimals. legacy: the quoted key/value preamble of the motec.csv profile; else a plain table with a units
// row.
bool writeBenchFile(const fs::path& file, std::uint64_t megabytes, bool legacy) {
    fs::create_directories(file.parent_path());
    const fs::path tmp = file.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::string eol = legacy ? "\r\n" : "\n";
    const auto q = [&](std::string_view s) { return legacy ? "\"" + std::string(s) + "\"" : std::string(s); };
    std::string head;
    if (legacy) {
        const std::string_view lines[] = {
            R"("Venue","Bench Park",,,"Worksheet","All")",
            R"("Vehicle","BENCH",,,"Vehicle Desc","")",
            R"("Driver","Bench",,,"Engine ID","")",
            R"("Device","BENCH")",
            R"("Comment","",,,"Session","")",
            R"("Log Date","01/01/2026",,,"Origin Time","0.000","s")",
            R"("Log Time","00:00:00",,,"Start Time","0.000","s")",
            R"("Sample Rate","100.000","Hz",,"End Time","0","s")",
            R"("Duration","0","s",,"Start Distance","0","m")",
            R"("Range","",,,"End Distance","0","m")",
        };
        head += q("Format") + "," + q(formats::kLegacyCsvFormatName) + R"(,,,"Workbook","Bench")" + eol;
        for (const std::string_view l : lines) {
            head.append(l).append(eol);
        }
        head += eol + eol;
    }
    std::string names = q("Time");
    std::string units = q("s");
    static constexpr std::string_view kUnits[] = {"km/h", "rpm", "bar", "deg C", "%", "V", "m", "g", ""};
    for (int c = 0; c < kChannels; ++c) {
        names += "," + q("Channel " + std::to_string(c + 1));
        units += "," + q(kUnits[c % std::size(kUnits)]);
    }
    head += names + eol + units + eol + (legacy ? eol + eol : "");
    out << head;
    const std::uint64_t target = megabytes * 1'000'000;
    std::uint64_t written = head.size();
    std::string row;
    row.reserve(2048);
    for (std::uint64_t r = 0; written < target; ++r) {
        row.clear();
        if (legacy) {
            row += '"';
        }
        appendFixed(row, static_cast<double>(r) / 100.0, 2);
        if (legacy) {
            row += '"';
        }
        for (int c = 0; c < kChannels; ++c) {
            row += ',';
            if (legacy) {
                row += '"';
            }
            const double v = 100.0 + 50.0 * std::sin(static_cast<double>(r) * (0.001 + 0.0001 * c)) + c;
            appendFixed(row, v, 2);
            if (legacy) {
                row += '"';
            }
        }
        row += eol;
        out.write(row.data(), static_cast<std::streamsize>(row.size()));
        written += row.size();
    }
    out.close();
    if (!out) {
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

struct Run {
    double probeMs = 0;
    double openMs = 0;
    double firstDataMs = 0;
    std::uint64_t samples = 0;
};

// One open of `file` with a fresh cache and pool; false with `error` on a failure.
bool runOnce(const fs::path& file, const formats::ImporterRegistry& registry,
             const units::UnitRegistry& units, unsigned workers, Run& run, std::string& error) {
    store::ChunkCache cache(std::size_t{8} << 30);
    TaskPool pool(workers);
    formats::OpenContext ctx;
    ctx.units = &units;
    ctx.cache = &cache;
    ctx.pool = &pool;
    ctx.loadSidecar = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto opened = registry.open(file, {}, ctx);
    const auto t1 = std::chrono::steady_clock::now();
    if (!opened) {
        error = opened.error().toString();
        return false;
    }
    const auto& channels = opened->source->layers.at(0).channels;
    // As a graph's first build asks: every chunk of the drawn channels at Visible, in channel order (chunk
    // tasks on the pool, which run the slab sweeps in parallel), then this thread waits for them.
    std::vector<store::Column> drawn;
    for (std::size_t c = 0; c < channels.size() && c < static_cast<std::size_t>(kDrawn); ++c) {
        drawn.push_back(store::seriesValues(channels[c].series));
    }
    for (const store::Column& col : drawn) {
        for (SampleIdx k = 0; k < col.chunkCount(); ++k) {
            col.prefetch(k, Priority::Visible);
        }
    }
    for (const store::Column& col : drawn) {
        if (col.size() == 0) {
            continue;
        }
        auto h = col.require(0, col.size() - 1, Priority::Visible, {});
        if (!h) {
            error = h.error().toString();
            return false;
        }
        run.samples += static_cast<std::uint64_t>(col.size());
    }
    const auto t2 = std::chrono::steady_clock::now();
    run.openMs = ms(t1 - t0);
    run.firstDataMs = ms(t2 - t0);
    return true;
}

bool probeOnce(const fs::path& file, const formats::ImporterRegistry& registry, Run& run,
               std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    auto p = registry.probe(file);
    if (!p) {
        error = p.error().toString();
        return false;
    }
    run.probeMs = ms(std::chrono::steady_clock::now() - t0);
    return true;
}

json row(const Stats& s, double limit) {
    return json{{"n", s.n},       {"minMs", s.min},   {"medianMs", s.median},     {"p95Ms", s.p95},
                {"maxMs", s.max}, {"limitMs", limit}, {"pass", s.median <= limit}};
}

void report(json& rows, const std::string& id, const Stats& s, double limit) {
    std::printf("%-46s median of %zu runs %.1f ms (min %.1f, max %.1f)  limit %.0f ms  %s\n", id.c_str(), s.n,
                s.median, s.min, s.max, limit, s.median <= limit ? "PASS" : "FAIL");
    rows[id] = row(s, limit);
}

} // namespace

int main(int argc, char** argv) {
    int runs = 5;
    if (const auto e = getEnv("ESSES_BENCH_REPETITIONS"); e && std::atoi(e->c_str()) > 0) {
        runs = std::atoi(e->c_str());
    }
    bool smoke = false;
    bool gate = false;
    std::string jsonPath;
    if (const auto e = getEnv("ESSES_BENCH_OUT"); e && !e->empty()) {
        jsonPath = pathToUtf8(pathFromUtf8(*e) / "formats_bench.json");
    }
    fs::path legacyFile;
    fs::path genericFile;
    unsigned workers = std::max(2u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--runs") {
            runs = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--smoke") {
            smoke = true;
        } else if (a == "--gate") {
            gate = true;
        } else if (a == "--json") {
            jsonPath = next();
        } else if (a == "--file") {
            legacyFile = pathFromUtf8(next());
        } else if (a == "--generic-file") {
            genericFile = pathFromUtf8(next());
        } else if (a == "--workers") {
            workers = static_cast<unsigned>(std::max(1, std::atoi(next().c_str())));
        } else {
            std::fprintf(
                stderr, "usage: esses-formats-bench [--runs N] [--smoke] [--gate] [--json file] [--file csv] "
                        "[--generic-file csv] [--workers N]\n");
            return 2;
        }
    }
    if (smoke) {
        runs = 1;
    }
    const fs::path exeDir = fs::absolute(pathFromUtf8(argv[0])).parent_path();
    const std::uint64_t mb = smoke ? 2 : 300;
    const std::string tag = std::to_string(mb) + "mb-" + std::to_string(kChannels) + "col";
    if (legacyFile.empty()) {
        legacyFile = exeDir.parent_path() / "bench-data" / ("csv-bench-" + tag + "-legacy.csv");
    }
    if (genericFile.empty()) {
        genericFile = exeDir.parent_path() / "bench-data" / ("csv-bench-" + tag + "-plain.csv");
    }
    for (const auto& [file, legacy] : {std::pair{legacyFile, true}, std::pair{genericFile, false}}) {
        if (!fs::exists(file)) {
            std::printf("formats_bench: writing %s\n", pathToUtf8(file).c_str());
            if (!writeBenchFile(file, mb, legacy)) {
                std::fprintf(stderr, "formats_bench: cannot write %s\n", pathToUtf8(file).c_str());
                return 2;
            }
        }
    }
    auto unitsLoaded =
        units::UnitRegistry::loadFiles(pathFromUtf8(ESSES_BENCH_SOURCE_DIR) / "data" / "units.json");
    if (!unitsLoaded) {
        std::fprintf(stderr, "formats_bench: %s\n", unitsLoaded.error().toString().c_str());
        return 2;
    }
    const units::UnitRegistry units = std::move(unitsLoaded).value();
    const auto registry = formats::ImporterRegistry::withBuiltins(&units);

    std::string error;
    std::vector<double> probe;
    std::vector<double> legacyFirst;
    std::vector<double> genericFirst;
    json runRows = json::array();
    for (int r = -1; r < runs; ++r) { // r == -1: the untimed warm-up (the files into the OS cache)
        Run a;
        Run b;
        if (!probeOnce(legacyFile, *registry, a, error) ||
            !runOnce(legacyFile, *registry, units, workers, a, error) ||
            !runOnce(genericFile, *registry, units, workers, b, error)) {
            std::fprintf(stderr, "formats_bench: %s\n", error.c_str());
            return 2;
        }
        if (r < 0) {
            continue;
        }
        probe.push_back(a.probeMs);
        legacyFirst.push_back(a.firstDataMs);
        genericFirst.push_back(b.firstDataMs);
        std::printf(
            "run %d: probe %.1f ms; legacy open %.1f ms, first data %.1f ms (%llu samples); generic open "
            "%.1f ms, first data %.1f ms\n",
            r + 1, a.probeMs, a.openMs, a.firstDataMs, static_cast<unsigned long long>(a.samples), b.openMs,
            b.firstDataMs);
        runRows.push_back(json{{"probeMs", a.probeMs},
                               {"legacyOpenMs", a.openMs},
                               {"legacyFirstDataMs", a.firstDataMs},
                               {"genericOpenMs", b.openMs},
                               {"genericFirstDataMs", b.firstDataMs}});
    }
    json rows = json::object();
    const std::string size = std::to_string(mb) + "mb_" + std::to_string(kChannels) + "col";
    report(rows, "formats.csv_probe_" + size, statsOf(probe), kProbeLimitMs);
    report(rows, "formats.csv_first_data_" + size, statsOf(legacyFirst), kFirstDataLimitMs);
    report(rows, "formats.csv_generic_first_data_" + size, statsOf(genericFirst), kFirstDataLimitMs);
    std::printf("formats_bench: %s (%.1f MB), %d channels, %u workers, %d run(s) after a warm-up\n",
                pathToUtf8(legacyFile).c_str(), static_cast<double>(fs::file_size(legacyFile)) / 1e6,
                kChannels, workers, runs);
    if (!jsonPath.empty()) {
        json benchmarks = json::array();
        for (const auto& [id, r] : rows.items()) {
            benchmarks.push_back(json{{"name", id},
                                      {"run_name", id},
                                      {"run_type", "aggregate"},
                                      {"aggregate_name", "median"},
                                      {"repetitions", r["n"]},
                                      {"real_time", r["medianMs"]},
                                      {"cpu_time", r["medianMs"]},
                                      {"time_unit", "ms"},
                                      {"limit_ms", r["limitMs"]}});
        }
        const json doc{{"bench", "formats_bench"},
                       {"ac", "AC-M2-13"},
                       {"threads", workers},
                       {"file", pathToUtf8(legacyFile)},
                       {"bytes", fs::file_size(legacyFile)},
                       {"runs", runRows},
                       {"rows", rows},
                       {"benchmarks", benchmarks}};
        const fs::path out = pathFromUtf8(jsonPath);
        if (out.has_parent_path()) {
            fs::create_directories(out.parent_path());
        }
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << doc.dump(2) << "\n";
        if (!f) {
            std::fprintf(stderr, "formats_bench: cannot write %s\n", jsonPath.c_str());
            return 2;
        }
    }
    if (gate && !smoke) {
        for (const auto& [id, r] : rows.items()) {
            if (!r["pass"].get<bool>()) {
                return 1;
            }
        }
    }
    return 0;
}
