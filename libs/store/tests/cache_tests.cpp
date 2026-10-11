// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChunkCache and ChunkHandle: pinning and lifetime, stale drops at pin count zero, budget tracking with the
// warning hook and no eviction (D-011, D-043), insertGroup, clear (02 section 3.2, 02 section 8 item 4; 01
// section 5.3 rules 5 and 11).
#include "store_test_support.hpp"

#include <esses/base/taskpool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <vector>

using namespace esses;
using namespace esses::store;

namespace {

constexpr std::size_t kGiB = std::size_t{1} << 30;

std::vector<float> ramp(SampleIdx n, float scale = 1.0f) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<float>(i % 100'000) * scale;
    }
    return v;
}

std::shared_ptr<test::TestProvider<float>> rampProvider(SampleIdx n) {
    test::ColumnData<float> d;
    d.v = ramp(n);
    return std::make_shared<test::TestProvider<float>>(std::move(d));
}

} // namespace

TEST_CASE("cache: a pinned chunk outlives its column and is freed at the last unpin", "[unit][cache]") {
    ChunkCache cache(kGiB);
    ChunkHandle h;
    {
        Column col(rampProvider(3 * kChunkSamples), ColumnOptions{&cache});
        Result<ChunkHandle> r = col.require(10, kChunkSamples + 10, Priority::Visible, {});
        REQUIRE(r.ok());
        h = std::move(*r);
        CHECK(cache.stats().chunks == 2u);
        CHECK(cache.stats().pinnedChunks == 2u);
    } // the Column (and its provider) is gone; the pinned chunks stay readable
    CHECK(cache.stats().chunks == 2u);
    CHECK(h.at(kChunkSamples + 5) == static_cast<double>(kChunkSamples + 5));
    CHECK(h.chunk(0)->count() == kChunkSamples);
    const std::size_t pinned = cache.stats().pinnedBytes;
    CHECK(pinned == cache.stats().residentBytes);
    h.reset();
    const ChunkCache::Stats s = cache.stats();
    CHECK(s.chunks == 0u);
    CHECK(s.residentBytes == 0u);
    CHECK(s.pinnedBytes == 0u);
    CHECK(s.pinnedChunks == 0u);
}

TEST_CASE("cache: a version bump makes chunks stale and stale memory is freed only at pin count zero",
          "[unit][cache]") {
    ChunkCache cache(kGiB);
    test::ColumnData<float> d;
    d.v = ramp(2 * kChunkSamples);
    auto provider = std::make_shared<test::TestProvider<float>>(d);
    provider->setValueFn([](std::uint64_t version, SampleIdx) { return static_cast<float>(version); });
    Column col(provider, ColumnOptions{&cache});

    Result<ChunkHandle> v1 = col.require(0, 0, Priority::Visible, {});
    REQUIRE(v1.ok());
    CHECK(v1->at(0) == 1.0);
    CHECK(col.isResident(0));
    const std::size_t oneChunk = cache.stats().residentBytes;

    provider->bumpVersion();
    CHECK(col.version() == 2u);
    CHECK_FALSE(col.isResident(0)); // stale is absent (behaviour 9)
    CHECK_FALSE(col.tryPin(0, Priority::Visible).has_value());

    Result<ChunkHandle> v2 = col.require(0, 0, Priority::Visible, {});
    REQUIRE(v2.ok());
    CHECK(v2->at(0) == 2.0);
    CHECK(v2->chunk(0)->version() == 2u);
    // The old pin still reads the old chunk; both are resident until it is released.
    CHECK(v1->at(0) == 1.0);
    CHECK(cache.stats().chunks == 2u);
    CHECK(cache.stats().residentBytes == 2 * oneChunk);
    CHECK(provider->calls(0) == 2);
    v1->reset();
    CHECK(cache.stats().chunks == 1u);
    CHECK(cache.stats().residentBytes == oneChunk);
    v2->reset();
    CHECK(cache.stats().chunks == 1u); // current and unpinned: stays resident (no eviction before M5)
    CHECK(cache.stats().pinnedBytes == 0u);
}

