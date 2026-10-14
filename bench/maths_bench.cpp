// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// bench/maths_bench (AC-M2-14; 07 section 12 "elementwise 2.16 M samples <= 10 ms one core"; 03 section
// 14.3): the maths language on one core. Rows and limits:
//   maths.elementwise_stage_2160k  the fused elementwise stage (one EwKernel) of
//                                  'Ground Speed'[km/h] * 2 + 1 over 2.16 M samples already gathered at
//                                  the result rate: <= 10 ms
//   maths.elementwise_eval_2160k   evaluateWhole of the same plan: the gather from the chunk store, the
//                                  stage and the 2.16 M-sample result (recorded, no limit)
//   maths.parse_check              Parser::parse + TypeChecker::check of an item of about 70 nodes
//                                  (03 section 14.3: parse+check <= 1 ms per expression): <= 1 ms
// The channel is an in-memory F32 column at 1 kHz in a chunk cache (first run untimed, so its chunks are
// resident).
//
// AC-M2-10 (07 section 12 "20 maths channels of bench/workloads.md on RW-2 ready <= 2 frames"; D-042, D-139):
//   maths.rw2_20_ready             the 20 expressions of bench/workloads.md ("Maths readiness set") as one
//                                  global module of a fresh MathGraph over RW-2 (in memory, its columns
//                                  resident after an untimed run): from the first require() of the 20 outputs
//                                  to the last Future ready, every node one task (its later chunks offered
//                                  to idle workers, D-191), parallel across nodes on the default TaskPool:
//                                  <= 2 frames at 60 Hz (33.3 ms); the logical processor count and the
//                                  workers are part of the result
//   maths.rw2_20_define            setModules() of the 20 items on that graph (parse, resolve, type check,
//                                  compile; owning thread): recorded
//   maths.filter_lp_zero_phase_21600k  evaluateWhole of filter_lp('X', 5, -2) over 21.6 M samples (6 h at
//                                  1 kHz) on one core: recorded (its 150 ms gate is v1.0, 07 section 12)
//
// AC-M2-05 (07 section 5.2: "beacon insert and MarkerEdit.offset rebuild laps < 5 ms on a 60-lap outing"):
//   analysis.marker_rebuild_60lap  DataSource::withSidecar with the circuit rules after a user beacon
//                                  inserted in lap 10 and after an offset edit of beacon 20, on a 60-lap
//                                  outing of 200 channels (the outing of libs/analysis/tests/
//                                  marker_edit_rebuild_tests.cpp), one core, both rebuilds per run: <= 5 ms
// Qt-free: std::chrono, nlohmann-json for --json (a Google-Benchmark-style "benchmarks" array plus the rows,
// as the other benches write). --smoke uses 100,000 samples, RW-2 shortened to 60 s, 216,000 samples for
// filter_lp and one run (ctest bench.maths_bench.smoke). --nodes prints each readiness output's time when
// required alone; --workers N runs the readiness set on a pool of N workers (1: the serial cost of the set).
// --digest prints, instead of timing anything, a fingerprint of every output of the readiness set (RW-2
// shortened to 120 s) and of a module of 67 expressions over every kernel class with invalid runs, gaps and
// mixed rates, through MathGraph and through evaluateWhole: two builds that print the same lines compute the
// same results bit for bit (the check of an evaluation change that must not change results, D-191).

#include <esses/analysis/range_tables.hpp>
#include <esses/analysis/rule_runner.hpp>
#include <esses/base/env.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/uuid.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace esses;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr double kStageLimitMs = 10.0;
constexpr double kParseLimitMs = 1.0;
constexpr double kReadyLimitMs = 2.0 * 1000.0 / 60.0; // two frames at 60 Hz
constexpr double kMarkerRebuildLimitMs = 5.0;         // AC-M2-05

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

// 'Ground Speed' and nothing else.
class BenchScope final : public maths::Scope {
public:
    BenchScope(const model::DataSource& src, const units::UnitRegistry& units)
        : Scope(maths::FunctionRegistry::builtin(), units), src_(src) {}

    std::optional<maths::ScopeEntry> lookup(std::string_view nameCf,
                                            const maths::ChannelKey& /*requester*/) const override {
        const model::Channel* ch = src_.layers.front().find(nameCf);
        if (ch == nullptr) {
            return std::nullopt;
        }
        maths::ScopeEntry e;
        e.type.kind = maths::Kind::Series;
        e.type.unit = ch->desc.unit;
        e.type.rate = ch->desc.rate;
        e.key = maths::ChannelKey::make(src_.id, "Normal", ch->desc.name);
        e.interp = ch->desc.interp;
        e.dtype = ch->desc.dtype;
        return e;
    }

private:
    const model::DataSource& src_;
};

// ---- AC-M2-10: RW-2 and the maths readiness set (bench/workloads.md) -------------------------------------

