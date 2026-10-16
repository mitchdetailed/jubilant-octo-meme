// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph on the 0.1 whole-node path (03 sections 6.0 and 7; D-016, D-139, D-140) with FakeModule, so the
// graph is tested without esses_analysis: definition and descriptors, require through PlanProvider into the
// ChunkCache, version-keyed caching and invalidation (settings, columns read, module enable; never
// DataSource::version), status transitions, error propagation, cancellation, the Background prefetch hook
// and the share-key coalescing of upstream evaluations. AC-M1-12 runs this suite under TSan.

#include "maths_test_support.hpp"

#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::FakeModule;

namespace {

constexpr std::size_t kSamples = 140'000; // three chunks at 1 kHz

std::vector<float> speedKmh() {
    std::vector<float> v(kSamples);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<float>(100.0 + 50.0 * std::sin(static_cast<double>(i) * 1e-3));
    }
    return v;
}

std::shared_ptr<model::DataSource> fakeSource(store::ChunkCache& cache, bool withSpeed = true) {
    std::vector<model::Channel> channels;
    if (withSpeed) {
        channels.push_back(
            test::uniformF32(cache, "Speed In", test::unitOf("km/h"), Rate::fromHz(1000), speedKmh()));
    }
    channels.push_back(
        test::uniformF32(cache, "Other In", units::kNoUnit, Rate::fromHz(10), {1.F, 2.F, 3.F}));
    return test::makeSource(std::move(channels), static_cast<Micros>(kSamples) * 1000);
}

MathGraph makeGraph(test::Runtime& rt, std::shared_ptr<const model::DataSource> src) {
    return MathGraph(std::move(src), "", test::shippedUnits(), {}, &rt.pool, &rt.cache);
}

Result<ColumnHandle> await(MathGraph& g, const ChannelKey& key, Priority prio = Priority::Visible) {
    Future<ColumnHandle> f = g.require(key, SampleRange::all(), prio, {});
    Result<ColumnHandle> r = f.get();
    static_cast<void>(g.applyReadyCompletions());
    return r;
}

std::vector<double> valuesOf(const ColumnHandle& h) {
    Result<analysis::UniformSignal> s = readWholeSignal(h.series, {});
    REQUIRE(s);
    return s->values;
}

} // namespace

TEST_CASE("math graph: defineBuiltin creates Pending nodes with maths descriptors", "[unit][graph]") {
    test::Runtime rt;
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    CHECK(g.layer() == "Normal");
    FakeModule module;
    Diagnostics diags;
    Result<std::vector<ChannelKey>> keys = g.defineBuiltin(module, {}, diags);
    REQUIRE(keys);
    REQUIRE(keys->size() == 3);
    const ChannelKey speed = (*keys)[0];
    const ChannelKey dist = (*keys)[1];
    CHECK(speed == ChannelKey::make(src->id, "Normal", "fake speed"));
    CHECK(g.outputs() == *keys);
    CHECK(g.find("FAKE DIST") == dist);
    CHECK_FALSE(g.find("Speed In").has_value());
    CHECK(g.consumersOf(speed) == std::vector<ChannelKey>{dist});
    CHECK(g.consumersOf(dist).empty());

    const MathStatus st = g.status(speed);
    CHECK(st.state == MathState::Pending);
    CHECK(st.summary == "Pending");
    CHECK(st.version == g.version(speed));
    CHECK(g.version(speed) != Version{});
    CHECK(g.version(speed) != g.version(dist));

    const std::optional<model::ChannelDescriptor> d = g.descriptor(dist);
    REQUIRE(d);
    CHECK(d->name == "Fake Dist");
    CHECK(d->id == model::channelIdMathsModule(src->id, "Normal", "Fake Dist", "test.fake"));
    CHECK(d->source == model::ChannelSource::Maths);
    CHECK(d->unit == test::unitOf("m"));
    CHECK(d->dtype == store::Dtype::F64);
    CHECK(d->statusText == "Pending");
    CHECK_FALSE(d->rate.has_value());

    const MathStatus unknown = g.status(ChannelKey::make(src->id, "Normal", "Speed In"));
    CHECK(unknown.state == MathState::Error);
    REQUIRE(unknown.diags.size() == 1);
    CHECK(unknown.diags[0].code == "E-NOT-MATHS");
    CHECK(g.version(ChannelKey::make(src->id, "Normal", "nope")) == Version{});
    CHECK(g.require(ChannelKey::make(src->id, "Normal", "nope"), SampleRange::all(), Priority::Visible, {})
              .get()
              .error()
              .code == ErrorCode::NotFound);
}

