// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LdChunkProvider (05 section 3.11): chunk validity from gap blocks (AllValid / Bitmap / AllInvalid), NaN in
// float chunks, I32 for identity integers, through a Column with the ChunkCache, the changed-file guard and
// cancellation.
#include "ld/ld_chunk_provider.hpp"
#include "ld/ld_codes.hpp"
#include "ld_test_support.hpp"
#include "m4/reload_test_support.hpp"

#include <esses/store/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
namespace L = esses::formats::ld::layout;
using store::Dtype;
using store::kChunkSamples;
using store::Validity;

namespace {

constexpr std::int64_t kN = 2 * kChunkSamples + 1000;

LdWriteInput providerInput() {
    LdWriteInput in;
    // Identity int32 -> I32 chunks.
    auto counter = intChannel("Counter", L::kTypeInt5, 4, 1000, ramp(kN, -100, 1));
    counter.invalid = {{100, 200}, {kChunkSamples, 2 * kChunkSamples}};
    in.channels.push_back(counter);
    // Scaled int16 -> F32 chunks with NaN at invalid samples.
    std::vector<std::int64_t> raw(static_cast<std::size_t>(kN));
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::int64_t>(i % 20000) - 10000;
    }
    auto temp = intChannel("Water Temp", L::kTypeInt3, 2, 1000, std::move(raw));
    temp.dec = 1;
    temp.invalid = {{kChunkSamples - 10, kChunkSamples + 10}};
    in.channels.push_back(temp);
    // Float with a stored NaN and no gap block.
    std::vector<double> fv(1000, 2.5);
    fv[7] = std::numeric_limits<double>::quiet_NaN();
    in.channels.push_back(floatChannel("Lambda", L::kTypeFloat, 4, 100, fv));
    return in;
}

store::OwnedChunk materialise(LdChunkProvider& p, store::SampleIdx chunkNo) {
    store::ChunkBuilder builder;
    REQUIRE(p.materialise(chunkNo, builder, {}).ok());
    auto sealed = builder.seal(p.version());
    REQUIRE(sealed.ok());
    return std::move(*sealed);
}

} // namespace

TEST_CASE("ld provider: chunk validity follows the gap intervals", "[unit][ld_provider]") {
    const auto f = roundTrip(providerInput());
    auto log = std::make_shared<DecodeLog>();
    LdChunkProvider counter(f, 0, log);
    CHECK(counter.size() == kN);
    CHECK(counter.dtype() == Dtype::I32);
    CHECK(counter.chunkIndependent());
    CHECK(counter.debugName() == "ld:Counter");

    const auto c0 = materialise(counter, 0);
    CHECK(c0->validity() == Validity::Bitmap);
    CHECK(c0->count() == kChunkSamples);
    CHECK(c0->validAt(99));
    CHECK_FALSE(c0->validAt(100));
    CHECK_FALSE(c0->validAt(199));
    CHECK(c0->validAt(200));
    CHECK(c0->samples<std::int32_t>()[0] == -100);
    CHECK(c0->samples<std::int32_t>()[150] == 50); // invalid I32 samples keep the stored value

    const auto c1 = materialise(counter, 1);
    CHECK(c1->validity() == Validity::AllInvalid);

    const auto c2 = materialise(counter, 2);
    CHECK(c2->validity() == Validity::AllValid);
    CHECK(c2->count() == 1000u);
    CHECK(c2->samples<std::int32_t>()[999] == kN - 1 - 100);

    store::ChunkBuilder builder;
    CHECK(counter.materialise(3, builder, {}).error().code == ErrorCode::Invalid);
    CHECK(log->size() == 0);
}

TEST_CASE("ld provider: float chunks carry NaN for invalid samples", "[unit][ld_provider]") {
    const auto f = roundTrip(providerInput());
    LdChunkProvider temp(f, 1);
    CHECK(temp.dtype() == Dtype::F32);
    const auto c0 = materialise(temp, 0);
    CHECK(c0->validity() == Validity::Bitmap);
    const auto s0 = c0->samples<float>();
    CHECK(s0[0] == -1000.0f);
    CHECK(s0[1] == static_cast<float>(-9999.0 / 10.0));
    CHECK(std::isnan(s0[kChunkSamples - 10]));
    CHECK_FALSE(c0->validAt(kChunkSamples - 1));
    const auto c1 = materialise(temp, 1);
    CHECK(c1->validity() == Validity::Bitmap);
    CHECK(std::isnan(c1->samples<float>()[9]));
    CHECK(c1->validAt(10));
    CHECK(std::isnan(c1->valueAt(0)));

    // A NaN stored in a float channel is invalid too (sealing downgrades AllValid to Bitmap).
    LdChunkProvider lambda(f, 2);
    const auto l0 = materialise(lambda, 0);
    CHECK(l0->validity() == Validity::Bitmap);
    CHECK_FALSE(l0->validAt(7));
    CHECK(l0->validAt(8));
}

TEST_CASE("ld provider: a Column decodes lazily through the cache", "[unit][ld_provider]") {
    TempDir dir("column");
    const auto path = dir.path() / "column.ld";
    REQUIRE(LdWriter::write(path, providerInput()).ok());
    auto f = LdFile::open(path);
    REQUIRE(f.ok());
    store::ChunkCache cache(std::size_t{1} << 30);
    store::Column col(std::make_shared<LdChunkProvider>(*f, 0), store::ColumnOptions{&cache});
    CHECK(col.size() == kN);
    CHECK(col.chunkCount() == 3);
    auto h = col.require(0, kN - 1, Priority::Visible, {});
    REQUIRE(h.ok());
    CHECK(h->at(0) == -100.0);
    CHECK(std::isnan(h->at(150)));
    CHECK_FALSE(h->validAt(kChunkSamples + 5));
    CHECK(h->at(kN - 1) == static_cast<double>(kN - 1 - 100));
}

TEST_CASE("ld provider: a file changed on disk fails the chunk with Io", "[unit][ld_provider]") {
    TempDir dir("changed");
    const auto path = dir.path() / "changed.ld";
    REQUIRE(LdWriter::write(path, providerInput()).ok());
    auto f = LdFile::open(path);
    REQUIRE(f.ok());
    auto log = std::make_shared<DecodeLog>();
    LdChunkProvider p(*f, 0, log);
    materialise(p, 0);
    // Another writer touched the file (changing the timestamp needs no write access to the mapped data;
    // through the attributes only, so every standard library can do it while the file is mapped).
    REQUIRE(reload_test::shiftMtime(path, -std::chrono::hours(1)));
    store::ChunkBuilder builder;
    const auto r = p.materialise(2, builder, {});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    CHECK(log->contains(code::kSourceChanged));
    CHECK_FALSE(p.materialise(0, builder, {}).ok());
    CHECK(log->count(code::kSourceChanged) == 1); // logged once
}

TEST_CASE("ld provider: cancellation", "[unit][ld_provider]") {
    const auto f = roundTrip(providerInput());
    LdChunkProvider p(f, 0);
    CancelSource src;
    src.cancel();
    store::ChunkBuilder builder;
    const auto r = p.materialise(0, builder, src.token());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
}