// RW-2 "Sprint": 8 channels at 1 kHz, 32 at 500 Hz, 60 at 100 Hz, 100 at 20 Hz over `duration` (30 min), in
// memory, with an outing and 90 s laps. Signals are smooth periodic shapes with a little fast ripple.
std::shared_ptr<const model::DataSource> makeRw2(store::ChunkCache& cache, const units::UnitRegistry& units,
                                                 Micros duration) {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("maths-bench-rw2");
    model::DataLayer layer;
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
            v[i] = static_cast<float>(base + amplitude * std::sin(6.283185307179586 * t / periodS + phase) +
                                      0.02 * amplitude * std::sin(37.0 * t + phase));
        }
        model::Channel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.unit = units.find(unit).value_or(units::kNoUnit);
        ch.desc.quantity = units.quantityOf(ch.desc.unit);
        ch.desc.rate = Rate::fromHz(hz);
        ch.desc.id = model::channelIdLogged(src->id, "Normal", name, "bench");
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
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    model::Range outing;
    outing.id = model::rangeId("Outings", model::MarkerId{uuidFromHash(hash128("rw2 outing"))});
    outing.kind = model::RangeKind::Outing;
    outing.start = 0;
    outing.end = duration;
    outing.number = 1;
    outings.ranges.push_back(outing);
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    int lap = 1;
    for (Micros t = 0; t < duration; t += 90'000'000, ++lap) {
        model::Range r;
        r.id = model::rangeId("Outings:Laps",
                              model::MarkerId{uuidFromHash(hash128("rw2 lap " + std::to_string(lap)))});
        r.kind = model::RangeKind::Lap;
        r.lapClass = model::LapClass::Flying;
        r.start = t;
        r.end = std::min<Micros>(t + 90'000'000, duration);
        r.number = lap;
        laps.ranges.push_back(r);
    }
    layer.ranges.setGroup(std::move(outings));
    layer.ranges.setGroup(std::move(laps));
    src->layers.push_back(std::move(layer));
    return src;
}

// The 20 expressions of the maths readiness set (bench/workloads.md): Elementwise 5, Windowed 4, IIR 4,
// Stateful 5, Range 2.
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

// Waits until `pool` has completed every task. A worker completes a task (and drops its key) a moment after
// the task's Future is ready, and an equal key submitted meanwhile joins that task (01 section 5.4): a fresh
// graph over the same source would get the previous graph's results instead of evaluating.
void waitIdle(const TaskPool& pool) {
    for (;;) {
        const TaskPool::Stats s = pool.stats();
        std::size_t queued = 0;
        for (const std::size_t q : s.queued) {
            queued += q;
        }
        if (s.running == 0 && s.parked == 0 && queued == 0) {
            return;
        }
        std::this_thread::yield();
    }
}

struct Readiness {
    std::vector<double> readyMs;
    std::vector<double> defineMs;
    std::string error; // why the set could not be measured
};

// One untimed run (so the inputs' chunks are resident, like RW-2 after open), then `runs` timed ones, each on
// a fresh graph (fresh versions and columns).
Readiness measureReadiness(const std::shared_ptr<const model::DataSource>& src,
                           const std::shared_ptr<const units::UnitRegistry>& units, TaskPool& pool,
                           store::ChunkCache& cache, int runs) {
    Readiness out;
    const maths::MathModule module = readinessModule();
    for (int r = -1; r < runs; ++r) {
        waitIdle(pool); // the previous run's tasks are complete: every output of this graph is evaluated anew
        maths::MathGraph graph(src, "Normal", units, {}, &pool, &cache);
        const auto t0 = std::chrono::steady_clock::now();
        static_cast<void>(graph.setModules(std::span<const maths::MathModule>(&module, 1), nullptr, nullptr));
        const auto t1 = std::chrono::steady_clock::now();
        for (const maths::ChannelKey& key : graph.outputs()) {
            const maths::MathStatus s = graph.status(key);
            if (s.state == maths::MathState::Error) {
                out.error = "'" + key.nameCf + "': " + s.summary;
                return out;
            }
        }
        std::vector<Future<maths::ColumnHandle>> futures;
        const auto t2 = std::chrono::steady_clock::now();
        for (const maths::ChannelKey& key : graph.outputs()) {
            futures.push_back(graph.require(key, maths::SampleRange::all(), Priority::Visible, {}));
        }
        for (const Future<maths::ColumnHandle>& f : futures) {
            if (!f.get()) {
                out.error = f.get().error().toString();
                return out;
            }
        }
        const auto t3 = std::chrono::steady_clock::now();
        static_cast<void>(graph.applyReadyCompletions());
        if (graph.evaluationCount() != futures.size()) {
            out.error = "expected one evaluation per output";
            return out;
        }
        if (r >= 0) {
            out.defineMs.push_back(ms(t1 - t0));
            out.readyMs.push_back(ms(t3 - t2));
        }
    }
    return out;
}

// --nodes: each output of the readiness set required alone on a fresh graph (its upstream already
// evaluated), one after the other: where the time goes.
void printNodeTimes(const std::shared_ptr<const model::DataSource>& src,
                    const std::shared_ptr<const units::UnitRegistry>& units, TaskPool& pool,
                    store::ChunkCache& cache) {
    const maths::MathModule module = readinessModule();
    waitIdle(pool);
    maths::MathGraph graph(src, "Normal", units, {}, &pool, &cache);
    static_cast<void>(graph.setModules(std::span<const maths::MathModule>(&module, 1), nullptr, nullptr));
    for (const maths::ChannelKey& key : graph.outputs()) {
        const auto t0 = std::chrono::steady_clock::now();
        Future<maths::ColumnHandle> f = graph.require(key, maths::SampleRange::all(), Priority::Visible, {});
        const bool done = f.get().ok();
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("  %-24s %8.3f ms%s\n", key.nameCf.c_str(), ms(t1 - t0), done ? "" : "  (failed)");
    }
}