TEST_CASE("math graph: require evaluates through PlanProvider and caches by version", "[unit][graph]") {
    test::Runtime rt;
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const ChannelKey speed = *g.find("Fake Speed");
    const ChannelKey dist = *g.find("Fake Dist");

    // Requiring the downstream output schedules its upstream too; each evaluates once.
    Future<ColumnHandle> f = g.require(dist, SampleRange::all(), Priority::Visible, {});
    CHECK((g.status(dist).state == MathState::Evaluating || f.ready()));
    const Result<ColumnHandle>& r = f.get();
    REQUIRE(r);
    const std::vector<ChannelKey> applied = g.applyReadyCompletions();
    CHECK(applied.size() == 2);
    CHECK(module.speedEvals->load() == 1);
    CHECK(module.distEvals->load() == 1);
    CHECK(g.status(dist).state == MathState::Ok);
    CHECK(g.status(dist).summary == "OK");
    CHECK(g.status(speed).state == MathState::Ok);
    CHECK(g.status(speed).progress == 1.0F);
    REQUIRE(g.status(speed).diags.size() == 1);
    CHECK(g.status(speed).diags[0].code == "I-FAKE");

    // The handle: descriptor with rate, a uniform series over a Column whose chunks are in the cache.
    CHECK(r->key == dist);
    CHECK(r->version == g.version(dist));
    CHECK(r->descriptor.rate == Rate::fromHz(1000));
    CHECK(r->descriptor.statusText.empty());
    const auto* series = std::get_if<store::UniformSeries>(&r->series);
    REQUIRE(series != nullptr);
    CHECK(series->size() == static_cast<SampleIdx>(kSamples));
    CHECK(series->values.dtype() == store::Dtype::F64);
    for (SampleIdx c = 0; c < series->values.chunkCount(); ++c) {
        CHECK(series->values.isResident(c));
    }
    CHECK(series->values.summary().complete());
    CHECK(g.descriptor(dist)->rate == Rate::fromHz(1000));
    CHECK(g.descriptor(dist)->statusText.empty());

    // Values: Fake Speed = 2 x km/h in m/s (stored F32); Fake Dist integrates the stored Fake Speed.
    Result<ColumnHandle> s = await(g, speed);
    REQUIRE(s);
    const std::vector<double> sv = valuesOf(*s);
    const std::vector<float> kmh = speedKmh();
    const units::Converter toMps = test::shippedUnits()->toBase(test::unitOf("km/h"));
    CHECK(sv[1234] == static_cast<double>(static_cast<float>(toMps(static_cast<double>(kmh[1234])) * 2.0)));
    CHECK(std::abs(sv[1234] - 2.0 * static_cast<double>(kmh[1234]) / 3.6) < 1e-4);
    const std::vector<double> dv = valuesOf(*r);
    double acc = 0.0;
    for (std::size_t i = 0; i < 2000; ++i) {
        CHECK(dv[i] == acc);
        acc += sv[i] * 1e-3;
    }

    // Cached: the same version comes back ready, without evaluating again.
    for (int i = 0; i < 3; ++i) {
        Future<ColumnHandle> again = g.require(dist, SampleRange::all(), Priority::Interactive, {});
        CHECK(again.ready());
        REQUIRE(again.get());
        CHECK(std::get<store::UniformSeries>(again.get()->series).values == series->values);
    }
    CHECK(module.speedEvals->load() == 1);
    CHECK(module.distEvals->load() == 1);
    CHECK(rt.pool.stats().queued[0] + rt.pool.stats().queued[1] + rt.pool.stats().queued[2] == 0);
}

