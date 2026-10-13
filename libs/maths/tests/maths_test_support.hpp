// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the esses_maths tests: the shipped unit table, a private ChunkCache and TaskPool per
// test (declared in that order so the pool joins before the cache goes), synthetic sources with in-memory
// uniform channels, a worker gate for priority tests, and FakeModule: a built-in module whose outputs
// exercise the graph without esses_analysis (Fake Speed = gain x "Speed In" in m/s, Fake Dist = the rectangle
// integral of Fake Speed as stored, Fake Other = "Other In").
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/maths.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esses::maths::test {

inline std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

inline std::shared_ptr<const units::UnitRegistry> shippedUnits() {
    static const std::shared_ptr<const units::UnitRegistry> registry = [] {
        Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        if (!r) {
            throw std::runtime_error("cannot load data/units.json: " + r.error().toString());
        }
        return std::make_shared<const units::UnitRegistry>(std::move(r).value());
    }();
    return registry;
}

inline units::UnitId unitOf(std::string_view symbol) {
    const std::optional<units::UnitId> u = shippedUnits()->find(symbol);
    if (!u) {
        throw std::runtime_error("unknown unit " + std::string(symbol));
    }
    return *u;
}

// A ChunkCache and a TaskPool owned by one test; the pool is joined before the cache is destroyed.
struct Runtime {
    store::ChunkCache cache{std::size_t{512} << 20};
    TaskPool pool;
    explicit Runtime(unsigned workers = 4) : pool(workers) {}
};

inline model::Channel uniformF32(store::ChunkCache& cache, const std::string& name, units::UnitId unit,
                                 Rate rate, const std::vector<float>& values, std::vector<bool> valid = {}) {
    model::Channel ch;
    ch.desc.name = name;
    ch.desc.loggedName = name;
    ch.desc.unit = unit;
    ch.desc.quantity = shippedUnits()->quantityOf(unit);
    ch.desc.dtype = store::Dtype::F32;
    ch.desc.rate = rate;
    store::ColumnOptions options;
    options.cache = &cache;
    ch.series = store::UniformSeries{
        rate, 0,
        store::Column(store::InMemoryProvider::fromF32(values, std::move(valid), "memory:" + name), options)};
    return ch;
}

inline std::shared_ptr<model::DataSource> makeSource(std::vector<model::Channel> channels, Micros duration,
                                                     std::string_view tag = "maths-test-source") {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(tag);
    model::DataLayer layer;
    layer.duration = duration;
    for (model::Channel& c : channels) {
        c.desc.id = model::channelIdLogged(src->id, "Normal", c.desc.loggedName, "test");
        layer.addChannel(std::move(c));
    }
    src->layers.push_back(std::move(layer));
    return src;
}

// Blocks `n` workers of a pool until release(), so submitted tasks stay queued in their bands. The tasks
// share the gate's state, so the gate may go away while they are still waking up.
class WorkerGate {
public:
    WorkerGate(TaskPool& pool, unsigned n) : state_(std::make_shared<State>(n)) {
        for (unsigned i = 0; i < n; ++i) {
            pool.run(Priority::Interactive, TaskKey{}, CancelToken{},
                     [state = state_, release = state_->release](CancelToken) {
                         state->started.count_down();
                         release.wait();
                     });
        }
        state_->started.wait();
    }
    ~WorkerGate() { release(); }
    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;
    void release() {
        if (!released_) {
            released_ = true;
            state_->promise.set_value();
        }
    }

private:
    struct State {
        explicit State(unsigned n) : started(n), release(promise.get_future().share()) {}
        std::latch started;
        std::promise<void> promise;
        std::shared_future<void> release;
    };
    std::shared_ptr<State> state_;
    bool released_ = false;
};

// A built-in module for graph tests. Settings: {"gain": number, "warn": bool, "fail": bool, "shape": string}
// (shape "duplicate", "unknownUpstream", "cycle" or "noEvaluate" produce invalid output sets).
class FakeModule final : public IBuiltinModule {
public:
    explicit FakeModule(std::string id = "test.fake", std::string speedName = "Fake Speed")
        : id_(std::move(id)), speedName_(std::move(speedName)) {}

    std::shared_ptr<std::atomic<int>> speedEvals = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> distEvals = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> otherEvals = std::make_shared<std::atomic<int>>(0);

    std::string id() const override { return id_; }
    int version() const override { return 1; }
    std::string settingsSchemaId() const override { return "builtin-" + id_ + "-1"; }
    const model::OpaqueJson& settingsSchema() const override { return schema_; }
    model::OpaqueJson defaultSettings() const override { return json(R"({"gain": 2})"); }

    static model::OpaqueJson json(std::string_view text) {
        Result<model::OpaqueJson> r = model::OpaqueJson::parse(text);
        if (!r) {
            throw std::runtime_error("bad test JSON");
        }
        return std::move(r).value();
    }

    Result<std::vector<BuiltinOutput>> expand(const model::OpaqueJson& settings,
                                              const units::UnitRegistry& units,
                                              Diagnostics& /*diags*/) const override {
        const nlohmann::json s = nlohmann::json::parse(settings.compact());
        const double gain = s.value("gain", 1.0);
        const bool warn = s.value("warn", false);
        const bool failEval = s.value("fail", false);
        const std::string shape = s.value("shape", std::string());
        const units::QuantityId speedQ = *units.quantityByName("Speed");
        const units::QuantityId distQ = *units.quantityByName("Distance");

        BuiltinOutput speed;
        speed.name = speedName_;
        speed.inputs = {"Speed In"};
        speed.quantity = speedQ;
        speed.resultUnit = *units.find("m/s", speedQ);
        speed.dtype = store::Dtype::F32;
        speed.evaluate = [gain, warn, failEval, counter = speedEvals](
                             const EvalContext& ctx, std::span<const analysis::UniformSignal>,
                             Diagnostics& d) -> Result<analysis::UniformSignal> {
            counter->fetch_add(1);
            const model::DataLayer* layer = ctx.source != nullptr ? ctx.source->layer(ctx.layer) : nullptr;
            const model::Channel* in = layer != nullptr ? layer->find("Speed In") : nullptr;
            if (in == nullptr || failEval) {
                ErrorContext c;
                c.detail = "E-FAKE-NO-INPUT";
                return fail(ErrorCode::NotFound, "no Speed In", std::move(c));
            }
            Result<analysis::UniformSignal> sig =
                readWholeSignal(in->series, ctx.cancel, ctx.units->toBase(in->desc.unit));
            if (!sig) {
                return sig;
            }
            for (double& v : sig->values) {
                v *= gain;
            }
            if (warn) {
                d.push_back(Diagnostic{"W-FAKE", Diagnostic::Warning, {}, "fake warning", {}});
                d.push_back(Diagnostic{"W-FAKE-2", Diagnostic::Warning, {}, "second warning", {}});
            }
            d.push_back(Diagnostic{"I-FAKE", Diagnostic::Info, {}, "fake info", {}});
            return sig;
        };

        BuiltinOutput dist;
        dist.name = "Fake Dist";
        dist.upstream = {shape == "unknownUpstream" ? std::string("Nope") : speedName_};
        dist.quantity = distQ;
        dist.resultUnit = *units.find("m", distQ);
        dist.dtype = store::Dtype::F64;
        dist.evaluate = [counter = distEvals](const EvalContext&, std::span<const analysis::UniformSignal> up,
                                              Diagnostics&) -> Result<analysis::UniformSignal> {
            counter->fetch_add(1);
            analysis::UniformSignal out;
            out.rate = up[0].rate;
            out.t0 = up[0].t0;
            out.values.resize(up[0].size());
            const double dt = 1.0 / up[0].rate.hz();
            double acc = 0.0;
            for (std::size_t i = 0; i < up[0].size(); ++i) {
                out.values[i] = acc;
                if (up[0].validAt(i)) {
                    acc += up[0].values[i] * dt;
                }
            }
            return out;
        };
        if (shape == "noEvaluate") {
            dist.evaluate = nullptr;
        }

        BuiltinOutput other;
        other.name = shape == "duplicate" ? std::string("FAKE dist") : std::string("Fake Other");
        other.inputs = {"Other In"};
        other.upstream =
            shape == "cycle" ? std::vector<std::string>{"Fake Dist"} : std::vector<std::string>{};
        if (shape == "cycle") {
            speed.upstream = {"Fake Other"};
        }
        other.dtype = store::Dtype::F32;
        other.evaluate = [counter = otherEvals](const EvalContext& ctx,
                                                std::span<const analysis::UniformSignal>,
                                                Diagnostics&) -> Result<analysis::UniformSignal> {
            counter->fetch_add(1);
            const model::Channel* in = ctx.source->layer(ctx.layer)->find("Other In");
            if (in == nullptr) {
                return fail(ErrorCode::NotFound, "no Other In");
            }
            return readWholeSignal(in->series, ctx.cancel);
        };

        std::vector<BuiltinOutput> out;
        out.push_back(std::move(speed));
        out.push_back(std::move(dist));
        out.push_back(std::move(other));
        return out;
    }

private:
    std::string id_;
    std::string speedName_;
    model::OpaqueJson schema_;
};

} // namespace esses::maths::test
