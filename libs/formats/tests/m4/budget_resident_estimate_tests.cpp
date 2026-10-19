// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The open-time resident estimate (M4 budget package, AC-M4-02; resident_estimate.hpp). The expected byte
// counts are worked out by hand below from the chunk layout of 01 section 7.1 and store/chunk.hpp's header
// comment (a 64-byte header, then the samples padded to 64 bytes, a validity bitmap of one bit per sample in
// 64-bit words padded to 64 bytes, and the factor-4 pyramid of ceil(n / 4^(k+1)) float pairs for k = 0..7,
// the layout rounded up to 64 bytes; no bitmap and no pyramid for an I64 time column), not computed with the
// store's own layout functions. The last cases hold the estimate against ChunkCache::Stats::residentBytes
// once every chunk of a source is materialised: equal when every chunk carries a bitmap, and exactly the
// bitmaps above it when every chunk is AllValid (the estimate is the upper bound).

#include <esses/formats/resident_estimate.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::formats;

namespace {

// ---- the hand-worked chunks (bytes) ----
// Full F32 or I32 chunk, 65,536 samples: samples 262,144; bitmap 1,024 words = 8,192; pyramid 21,845 entries
// (16,384 + 4,096 + 1,024 + 256 + 64 + 16 + 4 + 1) x 8 = 174,760; layout 445,096 -> 445,120 (24 of rounding);
// header 64.
constexpr std::uint64_t kFullF32 = 64 + 445'120;
// Full F64 chunk: samples 524,288; bitmap 8,192; pyramid 174,760; layout 707,240 -> 707,264 (24); header 64.
constexpr std::uint64_t kFullF64 = 64 + 707'264;
// Full I64 time chunk: samples 524,288, nothing else; header 64.
constexpr std::uint64_t kFullI64 = 64 + 524'288;
// 1,000 F32 samples: samples 4,000 -> 4,032; bitmap 16 words = 128; pyramid 250 + 63 + 16 + 4 + 1 + 1 + 1 + 1
// = 337 entries = 2,696; layout 6,856 -> 6,912 (56); header 64.
constexpr std::uint64_t kF32x1000 = 64 + 6'912;
// One F32 sample: samples 64; bitmap 64; pyramid 8 entries = 64; layout 192; header 64.
constexpr std::uint64_t kF32x1 = 64 + 192;
// 1,000 F64 samples: samples 8,000; bitmap 128; pyramid 2,696; layout 10,824 -> 10,880 (56); header 64.
constexpr std::uint64_t kF64x1000 = 64 + 10'880;
// 10 I32 samples: samples 40 -> 64; bitmap 1 word -> 64; pyramid 3 + 1 x 7 = 10 entries = 80; layout 208 ->
// 256 (48); header 64.
constexpr std::uint64_t kI32x10 = 64 + 256;
// 10 I64 times: samples 80 -> 128; header 64.
constexpr std::uint64_t kI64x10 = 64 + 128;
// 4,464 F32 samples (the rest of 70,000 after one full chunk): samples 17,856; bitmap 70 words = 560 -> 576;
// pyramid 1,116 + 279 + 70 + 18 + 5 + 2 + 1 + 1 = 1,492 entries = 11,936; layout 30,368 -> 30,400 (32).
constexpr std::uint64_t kF32x4464 = 64 + 30'400;
// 100 F64 samples: samples 800 -> 832; bitmap 2 words -> 64; pyramid 25 + 7 + 2 + 1 x 5 = 39 entries = 312;
// layout 1,208 -> 1,216 (8). 100 I64 times: samples 800 -> 832.
constexpr std::uint64_t kF64x100 = 64 + 1'216;
constexpr std::uint64_t kI64x100 = 64 + 832;

model::Channel uniformChannel(const model::SourceId& src, const std::string& name, model::ChannelSource kind,
                              store::Column column) {
    model::Channel ch;
    ch.desc.name = name;
    ch.desc.loggedName = name;
    ch.desc.source = kind;
    ch.desc.dtype = column.dtype();
    ch.desc.rate = Rate::fromHz(100);
    ch.desc.id =
        kind == model::ChannelSource::Logged
            ? model::channelIdLogged(src, "Normal", name, "test")
            : (kind == model::ChannelSource::Maths ? model::channelIdMathsModule(src, "Normal", name, "test")
                                                   : model::channelIdOther(kind, src, "Normal", name));
    ch.series = store::UniformSeries{Rate::fromHz(100), 0, std::move(column)};
    return ch;
}

std::vector<float> ramp(std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(i % 977) * 0.25F;
    }
    return v;
}

// One invalid sample per chunk, so every materialised chunk carries a bitmap.
std::vector<bool> oneInvalidPerChunk(std::size_t n) {
    std::vector<bool> valid(n, true);
    for (std::size_t i = 0; i < n; i += store::kChunkSamples) {
        valid[i] = false;
    }
    return valid;
}

// Every chunk of `column` resident (built inline on this thread: no pool is installed in this executable).
void materialise(const store::Column& column) {
    if (!column.valid() || column.size() == 0) {
        return;
    }
    Result<store::ChunkHandle> pinned = column.require(0, column.size() - 1, Priority::Interactive, {});
    REQUIRE(pinned);
}

} // namespace

