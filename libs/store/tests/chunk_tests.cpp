// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Chunk, ChunkBuilder and MinMax (02 section 3.1 behaviours 1-5; 02 section 8 item 3; D-054, D-062).
#include "store_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstring>
#include <random>

using namespace esses;
using namespace esses::store;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

template <class T>
OwnedChunk sealChunk(const std::vector<T>& v, const std::vector<std::uint8_t>& valid = {},
                     std::uint64_t version = 1) {
    ChunkBuilder b;
    const bool bitmap = !valid.empty();
    REQUIRE(b.allocate(static_cast<std::uint32_t>(v.size()), kDtypeOf<T>, bitmap).ok());
    std::span<T> s = b.samples<T>();
    std::copy(v.begin(), v.end(), s.begin());
    for (std::size_t i = 0; i < valid.size(); ++i) {
        if (valid[i] == 0) {
            b.setValid(static_cast<std::uint32_t>(i), false);
        }
    }
    Result<OwnedChunk> c = b.seal(version);
    REQUIRE(c.ok());
    return std::move(*c);
}

// Every level entry equals the brute-force fold over its bucket.
template <class T>
void checkLevels(const Chunk& c, const std::vector<T>& v, const std::vector<std::uint8_t>& mask) {
    REQUIRE(c.builtLevels() == kAllLevelsBuilt);
    for (unsigned k = 0; k < kPyramidLevels; ++k) {
        const std::span<const MinMax> level = c.level(k);
        REQUIRE(level.size() == levelBuckets(k, c.count()));
        const SampleIdx b = bucketSize(k);
        for (std::size_t j = 0; j < level.size(); ++j) {
            const auto first = static_cast<SampleIdx>(j) * b;
            const MinMax ref = test::bruteMinMax(v, mask, first, first + b);
            if (!level[j].bitEquals(ref)) {
                FAIL("level " << k << " bucket " << j << " differs from brute force");
            }
        }
    }
}

} // namespace