// filter_lp order -2 (zero phase) over `samples` at 1 kHz on the calling thread (one core).
Result<std::vector<double>> measureFilterLp(const std::shared_ptr<const units::UnitRegistry>& units,
                                            store::ChunkCache& cache, std::size_t samples, int runs) {
    std::vector<float> values(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) * 1e-3;
        values[i] = static_cast<float>(std::sin(t) + 0.1 * std::sin(150.0 * t));
    }
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("maths-bench-filter");
    model::DataLayer layer;
    layer.duration = static_cast<Micros>(samples) * 1000;
    model::Channel x;
    x.desc.name = "X";
    x.desc.loggedName = "X";
    x.desc.rate = Rate::fromHz(1000);
    store::ColumnOptions options;
    options.cache = &cache;
    x.series = store::UniformSeries{
        Rate::fromHz(1000), 0,
        store::Column(store::InMemoryProvider::fromF32(values, {}, "filter:X"), options)};
    layer.addChannel(std::move(x));
    src->layers.push_back(std::move(layer));
    const BenchScope scope(*src, *units);
    maths::Diagnostics diags;
    Result<maths::Ast> ast = maths::Parser::parse("filter_lp('X', 5, -2)", {}, diags);
    if (!ast) {
        return Failure{ast.error()};
    }
    maths::EvalContext ctx;
    ctx.source = src;
    ctx.layer = "Normal";
    ctx.resultRate = Rate::fromHz(1000);
    ctx.resultCount = samples;
    ctx.units = units;
    ctx.getChannel = [src](const maths::ChannelKey& key, units::UnitId,
                           Rate rate) -> Result<maths::MathInputGatherer> {
        const model::Channel* ch = src->layers.front().find(key.nameCf);
        if (ch == nullptr) {
            return fail(ErrorCode::NotFound, "no channel " + key.nameCf);
        }
        return maths::MathInputGatherer(ch->series, rate, 0, ch->desc.interp, units::Converter{},
                                        CancelToken{});
    };
    std::vector<double> times;
    for (int r = -1; r < runs; ++r) {
        Result<maths::Plan> plan = maths::compile(*ast, scope, maths::MathItem{}, {}, diags);
        if (!plan) {
            return Failure{plan.error()};
        }
        const auto t0 = std::chrono::steady_clock::now();
        Result<analysis::UniformSignal> out = maths::evaluateWhole(*plan, ctx);
        const auto t1 = std::chrono::steady_clock::now();
        if (!out) {
            return Failure{out.error()};
        }
        if (r >= 0) {
            times.push_back(ms(t1 - t0));
        }
    }
    return times;
}

// AC-M2-05: the wall times of the lap rebuilds after a beacon insert and after a MarkerEdit offset on a
// 60-lap outing (flying laps of 92 s between 61 beacons, 200 channel descriptors the snapshot copies), one
// untimed round first.
std::vector<double> measureMarkerRebuild(int runs) {
    constexpr int kLaps = 60;
    constexpr double kLapS = 92.0;
    constexpr double kFirstBeacon = 40.0;
    constexpr int kChannels = 200;
    const auto sec = [](double s) { return static_cast<Micros>(std::llround(s * 1e6)); };
    const auto beaconAt = [&](int k) { return kFirstBeacon + kLapS * static_cast<double>(k); };
    const double end = beaconAt(kLaps) + 50.0;
    std::vector<model::Marker> markers{
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived),
        model::makeLoggedMarker(model::MarkerClass::EOL, sec(end), "", "", "", 0, 0,
                                model::Provenance::Derived)};
    for (int k = 0; k <= kLaps; ++k) {
        markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, sec(beaconAt(k)), "Beacons",
                                                  "B" + std::to_string(k), "", 77, 0,
                                                  model::Provenance::Companion));
    }
    model::sortMarkers(markers);
    model::DataLayer layer;
    layer.duration = sec(end);
    layer.markers = markers;
    layer.baseMarkers = markers;
    for (int c = 0; c < kChannels; ++c) {
        model::Channel ch;
        ch.desc.name = "Channel " + std::to_string(c);
        ch.desc.loggedName = ch.desc.name;
        ch.desc.rate = Rate::fromHz(100);
        layer.addChannel(std::move(ch));
    }
    model::DataSource src;
    src.layers.push_back(std::move(layer));
    src.details.push_back(
        model::Detail{"File Name", std::string("bench.ld"), model::Provenance::Derived, std::nullopt});
    src.baseDetails = src.details;
    const analysis::RuleRunner runner = analysis::RuleRunner::circuitDefault();
    runner.run(src);

    model::SidecarDocument insert = src.sidecar; // SHIFT+F7 inside lap 10
    model::Marker manual;
    manual.cls = model::MarkerClass::BCN;
    manual.group = "Beacons";
    manual.name = "Manual.1";
    manual.t = sec(beaconAt(9) + kLapS / 2.0);
    insert.addUserMarker("Normal", manual);
    model::SidecarDocument offset = src.sidecar; // beacon 20 moved 1.5 s earlier
    for (const model::Marker& m : src.layers[0].markers) {
        if (m.name == "B20") {
            static_cast<void>(offset.applyEdit(
                "Normal", model::MarkerEdit{m.id, model::MarkerEditOp::Offset, -1'500'000, 0.0, "m"}));
        }
    }
    std::vector<double> out;
    for (int r = -1; r < runs; ++r) {
        for (const model::SidecarDocument* doc : {&insert, &offset}) {
            const auto t0 = std::chrono::steady_clock::now();
            const std::shared_ptr<const model::DataSource> rebuilt =
                src.withSidecar(*doc, nullptr, runner.rebuildFunction());
            const auto t1 = std::chrono::steady_clock::now();
            if (rebuilt == nullptr) {
                return {};
            }
            if (r < 0 && doc == &insert) {
                // The untimed round checks the rebuild did the work: one flying lap more after the insert.
                const model::RangeGroup* laps = rebuilt->layers[0].ranges.find("Outings:Laps");
                const auto flying =
                    laps == nullptr
                        ? 0
                        : std::count_if(laps->ranges.begin(), laps->ranges.end(), [](const model::Range& g) {
                              return g.lapClass == model::LapClass::Flying;
                          });
                if (flying != kLaps + 1) {
                    std::fprintf(stderr, "maths_bench: the marker rebuild gave %d flying laps, not %d\n",
                                 static_cast<int>(flying), kLaps + 1);
                    return {};
                }
            }
            if (r >= 0) {
                out.push_back(ms(t1 - t0));
            }
        }
    }
    return out;
}