TEST_CASE("math graph: versions follow settings, inputs and module state, never DataSource::version",
          "[unit][graph]") {
    test::Runtime rt;
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const ChannelKey speed = *g.find("Fake Speed");
    const ChannelKey dist = *g.find("Fake Dist");
    const ChannelKey other = *g.find("Fake Other");
    REQUIRE(await(g, dist));
    REQUIRE(await(g, other));
    const Version v0 = g.version(speed);
    const Version d0 = g.version(dist);
    const Version o0 = g.version(other);
    const std::vector<double> before = valuesOf(*await(g, dist));

    SECTION("a sidecar commit that shares the columns invalidates nothing") {
        auto next = std::make_shared<model::DataSource>(*src);
        next->version += 1;
        next->details.push_back(model::Detail{});
        next->layers[0].ranges = model::RangeGroupTree{};
        CHECK(g.setSource(next).empty());
        CHECK(g.version(speed) == v0);
        CHECK(g.status(dist).state == MathState::Ok);
        CHECK(g.require(dist, SampleRange::all(), Priority::Visible, {}).ready());
        CHECK(module.distEvals->load() == 1);
    }
    SECTION("re-defining with the same settings keeps the evaluated columns") {
        REQUIRE(g.defineBuiltin(module, FakeModule::json(R"({"gain":2})"), diags));
        CHECK(g.version(speed) == v0);
        CHECK(g.status(speed).state == MathState::Ok);
        CHECK(g.require(dist, SampleRange::all(), Priority::Visible, {}).ready());
    }
    SECTION("new settings re-version the module's outputs and their consumers") {
        REQUIRE(g.defineBuiltin(module, FakeModule::json(R"({"gain":3})"), diags));
        CHECK(g.version(speed) != v0);
        CHECK(g.version(dist) != d0);
        CHECK(g.status(speed).state == MathState::Pending);
        CHECK(g.status(dist).state == MathState::Pending);
        CHECK(g.descriptor(dist)->statusText == "Pending");
        Result<ColumnHandle> r = await(g, dist);
        REQUIRE(r);
        CHECK(module.speedEvals->load() == 2);
        CHECK(module.distEvals->load() == 2);
        CHECK(std::abs(valuesOf(*r)[1000] - 1.5 * before[1000]) <= 1e-6 * before[1000]);
        // Back to the first settings: the same version as before and a bit-identical output (03 6.7).
        REQUIRE(g.defineBuiltin(module, FakeModule::json(R"({"gain":2})"), diags));
        CHECK(g.version(dist) == d0);
        Result<ColumnHandle> back = await(g, dist);
        REQUIRE(back);
        CHECK(valuesOf(*back) == before);
    }
    SECTION("a new column for an input re-versions only the nodes that read it") {
        auto next = std::make_shared<model::DataSource>(*src);
        std::vector<float> faster = speedKmh();
        for (float& v : faster) {
            v += 10.F;
        }
        next->layers[0].findMutable("Speed In")->series =
            test::uniformF32(rt.cache, "Speed In", test::unitOf("km/h"), Rate::fromHz(1000), faster).series;
        const std::vector<ChannelKey> changed = g.setSource(next);
        CHECK(changed == std::vector<ChannelKey>{speed, dist});
        CHECK(g.version(other) == o0);
        CHECK(g.status(other).state == MathState::Ok);
        CHECK(g.status(speed).state == MathState::Pending);
        Result<ColumnHandle> r = await(g, dist);
        REQUIRE(r);
        CHECK(valuesOf(*r)[5000] > before[5000]);
        CHECK(module.otherEvals->load() == 1);
    }
    SECTION("a store version bump of an input column re-versions its readers") {
        const auto& in = std::get<store::UniformSeries>(src->layers[0].find("Speed In")->series);
        const std::uint64_t columnVersion = in.values.version();
        // The InMemoryProvider of the test column: bump through a fresh source sharing the same columns.
        auto next = std::make_shared<model::DataSource>(*src);
        auto provider = store::InMemoryProvider::fromF32(speedKmh(), {}, "memory:bumped");
        provider->bumpVersion();
        store::ColumnOptions options;
        options.cache = &rt.cache;
        next->layers[0].findMutable("Speed In")->series =
            store::UniformSeries{Rate::fromHz(1000), 0, store::Column(provider, options)};
        CHECK(std::get<store::UniformSeries>(next->layers[0].find("Speed In")->series).values.version() !=
              columnVersion);
        CHECK(g.setSource(next).size() == 2);
    }
    SECTION("an input that disappears or appears changes the version") {
        auto without = fakeSource(rt.cache, false);
        CHECK(g.setSource(without).size() == 3); // and a new "Other In" column
        CHECK(g.version(other) != o0);
        Result<ColumnHandle> r = await(g, dist);
        REQUIRE_FALSE(r);
        CHECK(g.status(speed).state == MathState::Error);
        REQUIRE_FALSE(g.status(speed).diags.empty());
        CHECK(g.status(speed).diags[0].code == "E-FAKE-NO-INPUT");
        CHECK(g.status(dist).state == MathState::Error);
        CHECK(g.status(dist).diags[0].code == "E-FAKE-NO-INPUT"); // propagated from the upstream
        CHECK_THAT(g.status(dist).summary, Catch::Matchers::StartsWith("input 'Fake Speed': "));
        CHECK_THAT(g.descriptor(dist)->statusText, Catch::Matchers::StartsWith("Error: input 'Fake Speed'"));
        // A deterministic failure is not retried until something changes.
        const int evals = module.speedEvals->load();
        CHECK(g.require(dist, SampleRange::all(), Priority::Visible, {}).get().error().code ==
              ErrorCode::NotFound);
        CHECK(module.speedEvals->load() == evals);
        CHECK(g.setSource(src).size() == 3);
        CHECK(await(g, dist));
    }
    SECTION("disabling the module disables its outputs; enabling brings them back") {
        CHECK(g.setModuleEnabled("test.fake", false).size() == 3);
        CHECK(g.status(speed).state == MathState::Disabled);
        CHECK(g.status(speed).summary == "Disabled");
        CHECK(g.descriptor(speed)->statusText == "Error: Disabled");
        CHECK(g.require(speed, SampleRange::all(), Priority::Visible, {}).get().error().code ==
              ErrorCode::Invalid);
        CHECK(g.prefetch().empty());
        CHECK(g.setModuleEnabled("other.module", false).empty());
        CHECK(g.setModuleEnabled("test.fake", true).size() == 3);
        CHECK(g.version(speed) == v0);
        CHECK(g.status(speed).state == MathState::Pending);
        CHECK(await(g, speed));
    }
}