TEST_CASE("cache: budget tracking warns once per 10 % step and never evicts (M0-M4)", "[unit][cache]") {
    ChunkCache cache(kGiB);
    const SampleIdx chunks = 14;
    Column col(rampProvider(chunks * kChunkSamples), ColumnOptions{&cache});
    // The bytes of one full F32 chunk without a bitmap.
    const std::size_t b = kChunkHeaderBytes + Chunk::layoutBytes(kChunkSamples, Dtype::F32, false);
    cache.setBudgetBytes(10 * b);
    std::mutex m;
    std::vector<ChunkCache::BudgetWarning> warnings;
    cache.setBudgetWarningHook([&](const ChunkCache::BudgetWarning& w) {
        std::lock_guard lock(m);
        warnings.push_back(w);
    });
    for (SampleIdx n = 0; n < 13; ++n) {
        REQUIRE(col.require(chunkFirstSample(n), chunkFirstSample(n), Priority::Visible, {}).ok());
    }
    ChunkCache::Stats s = cache.stats();
    CHECK(s.chunks == 13u);
    CHECK(s.residentBytes == 13 * b);
    CHECK(s.overBudget);
    CHECK(s.evictions == 0u);
    REQUIRE(warnings.size() == 3u); // 110 %, 120 %, 130 %
    CHECK(warnings[0].percent == 110u);
    CHECK(warnings[1].percent == 120u);
    CHECK(warnings[2].percent == 130u);
    CHECK(warnings[2].budgetBytes == 10 * b);
    CHECK(warnings[2].residentBytes == 13 * b);

    // Budget 0: still no eviction, one more warning, everything stays resident.
    cache.setBudgetBytes(0);
    CHECK(cache.stats().chunks == 13u);
    CHECK(cache.stats().overBudget);
    CHECK(warnings.size() == 4u);

    // Falling back under the budget re-arms the warning.
    cache.setBudgetBytes(20 * b);
    CHECK_FALSE(cache.stats().overBudget);
    cache.clear();
    CHECK(cache.stats().chunks == 0u);
    cache.setBudgetBytes(b / 2);
    REQUIRE(col.require(0, 0, Priority::Visible, {}).ok());
    CHECK(warnings.size() == 5u);
    CHECK(cache.budgetBytes() == b / 2);
}