// ---- --digest: a fingerprint of every result (evaluation changes must leave it unchanged) -----------------

// A source for the digest: 300 s with invalid runs (short gaps, a 3 s gap that re-primes the filters, single
// invalid samples), channels at 1 kHz, 500 Hz, 100 Hz (F64 and I32) and 20 Hz, and 88 s laps with gaps.
std::shared_ptr<const model::DataSource> makeDigestSource(store::ChunkCache& cache,
                                                          const units::UnitRegistry& units) {
    constexpr Micros kDuration = 300'000'000;
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("maths-bench-digest");
    model::DataLayer layer;
    layer.duration = kDuration;
    store::ColumnOptions options;
    options.cache = &cache;
    std::uint64_t rng = 0x9E3779B97F4A7C15ull;
    const auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    const auto validity = [&](std::size_t n, std::uint32_t hz, bool gaps) {
        std::vector<bool> valid(n, true);
        if (!gaps) {
            return valid;
        }
        for (std::size_t i = 0; i < n;) {
            i += 1 + static_cast<std::size_t>(next() % (std::uint64_t{7} * hz));
            const std::size_t len = 1 + static_cast<std::size_t>(next() % 50);
            for (std::size_t k = i; k < std::min(n, i + len); ++k) {
                valid[k] = false;
            }
            i += len;
        }
        for (std::size_t k = std::size_t{100} * hz; k < std::min(n, std::size_t{103} * hz); ++k) {
            valid[k] = false; // 3 s: longer than the filters' 1 s re-prime gap
        }
        return valid;
    };
    const auto wave = [](std::size_t i, std::uint32_t hz, double base, double amp, double period) {
        const double t = static_cast<double>(i) / static_cast<double>(hz);
        return base + amp * std::sin(6.283185307179586 * t / period) + 0.05 * amp * std::sin(41.0 * t);
    };
    const auto addChannel = [&](const std::string& name, std::uint32_t hz, std::string_view unit,
                                std::shared_ptr<store::IChunkProvider> provider) {
        model::Channel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.unit = units.find(unit).value_or(units::kNoUnit);
        ch.desc.quantity = units.quantityOf(ch.desc.unit);
        ch.desc.rate = Rate::fromHz(hz);
        ch.desc.dtype = provider->dtype();
        if (provider->dtype() == store::Dtype::I32) {
            ch.desc.interp = store::Interp::Previous;
        }
        ch.desc.id = model::channelIdLogged(src->id, "Normal", name, "bench");
        ch.series = store::UniformSeries{Rate::fromHz(hz), 0, store::Column(std::move(provider), options)};
        layer.addChannel(std::move(ch));
    };
    const auto f32 = [&](const std::string& name, std::uint32_t hz, std::string_view unit, double base,
                         double amp, double period, bool gaps) {
        const auto n = static_cast<std::size_t>(kDuration / 1'000'000 * static_cast<Micros>(hz));
        std::vector<float> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = static_cast<float>(wave(i, hz, base, amp, period));
        }
        addChannel(name, hz, unit, store::InMemoryProvider::fromF32(v, validity(n, hz, gaps), "dg:" + name));
    };
    f32("A", 1000, "m/s", 30.0, 20.0, 9.0, true);
    f32("B", 1000, "mm", 5.0, 3.0, 0.8, false);
    f32("C", 500, "", 1.0, 2.0, 5.0, true);
    f32("D", 500, "", 50.0, 49.0, 11.0, false);
    f32("T", 20, "", 3.0, 1.0, 30.0, true);
    {
        const std::size_t n = std::size_t{100} * 300u;
        std::vector<double> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = wave(i, 100, 40.0, 25.0, 60.0);
        }
        addChannel("S", 100, "m/s", store::InMemoryProvider::fromF64(v, validity(n, 100, true), "dg:S"));
        std::vector<std::int32_t> g(n);
        for (std::size_t i = 0; i < n; ++i) {
            g[i] = static_cast<std::int32_t>(std::lround(wave(i, 100, 3.0, 3.4, 20.0)));
        }
        addChannel("G", 100, "", store::InMemoryProvider::fromI32(g, validity(n, 100, true), "dg:G"));
    }
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    model::Range outing;
    outing.id = model::rangeId("Outings", model::MarkerId{uuidFromHash(hash128("dg outing"))});
    outing.kind = model::RangeKind::Outing;
    outing.start = 0;
    outing.end = kDuration;
    outing.number = 1;
    outings.ranges.push_back(outing);
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    int lap = 1;
    for (Micros t = 1'500'000; t < kDuration; t += 90'000'000, ++lap) {
        model::Range r;
        r.id = model::rangeId("Outings:Laps",
                              model::MarkerId{uuidFromHash(hash128("dg lap " + std::to_string(lap)))});
        r.kind = model::RangeKind::Lap;
        r.lapClass = model::LapClass::Flying;
        r.start = t;
        r.end = std::min<Micros>(t + 88'000'000, kDuration);
        r.number = lap;
        laps.ranges.push_back(r);
    }
    layer.ranges.setGroup(std::move(outings));
    layer.ranges.setGroup(std::move(laps));
    src->layers.push_back(std::move(layer));
    return src;
}