TEST_CASE("math graph: stale and cancelled completions", "[unit][graph]") {
    test::Runtime rt(2);
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const ChannelKey speed = *g.find("Fake Speed");

    SECTION("a completion for an older version is ignored") {
        const Version v0 = g.version(speed);
        Future<ColumnHandle> f = g.require(speed, SampleRange::all(), Priority::Visible, {});
        Result<ColumnHandle> r = f.get();
        REQUIRE(r);
        REQUIRE(g.defineBuiltin(module, FakeModule::json(R"({"gain":5})"), diags));
        g.applyCompletion(speed, v0, r);
        CHECK(g.status(speed).state == MathState::Pending);
        CHECK(g.applyReadyCompletions().empty()); // the evaluation of v0 was dropped with its version
    }
    SECTION("a cancelled evaluation leaves the node Pending and a new require evaluates") {
        CancelSource cancel;
        {
            test::WorkerGate gate(rt.pool, 2);
            Future<ColumnHandle> f = g.require(speed, SampleRange::all(), Priority::Visible, cancel.token());
            CHECK(g.status(speed).state == MathState::Evaluating);
            CHECK(g.status(speed).summary == "Calculating");
            cancel.cancel();
            gate.release();
            CHECK(f.get().error().code == ErrorCode::Cancelled);
        }
        CHECK(g.applyReadyCompletions() == std::vector<ChannelKey>{speed});
        CHECK(g.status(speed).state == MathState::Pending);
        Result<ColumnHandle> r = await(g, speed);
        CHECK(r);
        CHECK(g.status(speed).state == MathState::Ok);
    }
    SECTION("warnings make the status Warning with the first message") {
        REQUIRE(g.defineBuiltin(module, FakeModule::json(R"({"gain":2,"warn":true})"), diags));
        REQUIRE(await(g, speed));
        const MathStatus st = g.status(speed);
        CHECK(st.state == MathState::Warning);
        CHECK(st.summary == "fake warning (+1 more)");
        CHECK(st.diags.size() == 3);
        CHECK(toChannelStatus(st.state) == model::ChannelStatus::Ok);
        CHECK(g.descriptor(speed)->statusText.empty());
    }
    SECTION("a host that posts the completion itself") {
        const Version v = g.version(speed);
        Future<ColumnHandle> f = g.require(speed, SampleRange::all(), Priority::Visible, {});
        g.applyCompletion(speed, v, f.get());
        CHECK(g.status(speed).state == MathState::Ok);
        CHECK(g.applyReadyCompletions().empty());
    }
}

