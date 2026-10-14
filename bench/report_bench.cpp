// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/report_bench (AC-M3-03; 07 section 12 "Channel Report 50 laps x 30 ch x 8 stats, cold <= 300 ms"):
// ReportEngine::cells of a channel_report over one outing of 50 flying laps of 90 s (4,500 s) with 30 F32
// channels at the RW-2 rate mix (4 x 1 kHz, 12 x 500 Hz, 10 x 100 Hz, 4 x 20 Hz; 49.9 M samples, smooth
// periodic signals with ripple; every fifth channel invalid for the first 0.1 s of each lap, so the start
// probe and the validity bitmaps are exercised), headers Outings:Laps, statistics avg, min, max, absMax,
// start, end, change, stdDev (12,000 cells). As in the application, the report runs on a TaskPool worker
// (the default worker count) at Background and the columns schedule their chunk tasks on the same pool.
// Rows:
//   report.channel_50lap_30ch_8stat_cold      a fresh StatCache and a fresh chunk cache: every chunk is
//                                             materialised from its in-memory provider during the report
//                                             (no disk I/O; "cold" as far as the process goes): <= 300 ms
//   report.channel_50lap_30ch_8stat_cold_one_thread
//                                             the same on the calling thread without a pool (every chunk
//                                             built inline): recorded
//   report.channel_50lap_30ch_8stat_resident  a fresh StatCache, the chunks resident (after an open and a
//                                             first paint): recorded
//   report.channel_50lap_30ch_8stat_cached    every cell from the StatCache (a re-run on Selection): recorded
//   report.time_50lap_12sec                   TimeReportEngine::build over the 50 laps of 12 sections each:
//                                             recorded
// Qt-free: std::chrono, nlohmann-json for --json (a Google-Benchmark-style "benchmarks" array plus the rows,
// as the other benches write). --smoke runs 5 laps once and checks the table against
// ReportEngine::statistic (ctest bench.report_bench.smoke); --gate exits 1 above a limit;
// ESSES_BENCH_REPETITIONS and ESSES_BENCH_OUT as for the other benches.

#include <esses/analysis/report_engine.hpp>
#include <esses/analysis/stat_cache.hpp>
#include <esses/analysis/time_report_engine.hpp>
#include <esses/base/env.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace esses;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr double kColdLimitMs = 300.0; // 07 section 12, AC-M3-03
constexpr double kLapSeconds = 90.0;
constexpr int kSectionsPerLap = 12;
constexpr std::size_t kCacheBudget = std::size_t{2} << 30; // every chunk of the workload stays resident

struct Stats {
    std::size_t n = 0;
    double min = 0;
    double median = 0;
    double max = 0;
};

Stats statsOf(std::vector<double> v) {
    Stats s;
    s.n = v.size();
    if (v.empty()) {
        return s;
    }
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.median = v.size() % 2 == 1 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
    s.max = v.back();
    return s;
}

double ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

Micros secondsToMicros(double s) {
    return static_cast<Micros>(std::llround(s * 1'000'000.0));
}

struct ChannelPlan {
    std::string name;
    std::uint32_t hz = 0;
    std::shared_ptr<store::InMemoryProvider> provider;
};

// The 30 channels of the RW-2 rate mix over `laps` laps.
std::vector<ChannelPlan> makeChannels(int laps) {
    std::vector<ChannelPlan> out;
    const std::pair<std::uint32_t, int> mix[] = {{1000, 4}, {500, 12}, {100, 10}, {20, 4}};
    int index = 0;
    for (const auto& [hz, count] : mix) {
        for (int k = 0; k < count; ++k, ++index) {
            ChannelPlan c;
            c.hz = hz;
            c.name = "Ch" + std::to_string(hz) + " " + std::to_string(k + 1);
            const auto n = static_cast<std::size_t>(laps * kLapSeconds * hz);
            const auto perLap = static_cast<std::size_t>(kLapSeconds * hz);
            const std::size_t invalidRun = hz / 10; // 0.1 s
            std::vector<float> v(n);
            const double a = 10.0 + index;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / hz;
                double x = a * std::sin(t * (0.07 + 0.003 * index)) + 0.1 * a * std::sin(t * 9.1);
                if (index % 5 == 0 && i % perLap < invalidRun) {
                    x = std::nan("");
                }
                v[i] = static_cast<float>(x);
            }
            c.provider = store::InMemoryProvider::fromF32(v, {}, "bench:" + c.name);
            out.push_back(std::move(c));
        }
    }
    return out;
}

model::Range rangeOf(std::string_view group, std::string name, double start, double end) {
    model::Range r;
    r.name = std::move(name);
    r.start = secondsToMicros(start);
    r.end = secondsToMicros(end);
    r.id = model::rangeId(group, model::MarkerId{}, r.name + "@" + std::to_string(r.start));
    return r;
}