// Every kernel class and parameter form over channels with invalid runs (the digest's second module).
maths::MathModule digestModule() {
    const std::pair<const char*, const char*> items[] = {
        {"ew1", "('A' + 'B') / 2"},
        {"ew2", "choose('C' > 0.5, 'A', 'D' * 2)"},
        {"ew3", "hypot('C', 'D') + 'S'[km/h]"},
        {"ew4", "'G' * 2 + 1"},
        {"ew5", "'A' + 'T' - 'S'"},
        {"ew6", "max('C' + 'T', 1) - rand_chan()"},
        {"sm1", "smooth('A', 5)"},
        {"sm2", "smooth('A', 4)"},
        {"sm3", "smooth('C', 0.5)"},
        {"sm4", "smooth('B', 5001)"},
        {"sm5", "smooth('S', 1)"},
        {"sm6", "smooth('G', 7)"},
        {"dv1", "derivative('A')"},
        {"dv2", "derivative('B')"},
        {"dv3", "derivative('A', 11)"},
        {"dv4", "derivative('C', 0.02)"},
        {"dv5", "derivative('A', 'B')"},
        {"dv6", "derivative('C', 'D', 5)"},
        {"dv7", "derivative('C', 'D', 21)"},
        {"dv8", "derivative(smooth('A', 5))"},
        {"dv9", "derivative('A', 6)"},
        {"ir1", "filter_lp('A', 10, 1)"},
        {"ir2", "filter_lp('A', 10, 2)"},
        {"ir3", "filter_lp('C', 5, 3)"},
        {"ir4", "filter_lp('A', 5, -1)"},
        {"ir5", "filter_lp('C', 5, -2)"},
        {"ir6", "filter_hp('A', 1, 1)"},
        {"ir7", "filter_hp('B', 1, 2)"},
        {"ir8", "filter_hp('C', 2, 3)"},
        {"ir9", "filter_hp('C', 2, -2)"},
        {"ic1", "filter_cheby_lp('C', 20, 2)"},
        {"ic2", "filter_cheby_lp('C', 20, 4, 0.5)"},
        {"ic3", "filter_cheby_lp('A', 20, 6, 1)"},
        {"ic4", "filter_cheby_lp('C', 20, -4, 0.5)"},
        {"ic5", "filter_cheby_lp('C', 20, -8, 0.5)"},
        {"ic6", "filter_cheby_hp('A', 3, 4, 0.5)"},
        {"ic7", "filter_cheby_bp('A', 5, 20, 4, 1)"},
        {"ic8", "filter_cheby_bs('C', 5, 20, -4)"},
        {"ic9", "filter_cheby_lp('C', 20, -12, 0.1)"},
        {"st1", "integrate('A')"},
        {"st2", "integrate('C', 'D' > 50)"},
        {"st3", "integrate('S', 'G' > 2, range_change(\"Outings:Laps\"))"},
        {"st4", "stat_max('S', 'G' > 0, range_change(\"Outings:Laps\"))"},
        {"st5", "stat_mean('A')"},
        {"st6", "stat_std_dev('C', 'D' > 20)"},
        {"st7", "stat_min('C', 'D' > 20, range_change(\"Outings:Laps\"))"},
        {"st8", "stat_start('S', 'G' > 1, range_change(\"Outings:Laps\"))"},
        {"st9", "stat_end('T')"},
        {"rm1", "running_max('S', 'G' > 0, range_change(\"Outings:Laps\"))"},
        {"rm2", "running_min('A')"},
        {"rm3", "running_max('C', 'D' > 10)"},
        {"ff1", "flip_flop('C' > 2, 'C' < 0)"},
        {"ed1", "edge_delay('C' > 1, 0.05, rise)"},
        {"ed2", "edge_delay('C' > 1, 0.05, fall)"},
        {"ed3", "time_valid('A' > 30, 0.2)"},
        {"ts1", "time_shift('D', 0.1)"},
        {"ts2", "time_shift('A', -0.25)"},
        {"ts3", "time_shift('C', 1000)"},
        {"pw1", "pulse_width('C' > 1)"},
        {"ps1", "previous_sample('A', 3)"},
        {"ps2", "previous_sample('G')"},
        {"rg1", "range_is(\"Outings:Laps\")"},
        {"rg2", "range_change(\"Outings:Laps\")"},
        {"mx1", "filter_lp(smooth('A', 9), 10, 2) + integrate('C')"},
        {"mx2", "derivative(filter_cheby_lp('C', 20, 4, 0.5), 'D')"},
        {"mx3", "smooth(derivative('A', 3), 0.1) * 'B'"},
        {"mx4", "time_shift(smooth('C', 7), -0.05) - previous_sample('C', 2)"},
    };
    maths::MathModule m;
    m.id = "digest";
    for (const auto& [name, text] : items) {
        maths::MathItem item;
        item.name = name;
        item.expr = text;
        m.items.push_back(std::move(item));
    }
    return m;
}

// The fingerprint of a whole signal: its grid, every valid value's bits and every invalid sample's position.
std::string digestOf(const analysis::UniformSignal& s) {
    Hash128Stream h;
    h.updateU64(s.values.size());
    h.updateU64(static_cast<std::uint64_t>(s.rate.num));
    h.updateU64(static_cast<std::uint64_t>(s.rate.den));
    h.updateU64(static_cast<std::uint64_t>(s.t0));
    std::size_t invalid = 0;
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        const bool valid = s.validAt(i) && !std::isnan(s.values[i]);
        std::uint64_t bits = 0x7FF8DEAD7FF8DEADull;
        if (valid) {
            std::memcpy(&bits, &s.values[i], sizeof bits);
        } else {
            ++invalid;
        }
        h.updateU64(bits);
    }
    return h.digest().hex() + " n=" + std::to_string(s.values.size()) + " invalid=" + std::to_string(invalid);
}