TEST_CASE("math graph: the Background prefetch hook and share-key coalescing", "[unit][graph]") {
    test::Runtime rt(2);
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const ChannelKey speed = *g.find("Fake Speed");
    const ChannelKey dist = *g.find("Fake Dist");

    std::vector<MathRequest> requests;
    {
        test::WorkerGate gate(rt.pool, 2);
        requests = g.prefetch();
        REQUIRE(requests.size() == 3);
        CHECK(requests[0].key == speed);
        CHECK(requests[0].version == g.version(speed));
        // One task per node, queued in the Background band (D-016); none ran yet.
        TaskPool::Stats st = rt.pool.stats();
        CHECK(st.queued[static_cast<std::size_t>(Priority::Background)] == 3);
        CHECK(st.queued[static_cast<std::size_t>(Priority::Visible)] == 0);
        for (const ChannelKey& k : g.outputs()) {
            CHECK(g.status(k).state == MathState::Evaluating);
        }
        // A visible request joins the queued tasks (same share key) and raises them with their upstream.
        Future<ColumnHandle> visible = g.require(dist, SampleRange::all(), Priority::Visible, {});
        CHECK(visible.sharesStateWith(requests[1].future));
        st = rt.pool.stats();
        CHECK(st.queued[static_cast<std::size_t>(Priority::Visible)] == 2);
        CHECK(st.queued[static_cast<std::size_t>(Priority::Background)] == 1);
        CHECK(g.taskKey(dist).text ==
              MathGraph::taskKeyPrefix(src->id) + "Normal/fake dist/" + g.version(dist).hex() + "/all");
        CHECK(g.taskKey(dist).kind() == TaskKey::Kind::Share);
        gate.release();
        for (MathRequest& rq : requests) {
            REQUIRE(rq.future.get());
        }
    }
    CHECK(module.speedEvals->load() == 1);
    CHECK(module.distEvals->load() == 1);
    for (const MathRequest& rq : requests) {
        g.applyCompletion(rq.key, rq.version, rq.future.get());
        CHECK(g.status(rq.key).state == MathState::Ok);
    }
    // Prefetching again returns the cached columns.
    for (const MathRequest& rq : g.prefetch()) {
        CHECK(rq.future.ready());
    }
    CHECK(module.speedEvals->load() == 1);
}

TEST_CASE("math graph: a new version never shares a task with the superseded one", "[unit][graph]") {
    test::Runtime rt(2);
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const ChannelKey speed = *g.find("Fake Speed");
    const ChannelKey dist = *g.find("Fake Dist");
    const Version old = g.version(speed);

    std::vector<MathRequest> requests;
    {
        test::WorkerGate gate(rt.pool, 2);
        requests = g.prefetch(); // queued: the workers are held
        const std::string oldKey = g.taskKey(speed).text;
        CHECK(rt.pool.queuedTasks(oldKey).size() == 1);
        // A new column for the input re-versions Fake Speed and Fake Dist: their queued evaluations of the
        // old version are cancelled instead of being left to run (and to seal chunks) for nothing.
        auto next = std::make_shared<model::DataSource>(*src);
        std::vector<float> faster = speedKmh();
        for (float& v : faster) {
            v += 10.F;
        }
        next->layers[0].findMutable("Speed In")->series =
            test::uniformF32(rt.cache, "Speed In", test::unitOf("km/h"), Rate::fromHz(1000), faster).series;
        CHECK(g.setSource(next) == std::vector<ChannelKey>{speed, dist});
        CHECK(g.version(speed) != old);
        CHECK(rt.pool.queuedTasks(oldKey).empty());
        CHECK(rt.pool.queuedTasks(g.taskKey(speed).text).empty()); // nothing of the new version yet
    }
    for (MathRequest& rq : requests) {
        if (rq.key == speed || rq.key == dist) {
            CHECK(rq.future.get().error().code == ErrorCode::Cancelled);
        }
    }
    // The new version evaluates; its chunk task keys carry its version.
    Result<ColumnHandle> r = await(g, speed);
    REQUIRE(r);
    const store::Column& col = store::seriesValues(r->series);
    const std::string chunkKey = col.chunkTaskKey(0).text;
    CHECK(chunkKey.find("/maths/fake speed/" + g.version(speed).hex() + "/") != std::string::npos);
    CHECK(chunkKey.find(old.hex()) == std::string::npos);
}