// One outing of `laps` flying laps with kSectionsPerLap sections each.
std::shared_ptr<const model::DataSource> makeSource(int laps) {
    model::DataLayer layer;
    const double total = laps * kLapSeconds;
    layer.duration = secondsToMicros(total);
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    outings.ranges.push_back(rangeOf("Outings", "bench.ld", 0.0, total));
    outings.ranges.back().kind = model::RangeKind::Outing;
    model::RangeGroup lapGroup = model::RangeGroup::make("Outings:Laps");
    model::RangeGroup sections = model::RangeGroup::make("Outings:Laps:Track Sections:Default");
    for (int l = 0; l < laps; ++l) {
        const double s = l * kLapSeconds;
        lapGroup.ranges.push_back(
            rangeOf("Outings:Laps", "Lap " + std::to_string(l + 1), s, s + kLapSeconds));
        lapGroup.ranges.back().kind = model::RangeKind::Lap;
        const double step = kLapSeconds / kSectionsPerLap;
        for (int k = 0; k < kSectionsPerLap; ++k) {
            // Uneven sections: 0.8 .. 1.2 of the even step, the last one closing the lap.
            const double a = s + k * step;
            const double b =
                k + 1 == kSectionsPerLap ? s + kLapSeconds : a + step * (0.8 + 0.4 * ((k * 7 + l) % 5) / 4.0);
            sections.ranges.push_back(
                rangeOf(sections.path, "Turn " + std::to_string(k + 1), a, std::min(b, s + kLapSeconds)));
            sections.ranges.back().kind = model::RangeKind::Section;
        }
    }
    layer.ranges.setGroup(std::move(outings));
    layer.ranges.setGroup(std::move(lapGroup));
    layer.ranges.setGroup(model::RangeGroup::make("Outings:Laps:Track Sections"));
    layer.ranges.setGroup(std::move(sections));
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("report-bench");
    src->layers.push_back(std::move(layer));
    return src;
}

// The channels over a chunk cache: series and lookup data by name.
struct BoundChannels {
    std::unordered_map<std::string, analysis::ReportChannelData> byName;

    BoundChannels(const std::vector<ChannelPlan>& plans, const model::DataSource& src,
                  store::ChunkCache& cache, TaskPool* pool) {
        store::ColumnOptions options;
        options.cache = &cache;
        options.pool = pool;
        for (const ChannelPlan& p : plans) {
            analysis::ReportChannelData d;
            d.series = store::UniformSeries{Rate::fromHz(p.hz), 0, store::Column(p.provider, options)};
            model::ChannelDescriptor desc;
            desc.id = model::channelIdLogged(src.id, "Normal", p.name, "bench");
            desc.name = p.name;
            desc.rate = Rate::fromHz(p.hz);
            d.descriptor = desc;
            d.displayUnit = "unit";
            byName.emplace(p.name, std::move(d));
        }
    }
    analysis::ReportChannelLookup lookup() const {
        return [this](const analysis::ReportSource&, std::string_view name) {
            const auto it = byName.find(std::string(name));
            return it != byName.end() ? it->second : analysis::ReportChannelData{};
        };
    }
};

analysis::ReportSpec makeSpec(const std::vector<ChannelPlan>& plans) {
    analysis::ReportSpec spec;
    spec.scope = analysis::ReportScope::SelectedOutings;
    spec.headers = {"Outings:Laps"};
    using S = analysis::ReportStat;
    for (const ChannelPlan& p : plans) {
        spec.channels.push_back(analysis::ReportChannel{
            p.name, {S::Avg, S::Min, S::Max, S::AbsMax, S::Start, S::End, S::Change, S::StdDev}});
    }
    return spec;
}

analysis::ReportSource rangeSource(const std::shared_ptr<const model::DataSource>& src, std::string group,
                                   const model::Range& r) {
    analysis::ReportSource s;
    s.source = src;
    s.range.src = src->id;
    s.range.layer = "Normal";
    s.range.groupPath = std::move(group);
    s.range.range = r.id;
    s.range.name = r.name;
    return s;
}