TEST_CASE("budget: a full chunk counts its samples, bitmap, pyramid, rounding and header", "[unit][m4]") {
    const ResidentEstimate f32 = estimateChannel(store::Dtype::F32, 65'536);
    CHECK(f32.sampleBytes == 262'144U);
    CHECK(f32.bitmapBytes == 8'192U);
    CHECK(f32.pyramidBytes == 174'760U);
    CHECK(f32.overheadBytes == 64U + 24U);
    CHECK(f32.chunks == 1U);
    CHECK(f32.channels == 1U);
    CHECK(f32.total() == kFullF32);

    CHECK(estimateChannel(store::Dtype::I32, 65'536).total() == kFullF32); // 4 bytes per sample, as F32
    const ResidentEstimate f64 = estimateChannel(store::Dtype::F64, 65'536);
    CHECK(f64.sampleBytes == 524'288U);
    CHECK(f64.pyramidBytes == 174'760U); // float pairs for every dtype (D-054)
    CHECK(f64.total() == kFullF64);

    const ResidentEstimate i64 = estimateChannel(store::Dtype::I64, 65'536);
    CHECK(i64.sampleBytes == 524'288U);
    CHECK(i64.bitmapBytes == 0U);
    CHECK(i64.pyramidBytes == 0U);
    CHECK(i64.overheadBytes == 64U);
    CHECK(i64.total() == kFullI64);
}

TEST_CASE("budget: partial chunks pad each block to 64 bytes", "[unit][m4]") {
    const ResidentEstimate part = estimateChannel(store::Dtype::F32, 1'000);
    CHECK(part.sampleBytes == 4'032U);
    CHECK(part.bitmapBytes == 128U);
    CHECK(part.pyramidBytes == 2'696U);
    CHECK(part.overheadBytes == 64U + 56U);
    CHECK(part.total() == kF32x1000);
    CHECK(estimateChannel(store::Dtype::F32, 1).total() == kF32x1);
    CHECK(estimateChannel(store::Dtype::F64, 1'000).total() == kF64x1000);
    CHECK(estimateChannel(store::Dtype::I32, 10).total() == kI32x10);
    CHECK(estimateChannel(store::Dtype::I64, 10).total() == kI64x10);
    CHECK(estimateChannel(store::Dtype::F32, 4'464).total() == kF32x4464);
    CHECK(estimateChannel(store::Dtype::F64, 100).total() == kF64x100);
    CHECK(estimateChannel(store::Dtype::I64, 100).total() == kI64x100);
    const ResidentEstimate none = estimateChannel(store::Dtype::F64, 0);
    CHECK(none.total() == 0U);
    CHECK(none.chunks == 0U);
}

TEST_CASE("budget: a long channel is its full chunks plus the last partial one", "[unit][m4]") {
    const ResidentEstimate e = estimateChannel(store::Dtype::F32, 2 * 65'536 + 1'000);
    CHECK(e.chunks == 3U);
    CHECK(e.channels == 1U);
    CHECK(e.sampleBytes == 2U * 262'144U + 4'032U);
    CHECK(e.bitmapBytes == 2U * 8'192U + 128U);
    CHECK(e.pyramidBytes == 2U * 174'760U + 2'696U);
    CHECK(e.total() == 2 * kFullF32 + kF32x1000);
    // RW-2's 1 kHz channel (30 min, 1.8 M samples: 27 full chunks and 30,528 samples) is about 12.2 MB.
    const ResidentEstimate rw2 = estimateChannel(store::Dtype::F32, 1'800'000);
    CHECK(rw2.chunks == 28U);
    CHECK(rw2.total() > 27 * kFullF32);
    CHECK(rw2.total() < 28 * kFullF32);
}

TEST_CASE("budget: the probe estimate sums every channel of every layer with timestamped times",
          "[unit][m4]") {
    ProbeResult probe;
    LayerInfo normal;
    ChannelInfo a;
    a.dtype = store::Dtype::F32;
    a.sampleCount = 65'536;
    ChannelInfo b;
    b.dtype = store::Dtype::F64;
    b.sampleCount = 1'000;
    normal.channels = {a, b};
    LayerInfo burst;
    burst.name = "Burst0";
    ChannelInfo c;
    c.dtype = store::Dtype::I32;
    c.sampleCount = 10;
    c.timing = Timestamped{};
    ChannelInfo empty;
    empty.sampleCount = 0;
    burst.channels = {c, empty};
    probe.layers = {normal, burst};

    const ResidentEstimate e = estimateResident(probe);
    CHECK(e.total() == kFullF32 + kF64x1000 + kI32x10 + kI64x10);
    CHECK(e.channels == 4U); // the empty channel is counted, with no bytes
    CHECK(e.chunks == 4U);   // 1 + 1 + 1 + its I64 time chunk
}

TEST_CASE("budget: the source estimate counts logged columns once and nothing else", "[unit][m4]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    store::ColumnOptions options;
    options.cache = &cache;
    auto src = std::make_shared<model::DataSource>();
    src->id.lo = 0x51;
    model::DataLayer layer;
    const store::Column logged(store::InMemoryProvider::fromF32(ramp(70'000)), options);
    layer.addChannel(uniformChannel(src->id, "Logged A", model::ChannelSource::Logged, logged));
    // Another logged channel over the same column (shared chunks), an alias and a maths channel over it.
    layer.addChannel(uniformChannel(src->id, "Logged A copy", model::ChannelSource::Logged, logged));
    layer.addChannel(uniformChannel(src->id, "Alias A", model::ChannelSource::Alias, logged));
    layer.addChannel(uniformChannel(src->id, "Maths M", model::ChannelSource::Maths,
                                    store::Column(store::InMemoryProvider::fromF32(ramp(500'000)), options)));
    // A timestamped logged channel: F64 values and I64 times of 100 samples.
    std::vector<double> values(100);
    std::vector<std::int64_t> times(100);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = 0.5 * static_cast<double>(i);
        times[i] = static_cast<std::int64_t>(i) * 1'000;
    }
    model::Channel stamped;
    stamped.desc.name = "Stamped";
    stamped.desc.loggedName = "Stamped";
    stamped.desc.dtype = store::Dtype::F64;
    stamped.desc.id = model::channelIdLogged(src->id, "Normal", "Stamped", "test");
    stamped.series =
        store::TimestampedSeries{store::Column(store::InMemoryProvider::fromI64(times), options),
                                 store::Column(store::InMemoryProvider::fromF64(values), options), 0, 99'000};
    layer.addChannel(std::move(stamped));
    // A logged channel without samples.
    layer.addChannel(uniformChannel(src->id, "Empty", model::ChannelSource::Logged, store::Column{}));
    src->layers.push_back(std::move(layer));

    const ResidentEstimate e = estimateResident(*src);
    CHECK(e.total() == kFullF32 + kF32x4464 + kF64x100 + kI64x100);
    CHECK(e.chunks == 4U);
    CHECK(e.channels == 2U); // "Logged A" and "Stamped": the copy shares A's chunks, the empty one has none
    CHECK(cache.stats().residentBytes == 0U); // estimating decodes nothing
    CHECK(cache.stats().misses == 0U);
}

TEST_CASE("budget: the estimate equals the resident bytes when every chunk carries a bitmap", "[unit][m4]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    store::ColumnOptions options;
    options.cache = &cache;
    auto src = std::make_shared<model::DataSource>();
    src->id.lo = 0x52;
    model::DataLayer layer;
    const std::size_t n = 3 * store::kChunkSamples + 1'234;
    layer.addChannel(uniformChannel(
        src->id, "F32", model::ChannelSource::Logged,
        store::Column(store::InMemoryProvider::fromF32(ramp(n), oneInvalidPerChunk(n)), options)));
    std::vector<double> wide(2'000);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        wide[i] = static_cast<double>(i) / 3.0;
    }
    layer.addChannel(uniformChannel(
        src->id, "F64", model::ChannelSource::Logged,
        store::Column(store::InMemoryProvider::fromF64(wide, oneInvalidPerChunk(wide.size())), options)));
    std::vector<std::int32_t> counts(70'000);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        counts[i] = static_cast<std::int32_t>(i % 13);
    }
    layer.addChannel(uniformChannel(
        src->id, "I32", model::ChannelSource::Logged,
        store::Column(store::InMemoryProvider::fromI32(counts, oneInvalidPerChunk(counts.size())), options)));
    src->layers.push_back(std::move(layer));

    const std::uint64_t estimate = estimateResident(*src).total();
    REQUIRE(cache.stats().residentBytes == 0U);
    for (const model::Channel& c : src->layers.front().channels) {
        materialise(store::seriesValues(c.series));
    }
    CHECK(cache.stats().chunks == 4U + 1U + 2U);
    CHECK(cache.stats().residentBytes == estimate);
}

TEST_CASE("budget: all-valid chunks stay under the estimate by exactly their bitmaps", "[unit][m4]") {
    store::ChunkCache cache(std::size_t{256} << 20);
    store::ColumnOptions options;
    options.cache = &cache;
    auto src = std::make_shared<model::DataSource>();
    src->id.lo = 0x53;
    model::DataLayer layer;
    const std::size_t n = 2 * store::kChunkSamples + 1'000;
    layer.addChannel(uniformChannel(src->id, "Valid", model::ChannelSource::Logged,
                                    store::Column(store::InMemoryProvider::fromF32(ramp(n)), options)));
    src->layers.push_back(std::move(layer));

    const ResidentEstimate e = estimateResident(*src);
    REQUIRE(e.total() == 2 * kFullF32 + kF32x1000);
    materialise(store::seriesValues(src->layers.front().channels.front().series));
    CHECK(cache.stats().chunks == 3U);
    // AllValid chunks carry no bitmap: 2 x 8,192 + 128 bytes fewer than the upper bound.
    CHECK(cache.stats().residentBytes == e.total() - (2U * 8'192U + 128U));
    CHECK(cache.stats().residentBytes <= e.total());
}