TEST_CASE("math graph: cancelAll on the source prefix stops queued evaluations", "[unit][graph]") {
    test::Runtime rt(2);
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    std::vector<MathRequest> requests;
    {
        test::WorkerGate gate(rt.pool, 2);
        requests = g.prefetch();
        rt.pool.cancelAll(MathGraph::taskKeyPrefix(src->id));
    }
    for (MathRequest& rq : requests) {
        CHECK(rq.future.get().error().code == ErrorCode::Cancelled);
    }
    CHECK(g.applyReadyCompletions().size() == 3);
    for (const ChannelKey& k : g.outputs()) {
        CHECK(g.status(k).state == MathState::Pending);
    }
    CHECK(module.speedEvals->load() == 0);
}

TEST_CASE("math graph: invalid output sets are rejected and change nothing", "[unit][graph]") {
    test::Runtime rt;
    auto src = fakeSource(rt.cache);
    MathGraph g = makeGraph(rt, src);
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    const Version v0 = g.version(*g.find("Fake Speed"));
    struct Case {
        const char* settings;
        ErrorCode code;
        const char* diag;
    };
    for (const Case& c : {Case{R"({"shape":"duplicate"})", ErrorCode::Invalid, "E-DUPLICATE-OUTPUT"},
                          Case{R"({"shape":"unknownUpstream"})", ErrorCode::Invalid, "E-UNKNOWN-UPSTREAM"},
                          Case{R"({"shape":"cycle"})", ErrorCode::Cycle, "E-CYCLE"},
                          Case{R"({"shape":"noEvaluate"})", ErrorCode::Invalid, "E-EVAL"}}) {
        INFO(c.settings);
        Diagnostics d;
        Result<std::vector<ChannelKey>> r = g.defineBuiltin(module, FakeModule::json(c.settings), d);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == c.code);
        REQUIRE_FALSE(d.empty());
        CHECK(d.back().code == c.diag);
        CHECK(hasError(d));
        CHECK(g.outputs().size() == 3);
        CHECK(g.version(*g.find("Fake Speed")) == v0);
    }
    // Another module defining outputs of the same names shadows them (03 section 4.3, M2): one chain per name
    // in registration order, whose head is the later module's output; the earlier outputs keep their
    // versions.
    FakeModule second("test.second", "Fake Speed");
    Diagnostics d;
    Result<std::vector<ChannelKey>> shadow = g.defineBuiltin(second, {}, d);
    REQUIRE(shadow);
    CHECK(g.outputs().size() == 3);
    CHECK(g.nodes().size() == 6);
    const std::vector<MathNodeInfo> infos = g.nodes();
    CHECK(infos[0].moduleId == "test.fake");
    CHECK_FALSE(infos[0].head);
    CHECK(infos[0].id.chainIndex == 0);
    CHECK(infos[0].status.version == v0);
    CHECK(infos[3].moduleId == "test.second");
    CHECK(infos[3].head);
    CHECK(infos[3].id.chainIndex == 1);
    CHECK(g.version(*g.find("Fake Speed")) == infos[3].status.version);
    CHECK(g.descriptor(*g.find("Fake Speed"))->id ==
          model::channelIdMathsModule(src->id, "Normal", "Fake Speed", "test.second"));
    Result<ColumnHandle> head = await(g, *g.find("Fake Dist"));
    REQUIRE(head);
    CHECK(second.distEvals->load() == 1);
    CHECK(module.distEvals->load() == 0);

    MathGraph noUnits(src, "Normal", nullptr, {}, &rt.pool, &rt.cache);
    CHECK(noUnits.defineBuiltin(module, {}, d).error().code == ErrorCode::Invalid);
    CHECK(noUnits.outputs().empty());
}

TEST_CASE("math graph: without a pool or cache require fails instead of blocking", "[unit][graph]") {
    store::ChunkCache cache(std::size_t{64} << 20);
    auto src = fakeSource(cache);
    REQUIRE(TaskPool::installed() == nullptr);
    REQUIRE(store::ChunkCache::installed() == nullptr);
    MathGraph g(src, "Normal", test::shippedUnits());
    FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    Future<ColumnHandle> f = g.require(*g.find("Fake Speed"), SampleRange::all(), Priority::Visible, {});
    REQUIRE(f.ready());
    CHECK(f.get().error().code == ErrorCode::Invalid);
    MathGraph moved = std::move(g);
    CHECK(moved.outputs().size() == 3);
    CHECK(moved.source() == src);
}