TEST_CASE("chunk: layout sizes match 01 section 7.1", "[unit][chunk]") {
    // AC 02 section 8 item 3: 262,144 + 8,192 + 174,760 (+ alignment).
    const std::size_t exact = 262'144u + 8'192u + 174'760u;
    const std::size_t f32 = Chunk::layoutBytes(kChunkSamples, Dtype::F32, true);
    CHECK(f32 >= exact);
    CHECK(f32 < exact + kChunkAlignment);
    CHECK(f32 % kChunkAlignment == 0);
    CHECK(Chunk::layoutBytes(kChunkSamples, Dtype::F32, false) == 262'144u + 174'760u + 24u);
    CHECK(Chunk::layoutBytes(kChunkSamples, Dtype::I32, true) == f32);
    // F64: wider samples, the same float pyramid (D-054).
    CHECK(Chunk::layoutBytes(kChunkSamples, Dtype::F64, false) == 524'288u + 174'760u + 24u);
    // I64 time columns carry no pyramid (D-062).
    CHECK(Chunk::layoutBytes(kChunkSamples, Dtype::I64, false) == 524'288u);
    CHECK(pyramidEntries(1) == 8u);
    CHECK(Chunk::layoutBytes(1, Dtype::F32, false) == 128u);
    CHECK(kChunkPyramidEntries * sizeof(MinMax) == 174'760u);
    CHECK(kSummaryEntriesPerChunk * sizeof(MinMax) == 2'728u);
    CHECK(sizeof(Chunk) <= kChunkHeaderBytes);
}

TEST_CASE("chunk: full and partial chunks build exact levels and empty buckets are the sentinel",
          "[unit][chunk]") {
    std::mt19937_64 rng(test::testSeed());
    std::normal_distribution<float> nd(0.0f, 100.0f);
    for (const std::uint32_t count : {kChunkSamples, 1'000u, 1u, 3u, 257u, 65'535u}) {
        std::vector<float> v(count);
        for (float& x : v) {
            x = nd(rng);
        }
        std::vector<std::uint8_t> valid(count, 1);
        // An invalid run covering whole buckets at several levels.
        for (std::uint32_t i = count / 3; i < std::min(count, count / 3 + 300u); ++i) {
            valid[i] = 0;
        }
        OwnedChunk c = sealChunk(v, valid);
        INFO("count " << count);
        CHECK(c->count() == count);
        CHECK(c->validity() == Validity::Bitmap);
        checkLevels(*c, v, valid);
        CHECK(c->bytes() == kChunkHeaderBytes + Chunk::layoutBytes(count, Dtype::F32, true));
    }
    // An all-invalid bucket is {+inf, -inf}.
    std::vector<float> v(64, 1.0f);
    std::vector<std::uint8_t> valid(64, 1);
    for (std::size_t i = 16; i < 32; ++i) {
        valid[i] = 0;
    }
    OwnedChunk c = sealChunk(v, valid);
    const MinMax e = c->level(1)[1];
    CHECK(e.isEmpty());
    CHECK(e.lo == kInf);
    CHECK(e.hi == -kInf);
    CHECK(c->level(2)[0] == (MinMax{1.0f, 1.0f}));
}

TEST_CASE("chunk: seal downgrades AllValid to Bitmap on NaN and clears the NaN bits", "[unit][chunk]") {
    std::vector<float> v(10'000);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<float>(i);
    }
    SECTION("without NaN the chunk stays AllValid") {
        OwnedChunk c = sealChunk(v);
        CHECK(c->validity() == Validity::AllValid);
        CHECK(c->bitmap() == nullptr);
        CHECK(c->validAt(9'999));
        CHECK(c->level(7)[0] == (MinMax{0.0f, 9'999.0f}));
    }
    SECTION("NaN sprinkles") {
        v[17] = std::numeric_limits<float>::quiet_NaN();
        v[9'999] = std::numeric_limits<float>::quiet_NaN();
        for (std::size_t i = 4'096; i < 4'100; ++i) {
            v[i] = std::numeric_limits<float>::quiet_NaN(); // one whole level-0 bucket
        }
        OwnedChunk c = sealChunk(v);
        CHECK(c->validity() == Validity::Bitmap);
        REQUIRE(c->bitmap() != nullptr);
        CHECK_FALSE(c->validAt(17));
        CHECK(c->validAt(16));
        CHECK_FALSE(c->validAt(9'999));
        CHECK(std::isnan(c->valueAt(17)));
        CHECK(c->valueAt(16) == 16.0);
        CHECK(c->level(0)[1024].isEmpty());
        CHECK(c->level(7)[0] == (MinMax{0.0f, 9'998.0f}));
        CHECK(c->validCount(0, 10'000) == 10'000u - 6u);
        CHECK(c->anyInvalid(4'096, 4'097));
        CHECK_FALSE(c->anyInvalid(18, 4'096));
        std::vector<std::uint8_t> mask(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            mask[i] = std::isnan(v[i]) ? 0 : 1;
        }
        checkLevels(*c, v, mask);
    }
    SECTION("F64 NaN also downgrades") {
        std::vector<double> d(100, 2.5);
        d[50] = std::numeric_limits<double>::quiet_NaN();
        OwnedChunk c = sealChunk(d);
        CHECK(c->validity() == Validity::Bitmap);
        CHECK_FALSE(c->validAt(50));
        CHECK(c->level(0)[12] == (MinMax{2.5f, 2.5f}));
    }
}

TEST_CASE("chunk: I32 invalidity is bitmap-only and keeps the raw value and wide values round outward",
          "[unit][chunk]") {
    std::vector<std::int32_t> v(64, 5);
    v[3] = 12'345;
    v[8] = 16'777'217; // 2^24 + 1 is not a float
    v[9] = -16'777'217;
    std::vector<std::uint8_t> valid(64, 1);
    valid[3] = 0;
    OwnedChunk c = sealChunk(v, valid);
    CHECK(c->validity() == Validity::Bitmap);
    CHECK_FALSE(c->validAt(3));
    CHECK(c->samples<std::int32_t>()[3] == 12'345); // raw value preserved (behaviour 3)
    CHECK(std::isnan(c->valueAt(3)));
    CHECK(c->level(0)[0] == (MinMax{5.0f, 5.0f})); // the invalid 12345 is excluded
    const MinMax wide = c->level(0)[2];            // samples 8..11
    CHECK(wide.lo == -16'777'218.0f);              // toward -inf
    CHECK(wide.hi == 16'777'218.0f);               // toward +inf
    const MinMax top = c->level(7)[0];
    CHECK(top.hi == 16'777'218.0f);
    CHECK(top.lo == -16'777'218.0f);
}

TEST_CASE("chunk: AllInvalid chunks carry no samples and report empty levels", "[unit][chunk]") {
    SECTION("allocateAllInvalid") {
        ChunkBuilder b;
        REQUIRE(b.allocateAllInvalid(1'000, Dtype::F32).ok());
        Result<OwnedChunk> c = b.seal(7);
        REQUIRE(c.ok());
        const Chunk& k = **c;
        CHECK(k.validity() == Validity::AllInvalid);
        CHECK(k.bytes() == kChunkHeaderBytes); // header only (behaviour 4)
        CHECK(k.samples<float>().empty());
        CHECK(k.version() == 7u);
        CHECK(k.count() == 1'000u);
        CHECK_FALSE(k.validAt(0));
        CHECK(std::isnan(k.valueAt(0)));
        CHECK(k.anyInvalid(0, 1));
        for (unsigned l = 0; l < kPyramidLevels; ++l) {
            const std::span<const MinMax> level = k.level(l);
            REQUIRE(level.size() == levelBuckets(l, 1'000));
            CHECK(std::all_of(level.begin(), level.end(), [](const MinMax& m) { return m.isEmpty(); }));
        }
        CHECK(MinMaxPyramid(k).query(0, 999).isEmpty());
    }
    SECTION("markAllInvalid after allocate (decode fault)") {
        ChunkBuilder b;
        REQUIRE(b.allocate(kChunkSamples, Dtype::F64, true).ok());
        b.samples<double>()[0] = 1.0;
        b.markAllInvalid();
        Result<OwnedChunk> c = b.seal(1);
        REQUIRE(c.ok());
        CHECK((*c)->validity() == Validity::AllInvalid);
        CHECK((*c)->bytes() == kChunkHeaderBytes);
        CHECK((*c)->level(0).size() == kChunkSamples / 4);
    }
}

TEST_CASE("chunk: I64 time chunks have no pyramid and are always AllValid", "[unit][chunk]") {
    ChunkBuilder b;
    CHECK_FALSE(b.allocate(10, Dtype::I64, true).ok());
    REQUIRE(b.allocate(10, Dtype::I64).ok());
    std::span<std::int64_t> s = b.samples<std::int64_t>();
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = 1'000'000'000'000LL + static_cast<std::int64_t>(i) * 7;
    }
    Result<OwnedChunk> c = b.seal(1);
    REQUIRE(c.ok());
    CHECK((*c)->builtLevels() == 0);
    CHECK((*c)->level(0).empty());
    CHECK((*c)->validity() == Validity::AllValid);
    CHECK((*c)->samples<std::int64_t>()[9] == 1'000'000'000'063LL);
    CHECK(MinMaxPyramid(**c).levels() == 0u);
}

TEST_CASE("chunk: the bitmap is LSB first with 1 = valid (the importer ABI layout)", "[unit][chunk]") {
    ChunkBuilder b;
    REQUIRE(b.allocate(100, Dtype::F32, true).ok());
    std::span<std::uint8_t> bits = b.validBits();
    REQUIRE(bits.size() == 13u);
    CHECK(bits[0] == 0xFF);
    CHECK(bits[12] == 0x0F); // samples 96..99 valid; the rest of the byte is past the end
    b.setValid(0, false);
    b.setValid(9, false);
    b.setValid(63, false);
    b.setValid(64, false);
    CHECK(bits[0] == 0xFE);
    CHECK(bits[1] == 0xFD);
    CHECK(bits[7] == 0x7F);
    CHECK(bits[8] == 0xFE);
    bits[2] = 0xF0; // an importer writing validBits directly: samples 16..19 invalid
    Result<OwnedChunk> c = b.seal(1);
    REQUIRE(c.ok());
    const Chunk& k = **c;
    CHECK_FALSE(k.validAt(0));
    CHECK_FALSE(k.validAt(16));
    CHECK(k.validAt(20));
    CHECK(k.validBits()[1] == 0xFD);
    CHECK(k.validCount(0, 100) == 100u - 8u);
}

TEST_CASE("chunk: builder argument errors and scratch reuse", "[unit][chunk]") {
    ChunkBuilder b;
    CHECK(b.allocate(0, Dtype::F32).error().code == ErrorCode::Invalid);
    CHECK(b.allocate(kChunkSamples + 1, Dtype::F32).error().code == ErrorCode::Invalid);
    CHECK(b.seal(1).error().code == ErrorCode::Invalid);
    REQUIRE(b.allocate(4, Dtype::F32).ok());
    CHECK_FALSE(b.hasBitmap());
    REQUIRE(b.addBitmap().ok());
    CHECK(b.hasBitmap());
    // A scratch seal keeps the chunk in the builder; reallocating reuses the block.
    Result<const Chunk*> s = b.sealScratch(3);
    REQUIRE(s.ok());
    CHECK((*s)->version() == 3u);
    CHECK(b.sealScratch(3).error().code == ErrorCode::Invalid); // sealed twice
    CHECK(b.allocate(4, Dtype::F32).ok());
}

TEST_CASE("chunk: directional float rounding encloses every double tightly", "[unit][chunk]") {
    CHECK(static_cast<double>(roundFloatDown(0.1)) < 0.1);
    CHECK(static_cast<double>(roundFloatUp(0.1)) > 0.1);
    CHECK(nextFloatUp(roundFloatDown(0.1)) == roundFloatUp(0.1));
    CHECK(roundFloatDown(0.5) == 0.5f);
    CHECK(roundFloatUp(0.5) == 0.5f);
    CHECK(roundFloatDown(1e300) == FLT_MAX);
    CHECK(roundFloatUp(1e300) == kInf);
    CHECK(roundFloatDown(-1e300) == -kInf);
    CHECK(roundFloatUp(-1e300) == -FLT_MAX);
    CHECK(roundFloatDown(static_cast<double>(FLT_MAX)) == FLT_MAX);
    CHECK(roundFloatUp(std::numeric_limits<double>::infinity()) == kInf);
    CHECK(roundFloatDown(std::numeric_limits<double>::infinity()) == kInf);
    CHECK(roundFloatDown(1e-50) == 0.0f);
    CHECK(roundFloatUp(1e-50) == std::numeric_limits<float>::denorm_min());
    CHECK(roundFloatDown(-1e-50) == -std::numeric_limits<float>::denorm_min());
    CHECK(signBit(roundFloatDown(-0.0)));
    CHECK(signBit(roundFloatUp(-0.0)));
    std::mt19937_64 rng(test::testSeed());
    std::uniform_real_distribution<double> mant(-1.0, 1.0);
    std::uniform_int_distribution<int> expo(-140, 140);
    for (int i = 0; i < 100'000; ++i) {
        const double x = std::ldexp(mant(rng), expo(rng));
        const float lo = roundFloatDown(x);
        const float hi = roundFloatUp(x);
        REQUIRE(static_cast<double>(lo) <= x);
        REQUIRE(static_cast<double>(hi) >= x);
        if (static_cast<double>(lo) != x) {
            REQUIRE(nextFloatUp(lo) == hi); // the tightest enclosure
        } else {
            REQUIRE(lo == hi);
        }
    }
}

TEST_CASE("chunk: MinMax folds are order-independent, including zero signs", "[unit][chunk]") {
    MinMax a;
    a.fold(0.0f);
    a.fold(-0.0f);
    MinMax b;
    b.fold(-0.0f);
    b.fold(0.0f);
    CHECK(a.bitEquals(b));
    CHECK(signBit(a.lo));
    CHECK_FALSE(signBit(a.hi));
    MinMax n;
    n.fold(std::numeric_limits<float>::quiet_NaN());
    CHECK(n.isEmpty());
    CHECK(MinMax::empty().isEmpty());
    MinMax c;
    c.fold(MinMax::empty());
    CHECK(c.isEmpty());
    CHECK(descentLevel(31, 8) == -1);
    CHECK(descentLevel(32, 8) == 0);
    CHECK(descentLevel(8 * 65'536, 8) == 7);
    CHECK(descentLevel(1'000'000'000, 8) == 7);
    CHECK(descentLevel(3) == -1);
    CHECK(descentLevel(4) == 0);
    CHECK(descentLevel(255) == 2);
}