// --digest: one line per output of the readiness set (RW-2 shortened to 120 s) and of the digest module
// (makeDigestSource), through MathGraph (the sealed column) and through evaluateWhole (as esses-cli eval runs
// it). Two builds that print the same lines compute the same results.
int printDigests(const std::shared_ptr<const units::UnitRegistry>& units) {
    store::ChunkCache cache(std::size_t{4} << 30); // before the pool, which joins first (see main())
    TaskPool pool;
    const std::shared_ptr<const model::DataSource> digestSrc = makeDigestSource(cache, *units);
    const std::shared_ptr<const model::DataSource> rw2 = makeRw2(cache, *units, 120'000'000);
    int failures = 0;
    const std::pair<std::shared_ptr<const model::DataSource>, maths::MathModule> sets[] = {
        {rw2, readinessModule()}, {digestSrc, digestModule()}};
    for (const auto& [source, module] : sets) {
        const char* label = source == rw2 ? "rw2" : "dg";
        maths::MathGraph graph(source, "Normal", units, {}, &pool, &cache);
        static_cast<void>(graph.setModules(std::span<const maths::MathModule>(&module, 1), nullptr, nullptr));
        std::vector<std::pair<maths::ChannelKey, Future<maths::ColumnHandle>>> futures;
        for (const maths::ChannelKey& key : graph.outputs()) {
            futures.emplace_back(key, graph.require(key, maths::SampleRange::all(), Priority::Visible, {}));
        }
        for (const auto& [key, f] : futures) {
            const Result<maths::ColumnHandle>& r = f.get();
            std::string line;
            if (!r) {
                line = "error: " + r.error().toString();
                ++failures;
            } else if (Result<analysis::UniformSignal> s = maths::readWholeSignal(r->series, {}); s) {
                line = digestOf(*s);
            } else {
                line = "read failed: " + s.error().toString();
                ++failures;
            }
            std::printf("%s graph %-22s %s\n", label, key.nameCf.c_str(), line.c_str());
        }
        const BenchScope scope(*source, *units);
        const model::DataLayer& layer = source->layers.front();
        const std::shared_ptr<const analysis::RangeTables> ranges = analysis::RangeTables::build(layer);
        for (const maths::MathItem& item : module.items) {
            maths::Diagnostics diags;
            Result<maths::Ast> ast = maths::Parser::parse(item.expr, {}, diags);
            Result<maths::Plan> plan = ast ? maths::compile(*ast, scope, item, {}, diags)
                                           : Result<maths::Plan>(Failure{ast.error()});
            if (!plan) {
                std::printf("%s whole %-22s compile error: %s\n", label, item.name.c_str(),
                            plan.error().toString().c_str());
                continue;
            }
            const Rate rate = plan->out.rate.value_or(Rate::fromHz(1000));
            maths::EvalContext ctx;
            ctx.source = source;
            ctx.layer = "Normal";
            ctx.resultRate = rate;
            ctx.resultCount = static_cast<std::uint64_t>(rate.indexAt(layer.duration - 1, 0)) + 1;
            ctx.units = units;
            ctx.ranges = ranges;
            ctx.mathEndUs = layer.duration;
            ctx.getChannel = [source, units](const maths::ChannelKey& key, units::UnitId unit,
                                             Rate r) -> Result<maths::MathInputGatherer> {
                const model::Channel* ch = source->layers.front().find(key.nameCf);
                if (ch == nullptr) {
                    return fail(ErrorCode::NotFound, "no channel " + key.nameCf);
                }
                units::Converter conv;
                if (unit != ch->desc.unit && unit != units::kNoUnit && ch->desc.unit != units::kNoUnit) {
                    Result<units::Converter> c = units->converter(ch->desc.unit, unit);
                    if (!c) {
                        return Failure{c.error()};
                    }
                    conv = *c;
                }
                return maths::MathInputGatherer(ch->series, r, 0, ch->desc.interp, conv, CancelToken{});
            };
            Result<analysis::UniformSignal> out = maths::evaluateWhole(*plan, ctx);
            std::printf("%s whole %-22s %s\n", label, item.name.c_str(),
                        out ? digestOf(*out).c_str() : ("error: " + out.error().toString()).c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}

json row(const Stats& s, std::optional<double> limit) {
    json r{{"n", s.n}, {"minMs", s.min}, {"medianMs", s.median}, {"maxMs", s.max}};
    if (limit) {
        r["limitMs"] = *limit;
        r["pass"] = s.median <= *limit;
    } else {
        r["limitMs"] = nullptr;
        r["pass"] = true;
    }
    return r;
}

void report(json& rows, const std::string& id, const Stats& s, std::optional<double> limit) {
    const json r = row(s, limit);
    std::printf("%-34s median %8.3f ms (min %.3f, max %.3f, n %zu)%s\n", id.c_str(), s.median, s.min, s.max,
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
    bool nodes = false;
    bool digest = false;
    unsigned poolWorkers = 0; // 0: the TaskPool default
    std::string jsonPath;
    if (const auto e = getEnv("ESSES_BENCH_OUT"); e && !e->empty()) {
        jsonPath = pathToUtf8(pathFromUtf8(*e) / "maths_bench.json");
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
        } else if (a == "--nodes") {
            nodes = true;
        } else if (a == "--digest") {
            digest = true;
        } else if (a == "--workers") {
            poolWorkers = static_cast<unsigned>(std::max(1, std::atoi(next().c_str())));
        } else if (a == "--json") {
            jsonPath = next();
        } else {
            std::fprintf(
                stderr,
                "usage: esses-maths-bench [--runs N] [--smoke] [--gate] [--nodes] [--workers N] [--digest] "
                "[--json file]\n");
            return 2;
        }
    }
    if (smoke) {
        runs = 1;
    }
    auto unitsLoaded =
        units::UnitRegistry::loadFiles(pathFromUtf8(ESSES_BENCH_SOURCE_DIR) / "data" / "units.json");
    if (!unitsLoaded) {
        std::fprintf(stderr, "maths_bench: %s\n", unitsLoaded.error().toString().c_str());
        return 2;
    }
    const auto units = std::make_shared<const units::UnitRegistry>(std::move(unitsLoaded).value());
    const units::UnitId mps = units->find("m/s").value_or(units::kNoUnit);
    if (digest) {
        return printDigests(units);
    }

    // 2.16 M samples (36 minutes at 1 kHz) of a smooth speed in m/s.
    const std::size_t samples = smoke ? 100'000 : 2'160'000;
    std::vector<float> values(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        values[i] = static_cast<float>(40.0 + 30.0 * std::sin(static_cast<double>(i) * 2e-4));
    }
    store::ChunkCache cache(std::size_t{1} << 30);
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("maths-bench");
    model::DataLayer layer;
    layer.duration = static_cast<Micros>(samples) * 1000;
    model::Channel speed;
    speed.desc.name = "Ground Speed";
    speed.desc.loggedName = "Ground Speed";
    speed.desc.unit = mps;
    speed.desc.quantity = units->quantityOf(mps);
    speed.desc.rate = Rate::fromHz(1000);
    store::ColumnOptions columnOptions;
    columnOptions.cache = &cache;
    speed.series = store::UniformSeries{
        Rate::fromHz(1000), 0, store::Column(store::InMemoryProvider::fromF32(values), columnOptions)};
    layer.addChannel(std::move(speed));
    src->layers.push_back(std::move(layer));
    const BenchScope scope(*src, *units);

    const std::string expr = "'Ground Speed'[km/h] * 2 + 1";
    maths::Diagnostics diags;
    Result<maths::Ast> ast = maths::Parser::parse(expr, {}, diags);
    Result<maths::Plan> plan = ast ? maths::compile(*ast, scope, maths::MathItem{}, {}, diags)
                                   : Result<maths::Plan>(Failure{ast.error()});
    if (!plan) {
        std::fprintf(stderr, "maths_bench: %s\n", plan.error().toString().c_str());
        return 2;
    }
    maths::EvalContext ctx;
    ctx.source = src;
    ctx.layer = "Normal";
    ctx.resultRate = Rate::fromHz(1000);
    ctx.resultCount = samples;
    ctx.units = units;
    ctx.getChannel = [src, units](const maths::ChannelKey& key, units::UnitId unit,
                                  Rate r) -> Result<maths::MathInputGatherer> {
        const model::Channel* ch = src->layers.front().find(key.nameCf);
        if (ch == nullptr) {
            return fail(ErrorCode::NotFound, "no channel " + key.nameCf);
        }
        Result<units::Converter> conv = units->converter(ch->desc.unit, unit);
        if (!conv) {
            return Failure{conv.error()};
        }
        return maths::MathInputGatherer(ch->series, r, 0, ch->desc.interp, *conv, CancelToken{});
    };

    // The stage's input, gathered once at the result rate in km/h.
    std::vector<double> input(samples);
    std::vector<std::uint8_t> inputBits((samples + 7) / 8);
    std::vector<double> output(samples);
    std::vector<std::uint8_t> outputBits((samples + 7) / 8);
    {
        Result<maths::MathInputGatherer> g =
            ctx.getChannel(plan->inputs[0].key, plan->inputs[0].unit, Rate::fromHz(1000));
        const maths::BlockView into{input.data(), inputBits.data(), store::Dtype::F64,
                                    static_cast<std::uint32_t>(samples)};
        if (!g || !g->fill(0, static_cast<std::uint32_t>(samples), into)) {
            std::fprintf(stderr, "maths_bench: cannot gather the input\n");
            return 2;
        }
    }
    maths::Kernel& stage = *plan->stages.back().kernel;

    std::vector<double> stageMs;
    std::vector<double> evalMs;
    std::vector<double> parseMs;
    // A long realistic item: eight nested choose() with references, literals and calls (about 70 nodes).
    std::string nested = "'Ground Speed'[km/h]";
    for (int k = 0; k < 8; ++k) {
        std::string next = "choose('Ground Speed'[km/h] > ";
        next += std::to_string(k * 10);
        next += ", abs(";
        next += nested;
        next += ") / 3.6, 0) * 1.5";
        nested = std::move(next);
    }
    for (int r = -1; r < runs; ++r) { // r == -1: untimed warm-up (chunks resident, pages touched)
        const auto t0 = std::chrono::steady_clock::now();
        Result<analysis::UniformSignal> out = maths::evaluateWhole(*plan, ctx);
        const auto t1 = std::chrono::steady_clock::now();
        if (!out || out->values.size() != samples) {
            std::fprintf(stderr, "maths_bench: evaluation failed\n");
            return 2;
        }
        maths::BlockCtx b;
        b.n = static_cast<std::uint32_t>(samples);
        b.rate = Rate::fromHz(1000);
        const maths::BlockView in{input.data(), inputBits.data(), store::Dtype::F64,
                                  static_cast<std::uint32_t>(samples)};
        const maths::BlockView outView{output.data(), outputBits.data(), store::Dtype::F64,
                                       static_cast<std::uint32_t>(samples)};
        stage.reset(ctx);
        const auto t2 = std::chrono::steady_clock::now();
        stage.run(b, std::span<const maths::BlockView>(&in, 1), outView);
        const auto t3 = std::chrono::steady_clock::now();
        const auto t4 = std::chrono::steady_clock::now();
        maths::Diagnostics d;
        Result<maths::Ast> nestedAst = maths::Parser::parse(nested, {}, d);
        if (nestedAst) {
            (void)maths::TypeChecker::check(*nestedAst, scope, d);
        }
        const auto t5 = std::chrono::steady_clock::now();
        if (!nestedAst) {
            std::fprintf(stderr, "maths_bench: the nested expression does not parse\n");
            return 2;
        }
        if (r < 0) {
            continue;
        }
        evalMs.push_back(ms(t1 - t0));
        stageMs.push_back(ms(t3 - t2));
        parseMs.push_back(ms(t5 - t4));
    }
    json rows = json::object();
    const std::string size = smoke ? "100k" : "2160k";
    report(rows, "maths.elementwise_stage_" + size, statsOf(stageMs), kStageLimitMs);
    report(rows, "maths.elementwise_eval_" + size, statsOf(evalMs), std::nullopt);
    report(rows, "maths.parse_check", statsOf(parseMs), kParseLimitMs);
    std::printf("maths_bench: %zu samples at 1 kHz, expression %s, one core, %d run(s) after a warm-up\n",
                samples, expr.c_str(), runs);

    // AC-M2-10: the 20 maths channels on RW-2, parallel across nodes.
    const unsigned cores = std::thread::hardware_concurrency();
    std::size_t workers = 0;
    bool readinessOk = true;
    {
        // The cache is declared before the pool so that it is destroyed after it: ~TaskPool joins the workers
        // first. A worker still holds the shared state of the task it just ran, and through it the result's
        // Column, for a moment after the requester's Future is ready (it publishes, wakes the requester, then
        // runs the continuations and completes the task); destroying the cache in that window trips its
        // "destroyed while Columns still use it" assertion.
        store::ChunkCache rwCache(std::size_t{4} << 30);
        TaskPool pool(poolWorkers > 0 ? poolWorkers : TaskPool::defaultWorkerCount());
        workers = pool.stats().workers;
        const Micros rwDuration = smoke ? 60'000'000 : 1'800'000'000; // 30 min (bench/workloads.md RW-2)
        const std::shared_ptr<const model::DataSource> rw2 = makeRw2(rwCache, *units, rwDuration);
        const Readiness ready = measureReadiness(rw2, units, pool, rwCache, runs);
        if (nodes) {
            printNodeTimes(rw2, units, pool, rwCache);
        }
        if (!ready.error.empty()) {
            std::fprintf(stderr, "maths_bench: the readiness set cannot be measured: %s\n",
                         ready.error.c_str());
            rows["maths.rw2_20_ready"] =
                json{{"n", 0}, {"limitMs", kReadyLimitMs}, {"pass", false}, {"error", ready.error}};
            readinessOk = false;
        } else {
            report(rows, "maths.rw2_20_ready", statsOf(ready.readyMs), kReadyLimitMs);
            report(rows, "maths.rw2_20_define", statsOf(ready.defineMs), std::nullopt);
        }
        rows["maths.rw2_20_ready"]["cores"] = cores;
        rows["maths.rw2_20_ready"]["workers"] = workers;
        std::printf("maths_bench: RW-2 %s, 20 maths channels, %u logical processors, %zu pool workers\n",
                    smoke ? "shortened to 60 s" : "30 min", cores, workers);
    }
    {
        store::ChunkCache filterCache(std::size_t{2} << 30);
        const std::size_t filterSamples = smoke ? 216'000 : 21'600'000;
        Result<std::vector<double>> filterMs =
            measureFilterLp(units, filterCache, filterSamples, smoke ? 1 : 5);
        const std::string id =
            smoke ? "maths.filter_lp_zero_phase_216k" : "maths.filter_lp_zero_phase_21600k";
        if (filterMs) {
            report(rows, id, statsOf(*filterMs), std::nullopt);
        } else {
            std::fprintf(stderr, "maths_bench: filter_lp: %s\n", filterMs.error().toString().c_str());
            rows[id] =
                json{{"n", 0}, {"limitMs", nullptr}, {"pass", true}, {"error", filterMs.error().message}};
        }
    }
    {
        const std::vector<double> rebuildMs = measureMarkerRebuild(runs);
        if (rebuildMs.empty()) {
            std::fprintf(stderr, "maths_bench: the marker rebuild produced no snapshot\n");
            rows["analysis.marker_rebuild_60lap"] =
                json{{"n", 0}, {"limitMs", kMarkerRebuildLimitMs}, {"pass", false}, {"error", "no snapshot"}};
        } else {
            report(rows, "analysis.marker_rebuild_60lap", statsOf(rebuildMs), kMarkerRebuildLimitMs);
        }
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
        const json doc{{"bench", "maths_bench"},
                       {"ac", json::array({"AC-M2-14", "AC-M2-10", "AC-M2-05"})},
                       {"threads", 1},
                       {"cores", cores},
                       {"workers", workers},
                       {"samples", samples},
                       {"expression", expr},
                       {"rows", rows},
                       {"benchmarks", benchmarks}};
        const fs::path out = pathFromUtf8(jsonPath);
        if (out.has_parent_path()) {
            fs::create_directories(out.parent_path());
        }
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << doc.dump(2) << "\n";
        if (!f) {
            std::fprintf(stderr, "maths_bench: cannot write %s\n", jsonPath.c_str());
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
    if (!readinessOk && smoke) {
        return 1; // the smoke run checks that the readiness set evaluates
    }
    return 0;
}