TEST_CASE("cache: ChunkHandle is move-only and move assignment unpins the old span", "[unit][cache]") {
    ChunkCache cache(kGiB);
    Column col(rampProvider(4 * kChunkSamples), ColumnOptions{&cache});
    Result<ChunkHandle> a = col.require(0, 3 * kChunkSamples, Priority::Visible, {}); // 4 chunks: heap pins
    Result<ChunkHandle> b = col.require(0, 0, Priority::Visible, {});
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a->lastChunk() == 3);
    CHECK(cache.stats().pinnedChunks == 4u);
    ChunkHandle moved(std::move(*a));
    CHECK(a->empty());
    CHECK(moved.lastChunk() == 3);
    CHECK(moved.at(3 * kChunkSamples + 1) == static_cast<double>((3 * kChunkSamples + 1) % 100'000));
    CHECK(cache.stats().pinnedChunks == 4u);
    moved = std::move(*b); // unpins chunks 1..3 (chunk 0 is still pinned through b's pin, now in moved)
    CHECK(cache.stats().pinnedChunks == 1u);
    CHECK(moved.firstChunk() == 0);
    CHECK(moved.lastChunk() == 0);
    CHECK(std::isnan(moved.at(kChunkSamples))); // outside the pinned span
    CHECK(moved.chunk(1) == nullptr);
    CHECK_FALSE(moved.int64At(0).has_value()); // not an I64 chunk
    moved.reset();
    CHECK(cache.stats().pinnedChunks == 0u);
    ChunkHandle empty;
    CHECK(empty.empty());
    CHECK(std::isnan(empty.at(0)));
}

TEST_CASE("cache: clear frees unpinned chunks only and columns re-materialise", "[unit][cache]") {
    ChunkCache cache(kGiB);
    auto provider = rampProvider(3 * kChunkSamples);
    Column col(provider, ColumnOptions{&cache});
    Result<ChunkHandle> keep = col.require(0, 0, Priority::Visible, {});
    REQUIRE(keep.ok());
    REQUIRE(col.require(kChunkSamples, 3 * kChunkSamples - 1, Priority::Visible, {}).ok());
    CHECK(cache.stats().chunks == 3u);
    cache.clear();
    CHECK(cache.stats().chunks == 1u);
    CHECK(col.isResident(0));
    CHECK_FALSE(col.isResident(1));
    REQUIRE(col.require(kChunkSamples, kChunkSamples, Priority::Visible, {}).ok());
    CHECK(provider->calls(1) == 2);
    CHECK(provider->calls(0) == 1);
}

TEST_CASE("cache: hit and miss counters", "[unit][cache]") {
    ChunkCache cache(kGiB);
    Column col(rampProvider(kChunkSamples), ColumnOptions{&cache});
    CHECK_FALSE(col.tryPin(0, Priority::Visible).has_value()); // no pool: nothing scheduled
    CHECK(cache.stats().misses == 1u);
    REQUIRE(col.require(0, 0, Priority::Visible, {}).ok());
    CHECK(cache.stats().misses == 2u);
    CHECK(col.tryPin(0, Priority::Visible).has_value());
    CHECK(cache.stats().hits == 1u);
}

TEST_CASE("cache: insertGroup seals sibling columns from one sweep", "[unit][cache]") {
    ChunkCache cache(kGiB);
    std::vector<std::shared_ptr<test::TestProvider<float>>> providers;
    std::vector<Column> cols;
    for (int c = 0; c < 3; ++c) {
        providers.push_back(rampProvider(kChunkSamples + 500));
        cols.emplace_back(providers.back(), ColumnOptions{&cache});
    }
    // Column 0's chunk 1 is already resident: the group skips it.
    REQUIRE(cols[0].require(kChunkSamples, kChunkSamples, Priority::Visible, {}).ok());

    std::vector<ChunkCache::SealedChunk> group;
    for (int c = 0; c < 3; ++c) {
        ChunkBuilder b;
        REQUIRE(b.allocate(500, Dtype::F32).ok());
        std::span<float> s = b.samples<float>();
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = static_cast<float>(c * 1'000 + static_cast<int>(i));
        }
        Result<OwnedChunk> sealed = b.seal(1);
        REQUIRE(sealed.ok());
        group.push_back(
            ChunkCache::SealedChunk{&cols[static_cast<std::size_t>(c)], 1, 1, std::move(*sealed)});
    }
    // A member of the wrong version and one of the wrong size are skipped.
    {
        ChunkBuilder b;
        REQUIRE(b.allocate(500, Dtype::F32).ok());
        Result<OwnedChunk> sealed = b.seal(9);
        REQUIRE(sealed.ok());
        group.push_back(ChunkCache::SealedChunk{&cols[1], 1, 9, std::move(*sealed)});
        ChunkBuilder w;
        REQUIRE(w.allocate(10, Dtype::F32).ok());
        Result<OwnedChunk> wrong = w.seal(1);
        REQUIRE(wrong.ok());
        group.push_back(ChunkCache::SealedChunk{&cols[2], 0, 1, std::move(*wrong)});
    }
    const std::size_t before = cache.stats().chunks;
    CHECK(cache.insertGroup(group) == 2u);
    CHECK(cache.stats().chunks == before + 2u);
    CHECK(cols[1].isResident(1));
    CHECK(cols[2].isResident(1));
    CHECK_FALSE(cols[2].isResident(0));
    // Read through a pin, without the provider ever being called for those chunks.
    std::optional<ChunkHandle> h = cols[2].tryPin(1, Priority::Visible);
    REQUIRE(h.has_value());
    CHECK(h->at(kChunkSamples + 3) == 2'003.0);
    CHECK(providers[2]->totalCalls() == 0);
    CHECK(providers[1]->totalCalls() == 0);
    // The group fills the summary too.
    CHECK(cols[2].summary().chunkFilled(1));
}

TEST_CASE("cache: the runtime installs the process-wide cache", "[unit][cache]") {
    ChunkCache* previous = ChunkCache::installed();
    {
        ChunkCache cache(kGiB);
        ChunkCache::install(&cache);
        CHECK(&ChunkCache::instance() == &cache);
        Column col(rampProvider(10)); // uses ChunkCache::instance()
        REQUIRE(col.require(0, 9, Priority::Visible, {}).ok());
        CHECK(cache.stats().chunks == 1u);
    } // the cache uninstalls itself on destruction
    CHECK(ChunkCache::installed() == nullptr);
    ChunkCache::install(previous);
    CHECK(ChunkCache::defaultBudgetBytes() >= kGiB);
}