// The table must be complete and agree with ReportEngine::statistic on a sample of cells.
bool checkTable(const analysis::ReportTable& t, const analysis::ReportSpec& spec, const BoundChannels& bound,
                const model::DataSource& src, int laps) {
    const std::size_t columns = spec.channels.size() * 8;
    if (t.rows.size() != static_cast<std::size_t>(laps) || t.columns.size() != columns ||
        t.cells.size() != t.rows.size() * columns) {
        std::fprintf(stderr, "report_bench: the table is %zu x %zu\n", t.rows.size(), t.columns.size());
        return false;
    }
    const model::RangeGroup* lapGroup = src.layers.front().ranges.find("Outings:Laps");
    for (std::size_t r = 0; r < t.rows.size(); ++r) {
        for (std::size_t c = 0; c < columns; c += 5) {
            const analysis::ReportCell& cell = t.at(r, c);
            const analysis::ReportColumn& col = t.columns[c];
            const auto it = bound.byName.find(spec.channels[col.channel].name);
            if (it == bound.byName.end() || !it->second.series || lapGroup == nullptr) {
                return false;
            }
            const model::Range& lap = lapGroup->ranges[r];
            Result<analysis::StatValue> want = analysis::ReportEngine::statistic(
                *it->second.series, col.stat, lap.start, lap.end, spec.options, {});
            const bool ok =
                want &&
                ((!want->valid && cell.state == analysis::ReportCellState::Blank) ||
                 (want->valid && cell.state == analysis::ReportCellState::Ok && cell.value == want->value));
            if (!ok) {
                std::fprintf(stderr, "report_bench: cell (%zu, %zu) disagrees with ReportEngine::statistic\n",
                             r, c);
                return false;
            }
        }
    }
    return true;
}

void report(json& rows, const std::string& id, const Stats& s, std::optional<double> limit) {
    json r{{"n", s.n}, {"minMs", s.min}, {"medianMs", s.median}, {"maxMs", s.max}};
    r["limitMs"] = limit ? json(*limit) : json(nullptr);
    r["pass"] = !limit || s.median <= *limit;
    std::printf("%-44s median %8.3f ms (min %.3f, max %.3f, n %zu)%s\n", id.c_str(), s.median, s.min, s.max,
                s.n, limit ? (r["pass"].get<bool>() ? "  PASS" : "  FAIL") : "  (recorded)");
    rows[id] = r;
}

} // namespace

