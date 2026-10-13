// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlanProvider sealing a whole-extent maths output into a ChunkCache (03 sections 6.0 and 7.2): chunk
// slicing, validity and NaN, dtypes, the release of the evaluated extent once every chunk is sealed,
// recomputation after that, and which evaluation failures are memoised. Also readWholeSignal, the inverse
// read.

#include "maths_test_support.hpp"

#include <esses/maths/plan_provider.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/column.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace esses;
using namespace esses::maths;

namespace {

constexpr SampleIdx kSize = 70'000; // two chunks: 65,536 + 4,464

analysis::UniformSignal ramp(SampleIdx n, bool invalidInSecondChunk) {
    analysis::UniformSignal s;
    s.rate = Rate::fromHz(100);
    s.t0 = 1'000;
    s.values.resize(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        s.values[i] = 0.5 * static_cast<double>(i) + 0.25;
    }
    if (invalidInSecondChunk) {
        s.valid.assign(s.values.size(), 1);
        s.valid[66'000] = 0;
        s.valid[69'999] = 0;
    }
    return s;
}

std::shared_ptr<PlanSnapshot> planFor(store::Dtype dtype, SampleIdx size,
                                      std::function<Result<analysis::UniformSignal>(const CancelToken&)> fn) {
    auto plan = std::make_shared<PlanSnapshot>();
    plan->key = ChannelKey::make(model::SourceId{}, "Normal", "Ramp");
    plan->version = hash128("ramp-version");
    plan->outDtype = dtype;
    plan->rate = Rate::fromHz(100);
    plan->t0 = 1'000;
    plan->size = size;
    plan->name = "Ramp";
    plan->evaluate = std::move(fn);
    return plan;
}

store::Column columnOver(store::ChunkCache& cache, std::shared_ptr<PlanProvider> provider) {
    store::ColumnOptions options;
    options.cache = &cache;
    options.taskKeyPrefix = "chunk/test/Normal/maths/Ramp";
    return store::Column(std::move(provider), options);
}

} // namespace

TEST_CASE("plan provider: a seeded extent seals into ChunkCache chunks with validity and NaN",
          "[unit][plan]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    auto provider =
        std::make_shared<PlanProvider>(planFor(store::Dtype::F32, kSize, nullptr), ramp(kSize, true));
    CHECK(provider->size() == kSize);
    CHECK(provider->dtype() == store::Dtype::F32);
    CHECK_FALSE(provider->chunkIndependent());
    CHECK(provider->version() == hash128("ramp-version").lo);
    CHECK(provider->debugName() == "plan:Ramp@" + hash128("ramp-version").hex());
    CHECK(provider->holdsExtent());

    store::Column column = columnOver(cache, provider);
    {
        Result<store::ChunkHandle> pinned = column.require(0, kSize - 1, Priority::Visible, {});
        REQUIRE(pinned);
        const store::ChunkHandle& h = *pinned;
        REQUIRE(h.chunk(0) != nullptr);
        REQUIRE(h.chunk(1) != nullptr);
        CHECK(h.chunk(0)->validity() == store::Validity::AllValid); // no invalid sample: no bitmap
        CHECK(h.chunk(1)->validity() == store::Validity::Bitmap);
        CHECK(h.chunk(1)->count() == static_cast<std::uint32_t>(kSize - 65'536));
        CHECK(h.at(0) == 0.25);
        CHECK(h.at(65'535) == static_cast<double>(static_cast<float>(0.5 * 65'535 + 0.25)));
        CHECK(h.at(65'999) == static_cast<double>(static_cast<float>(0.5 * 65'999 + 0.25)));
        CHECK_FALSE(h.validAt(66'000));
        CHECK(std::isnan(h.chunk(1)->samples<float>()[66'000 - 65'536])); // float columns carry NaN (03 6.6)
        CHECK_FALSE(h.validAt(69'999));
        CHECK(h.validAt(69'998));
        CHECK(h.chunk(0)->builtLevels() == store::kAllLevelsBuilt);
    }
    // Every chunk sealed: the provider dropped the extent and never evaluated (it was seeded).
    CHECK_FALSE(provider->holdsExtent());
    CHECK(provider->evaluations() == 0);
    CHECK(cache.stats().chunks == 2);
    CHECK(column.summary().complete());
}

TEST_CASE("plan provider: a chunk asked for after the release evaluates the whole extent again",
          "[unit][plan]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    std::atomic<int> calls{0};
    auto plan =
        planFor(store::Dtype::F64, kSize, [&calls](const CancelToken&) -> Result<analysis::UniformSignal> {
            calls.fetch_add(1);
            return ramp(kSize, false);
        });
    auto provider = std::make_shared<PlanProvider>(plan, ramp(kSize, false));
    store::Column column = columnOver(cache, provider);
    std::vector<double> first;
    {
        Result<store::ChunkHandle> pinned = column.require(0, kSize - 1, Priority::Visible, {});
        REQUIRE(pinned);
        for (SampleIdx i = 0; i < kSize; i += 997) {
            first.push_back(pinned->at(i));
        }
        CHECK(pinned->chunk(0)->dtype() == store::Dtype::F64);
        CHECK(pinned->at(12'345) == 0.5 * 12'345 + 0.25); // F64 keeps the double exactly
    }
    CHECK(calls.load() == 0);
    cache.clear(); // stands in for the M5 eviction of the output's eviction group
    {
        Result<store::ChunkHandle> pinned = column.require(0, kSize - 1, Priority::Visible, {});
        REQUIRE(pinned);
        std::size_t k = 0;
        for (SampleIdx i = 0; i < kSize; i += 997) {
            CHECK(pinned->at(i) == first[k++]); // bit-identical recomputation
        }
    }
    CHECK(calls.load() == 1);
    CHECK(provider->evaluations() == 1);
    CHECK_FALSE(provider->holdsExtent());
}

TEST_CASE("plan provider: a cancelled evaluation is retried and a failed one is memoised", "[unit][plan]") {
    std::atomic<int> calls{0};
    std::atomic<bool> failHard{false};
    auto plan =
        planFor(store::Dtype::F32, 10, [&](const CancelToken& token) -> Result<analysis::UniformSignal> {
            calls.fetch_add(1);
            if (token.cancelled()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            if (failHard.load()) {
                return fail(ErrorCode::NotFound, "no input");
            }
            return ramp(10, false);
        });
    SECTION("cancelled") {
        PlanProvider provider(plan);
        CancelSource cancelled;
        cancelled.cancel();
        store::ChunkBuilder b;
        CHECK(provider.materialise(0, b, cancelled.token()).error().code == ErrorCode::Cancelled);
        CHECK(calls.load() == 0); // checked before evaluating
        Result<void> again = provider.materialise(0, b, {});
        CHECK(again);
        CHECK(calls.load() == 1);
        REQUIRE(b.allocated());
        CHECK(b.count() == 10);
        CHECK(b.samples<float>()[3] == 1.75F);
    }
    SECTION("failed") {
        failHard = true;
        PlanProvider provider(plan);
        store::ChunkBuilder b;
        CHECK(provider.materialise(0, b, {}).error().code == ErrorCode::NotFound);
        CHECK(provider.materialise(0, b, {}).error().code == ErrorCode::NotFound);
        CHECK(calls.load() == 1);
    }
    SECTION("wrong size and range") {
        auto wrong =
            planFor(store::Dtype::F32, 11,
                    [](const CancelToken&) -> Result<analysis::UniformSignal> { return ramp(10, false); });
        PlanProvider provider(wrong);
        store::ChunkBuilder b;
        CHECK(provider.materialise(0, b, {}).error().code == ErrorCode::Internal);
        PlanProvider ok10(plan);
        CHECK(ok10.materialise(1, b, {}).error().code == ErrorCode::Invalid);
        CHECK(ok10.materialise(-1, b, {}).error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("plan provider: I32 outputs round and invalid samples clear their bit", "[unit][plan]") {
    analysis::UniformSignal s;
    s.rate = Rate::fromHz(10);
    s.values = {0.4, 1.6, -2.5, 7.0};
    s.valid = {1, 1, 1, 0};
    PlanProvider provider(planFor(store::Dtype::I32, 4, nullptr), s);
    store::ChunkBuilder b;
    REQUIRE(provider.materialise(0, b, {}));
    REQUIRE(b.hasBitmap());
    Result<store::OwnedChunk> sealed = b.seal(1);
    REQUIRE(sealed);
    const store::Chunk& c = **sealed;
    CHECK(c.samples<std::int32_t>()[0] == 0);
    CHECK(c.samples<std::int32_t>()[1] == 2);
    CHECK(c.samples<std::int32_t>()[2] == -3); // llround: half away from zero
    CHECK_FALSE(c.validAt(3));
}

TEST_CASE("whole signal: reads a column across chunks with validity and unit conversion", "[unit][plan]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    std::vector<float> kmh(70'000);
    std::vector<bool> valid(kmh.size(), true);
    for (std::size_t i = 0; i < kmh.size(); ++i) {
        kmh[i] = static_cast<float>(i % 360);
    }
    valid[65'536] = false;
    kmh[10] = std::nanf("");
    const model::Channel ch =
        test::uniformF32(cache, "Speed", test::unitOf("km/h"), Rate::fromHz(50), kmh, valid);
    const units::Converter toBase = test::shippedUnits()->toBase(test::unitOf("km/h"));
    Result<analysis::UniformSignal> s = readWholeSignal(ch.series, {}, toBase);
    REQUIRE(s);
    REQUIRE(s->size() == kmh.size());
    CHECK(s->rate == Rate::fromHz(50));
    CHECK(s->t0 == 0);
    CHECK(s->values[100] == toBase(static_cast<double>(kmh[100])));
    CHECK(std::abs(s->values[36] - 10.0) < 1e-12); // 36 km/h = 10 m/s
    CHECK_FALSE(s->validAt(65'536));
    CHECK(std::isnan(s->values[65'536]));
    CHECK_FALSE(s->validAt(10)); // NaN is invalid
    CHECK(s->validAt(65'537));

    const model::Channel clean =
        test::uniformF32(cache, "Clean", units::kNoUnit, Rate::fromHz(5), {1.F, 2.F});
    Result<analysis::UniformSignal> c = readWholeSignal(clean.series, {});
    REQUIRE(c);
    CHECK(c->valid.empty()); // all valid: no flags
    CHECK(c->values == std::vector<double>{1.0, 2.0});

    store::TimestampedSeries ts;
    CHECK(readWholeSignal(store::Series{ts}, {}).error().code == ErrorCode::Unsupported);
    const model::Channel empty = test::uniformF32(cache, "Empty", units::kNoUnit, Rate::fromHz(5), {});
    Result<analysis::UniformSignal> e = readWholeSignal(empty.series, {});
    REQUIRE(e);
    CHECK(e->size() == 0);
    CancelSource cancelled;
    cancelled.cancel();
    CHECK(readWholeSignal(clean.series, cancelled.token()).error().code == ErrorCode::Cancelled);
}