int main(int argc, char** argv) {
    int runs = 7;
    if (const auto e = getEnv("ESSES_BENCH_REPETITIONS"); e && std::atoi(e->c_str()) > 0) {
        runs = std::atoi(e->c_str());
    }
    bool smoke = false;
    bool gate = false;
    std::string jsonPath;
    if (const auto e = getEnv("ESSES_BENCH_OUT"); e && !e->empty()) {
        jsonPath = pathToUtf8(pathFromUtf8(*e) / "report_bench.json");
    }
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
        } else {
            std::fprintf(stderr, "usage: esses-report-bench [--runs N] [--smoke] [--gate] [--json file]\n");
            return 2;
        }
    }
    if (smoke) {
        runs = 1;
    }
    const int laps = smoke ? 5 : 50;
    const std::vector<ChannelPlan> plans = makeChannels(laps);
    const std::shared_ptr<const model::DataSource> src = makeSource(laps);
    const analysis::ReportSpec spec = makeSpec(plans);
    const model::Range& outing = src->layers.front().ranges.find("Outings")->ranges.front();
    std::size_t samples = 0;
    for (const ChannelPlan& p : plans) {
        samples += static_cast<std::size_t>(p.provider->size());
    }
    std::printf("report_bench: %d laps, %zu channels, %zu samples, %zu cells\n", laps, plans.size(), samples,
                static_cast<std::size_t>(laps) * spec.channels.size() * 8);

    json rows = json::object();
    bool tableOk = true;
    // The application's path: a TaskPool (the default workers), the report on one of its workers at
    // Background, the columns scheduling their chunk tasks on the same pool.
    TaskPool pool;
    const auto onPool = [&pool](const analysis::ReportSpec& sp, const analysis::ReportInputs& in,
                                analysis::StatCache& stats) {
        return pool
            .submit(Priority::Background, TaskKey{}, CancelToken{},
                    [&sp, &in, &stats](const CancelToken& token) {
                        return analysis::ReportEngine::cells(sp, in, stats, token);
                    })
            .get();
    };
    const auto direct = [](const analysis::ReportSpec& sp, const analysis::ReportInputs& in,
                           analysis::StatCache& stats) {
        return analysis::ReportEngine::cells(sp, in, stats, {});
    };
    // Cold: per run a fresh chunk cache, fresh columns over the same providers and a fresh StatCache; with
    // the pool, and on the calling thread alone (no pool: every chunk materialised inline).
    std::vector<double> coldMs;
    std::vector<double> coldSerialMs;
    for (const bool pooled : {true, false}) {
        for (int r = 0; r < runs; ++r) {
            auto chunks = std::make_unique<store::ChunkCache>(kCacheBudget);
            std::optional<BoundChannels> bound(std::in_place, plans, *src, *chunks, pooled ? &pool : nullptr);
            analysis::StatCache stats;
            const analysis::ReportInputs inputs{{rangeSource(src, "Outings", outing)}, bound->lookup()};
            const auto t0 = std::chrono::steady_clock::now();
            Result<analysis::ReportTable> table =
                pooled ? onPool(spec, inputs, stats) : direct(spec, inputs, stats);
            (pooled ? coldMs : coldSerialMs).push_back(ms(std::chrono::steady_clock::now() - t0));
            if (!table) {
                std::fprintf(stderr, "report_bench: %s\n", table.error().toString().c_str());
                return 2;
            }
            if (r == 0) {
                tableOk = checkTable(*table, spec, *bound, *src, laps) && tableOk;
            }
            bound.reset(); // the columns before their cache
        }
    }
    // Resident and cached: one chunk cache warmed by an untimed run.
    std::vector<double> residentMs;
    std::vector<double> cachedMs;
    {
        auto chunks = std::make_unique<store::ChunkCache>(kCacheBudget);
        std::optional<BoundChannels> bound(std::in_place, plans, *src, *chunks, &pool);
        const analysis::ReportInputs inputs{{rangeSource(src, "Outings", outing)}, bound->lookup()};
        {
            analysis::StatCache warm;
            if (!onPool(spec, inputs, warm)) {
                return 2;
            }
        }
        for (int r = 0; r < runs; ++r) {
            analysis::StatCache stats;
            const auto t0 = std::chrono::steady_clock::now();
            Result<analysis::ReportTable> table = onPool(spec, inputs, stats);
            residentMs.push_back(ms(std::chrono::steady_clock::now() - t0));
            const auto t1 = std::chrono::steady_clock::now();
            Result<analysis::ReportTable> again = onPool(spec, inputs, stats);
            cachedMs.push_back(ms(std::chrono::steady_clock::now() - t1));
            if (!table || !again || again->cells != table->cells) {
                std::fprintf(stderr, "report_bench: the cached table differs\n");
                tableOk = false;
            }
        }
        bound.reset();
    }
    std::vector<double> timeMs;
    {
        analysis::TimeReportSpec timeSpec;
        timeSpec.bands = {analysis::TimeReportBand{1.0, 0x30C030FF, true},
                          analysis::TimeReportBand{3.0, 0xC0C030FF, true}};
        analysis::ReportInputs inputs;
        for (const model::Range& lap : src->layers.front().ranges.find("Outings:Laps")->ranges) {
            inputs.sources.push_back(rangeSource(src, "Outings:Laps", lap));
        }
        for (int r = 0; r < runs; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            Result<analysis::TimeReportTable> table = analysis::TimeReportEngine::build(timeSpec, inputs, {});
            timeMs.push_back(ms(std::chrono::steady_clock::now() - t0));
            if (!table || table->rows.size() != static_cast<std::size_t>(laps) ||
                table->sections.size() != static_cast<std::size_t>(kSectionsPerLap) || !table->eclectic ||
                !table->rollingMin) {
                std::fprintf(stderr, "report_bench: the time report is incomplete\n");
                tableOk = false;
            }
        }
    }
    const std::string suffix = smoke ? "_smoke" : "";
    report(rows, "report.channel_50lap_30ch_8stat_cold" + suffix, statsOf(coldMs),
           smoke ? std::nullopt : std::optional<double>(kColdLimitMs));
    report(rows, "report.channel_50lap_30ch_8stat_cold_one_thread" + suffix, statsOf(coldSerialMs),
           std::nullopt);
    report(rows, "report.channel_50lap_30ch_8stat_resident" + suffix, statsOf(residentMs), std::nullopt);
    report(rows, "report.channel_50lap_30ch_8stat_cached" + suffix, statsOf(cachedMs), std::nullopt);
    report(rows, "report.time_50lap_12sec" + suffix, statsOf(timeMs), std::nullopt);
    if (!tableOk) {
        std::fprintf(stderr, "report_bench: the report tables are wrong\n");
    }

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
        const json doc{{"bench", "report_bench"},
                       {"ac", json::array({"AC-M3-03"})},
                       {"workers", pool.stats().workers},
                       {"cores", std::thread::hardware_concurrency()},
                       {"laps", laps},
                       {"channels", plans.size()},
                       {"samples", samples},
                       {"rows", rows},
                       {"benchmarks", benchmarks}};
        const fs::path out = pathFromUtf8(jsonPath);
        if (out.has_parent_path()) {
            fs::create_directories(out.parent_path());
        }
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << doc.dump(2) << "\n";
        if (!f) {
            std::fprintf(stderr, "report_bench: cannot write %s\n", jsonPath.c_str());
            return 2;
        }
    }
    if (!tableOk) {
        return 1;
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
