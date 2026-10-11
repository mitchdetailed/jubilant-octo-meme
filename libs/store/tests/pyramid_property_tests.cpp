// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M0-04 and 02 section 8 item 3: MinMaxPyramid and the exact-M4 range query (hierarchical boundary
// descent) equal a brute-force reference on NaN-sprinkled random data at 50 random zooms x 10 seeds;
// SummaryPyramid equals brute force at bucket boundaries. F32 is compared bit for bit; F64 and I32 are
// compared bit for bit with the brute-force result rounded directionally to float (which is the tightest
// enclosure), and the enclosure of the exact double result is checked separately.
#include "store_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <sstream>

using namespace esses;
using namespace esses::store;

namespace {

constexpr int kSeeds = 10;
constexpr int kZoomsPerSeed = 50;
constexpr int kSpansPerSeed = 1'000; // x 10 seeds = 10^4 MinMaxPyramid spans per dtype

struct Fixture {
    ChunkCache cache{std::size_t{1} << 32};
};

std::string describe(const MinMax& m) {
    std::ostringstream os;
    os.precision(10);
    os << "{" << m.lo << ", " << m.hi << "}";
    return os.str();
}

// Per-seed column parameters: sizes with a partial last chunk, varying NaN and invalid-run densities.
struct SeedParams {
    SampleIdx size;
    double nanRate;
    double invalidRunRate;
    bool allInvalidChunk;
    bool allNaNChunk;
};

SeedParams paramsFor(int seed, std::mt19937_64& rng) {
    static constexpr double kNan[] = {0.0, 0.0005, 0.01, 0.2, 0.0, 0.001, 0.05, 0.5, 0.0001, 0.02};
    static constexpr double kRuns[] = {0.0, 0.0001, 0.00002, 0.0, 0.001, 0.0, 0.0003, 0.0, 0.00005, 0.0001};
    SeedParams p{};
    // 2-5 chunks; seed 3 ends exactly on a chunk boundary, the others have a partial last chunk.
    p.size = seed == 3 ? SampleIdx{4} * kChunkSamples : static_cast<SampleIdx>(70'000 + rng() % 230'000);
    p.nanRate = kNan[seed % 10];
    p.invalidRunRate = kRuns[seed % 10];
    p.allInvalidChunk = seed % 2 == 0;
    p.allNaNChunk = seed % 3 == 1;
    return p;
}

template <class T>
void checkEnclosure(const MinMax& got, const test::ColumnData<T>& d, const std::vector<std::uint8_t>& mask,
                    SampleIdx b, SampleIdx e, int& failures) {
    const auto exact = test::exactMinMax(d.v, mask, b, e);
    if (!exact) {
        if (!got.isEmpty()) {
            ++failures;
        }
        return;
    }
    if (!(static_cast<double>(got.lo) <= exact->first && static_cast<double>(got.hi) >= exact->second)) {
        ++failures;
    }
}

// MinMaxPyramid::query (inclusive, chunk-local) on random spans at random start levels.
template <class T> void pyramidSpans(int seed, int& checks) {
    std::mt19937_64 rng(test::testSeed() + static_cast<std::uint64_t>(seed) * 7919u + sizeof(T));
    const SeedParams p = paramsFor(seed, rng);
    test::ColumnData<T> d =
        test::randomColumn<T>(rng, p.size, p.nanRate, p.invalidRunRate, p.allInvalidChunk, p.allNaNChunk);
    const std::vector<std::uint8_t> mask = d.effectiveMask();
    Fixture fx;
    auto provider = std::make_shared<test::TestProvider<T>>(d);
    Column col(provider, ColumnOptions{&fx.cache});
    Result<ChunkHandle> h = col.require(0, col.size() - 1, Priority::Visible, {});
    REQUIRE(h.ok());
    int failures = 0;
    std::string first;
    for (int s = 0; s < kSpansPerSeed; ++s) {
        const auto c = static_cast<SampleIdx>(rng() % static_cast<std::uint64_t>(col.chunkCount()));
        const Chunk* chunk = h->chunk(c);
        REQUIRE(chunk != nullptr);
        const std::uint32_t count = chunk->count();
        // Span lengths spread over every level: log-uniform in [1, count].
        const double len = std::exp(std::uniform_real_distribution<double>(0.0, std::log(count))(rng));
        const auto n = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(len));
        const auto a = static_cast<std::uint32_t>(rng() % (count - std::min(n, count) + 1));
        const std::uint32_t b = std::min(count - 1, a + n - 1);
        const int topChoice = static_cast<int>(rng() % 10); // 0..7 explicit, 8 raw, 9 auto
        const int top = topChoice < 8 ? topChoice : (topChoice == 8 ? kRawOnly : kAutoLevel);
        DescentStats stats;
        const MinMax got = MinMaxPyramid(*chunk).query(a, b, top, &stats);
        const SampleIdx base = chunkFirstSample(c);
        const MinMax ref = test::bruteMinMax(d.v, mask, base + a, base + b + 1);
        if (!got.bitEquals(ref)) {
            if (failures++ == 0) {
                std::ostringstream os;
                os << "chunk " << c << " [" << a << ", " << b << "] top " << top << ": got " << describe(got)
                   << " want " << describe(ref);
                first = os.str();
            }
        }
        if constexpr (!std::is_same_v<T, float>) {
            checkEnclosure(got, d, mask, base + a, base + b + 1, failures);
        }
        if (top == kAutoLevel && chunk->validity() != Validity::AllInvalid) {
            // Cost: <= 7 whole buckets at the top, <= 3 per level per edge, <= 3 raw per edge.
            if (stats.bucketReads + stats.rawReads > 7u + 2u * 3u * kPyramidLevels + 6u) {
                ++failures;
            }
        }
        ++checks;
    }
    INFO(first);
    CHECK(failures == 0);
    // The default overload (auto level) is the same query.
    const Chunk* c0 = h->chunk(0);
    CHECK(
        MinMaxPyramid(*c0).query(0, c0->count() - 1).bitEquals(test::bruteMinMax(d.v, mask, 0, c0->count())));
    CHECK(MinMaxPyramid(*c0).query(10, 5).isEmpty());
    CHECK(MinMaxPyramid(*c0).query(0, 1'000'000).bitEquals(test::bruteMinMax(d.v, mask, 0, c0->count())));
}

// Exact M4 over pixel columns (04 sections 4.3-4.5): spans [ceil(s_c), ceil(s_c+1)) at random zooms and
// offsets, regime by n_c (raw below 32 samples, pyramid from descentLevel(n, 8) above), per-chunk folds for
// columns that straddle chunks.
template <class T> void m4Zooms(int seed, int& checks) {
    std::mt19937_64 rng(test::testSeed() ^ (0x9E37'79B9'7F4A'7C15ull * static_cast<std::uint64_t>(seed + 1)) ^
                        sizeof(T));
    const SeedParams p = paramsFor(seed, rng);
    test::ColumnData<T> d =
        test::randomColumn<T>(rng, p.size, p.nanRate, p.invalidRunRate, p.allInvalidChunk, p.allNaNChunk);
    const std::vector<std::uint8_t> mask = d.effectiveMask();
    Fixture fx;
    auto provider = std::make_shared<test::TestProvider<T>>(d);
    Column col(provider, ColumnOptions{&fx.cache});
    Result<ChunkHandle> h = col.require(0, col.size() - 1, Priority::Visible, {});
    REQUIRE(h.ok());
    const SampleIdx size = col.size();
    int failures = 0;
    std::string first;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int z = 0; z < kZoomsPerSeed; ++z) {
        // Zoom: W pixel columns at spp samples per column, log-uniform from 0.1 to the whole column.
        const int w = z % 10 == 0 ? 1 + static_cast<int>(rng() % 4) : 1 + static_cast<int>(rng() % 400);
        const double maxSpp = static_cast<double>(size) / w;
        const double spp = std::exp(std::log(0.1) + u01(rng) * (std::log(maxSpp) - std::log(0.1)));
        const double span = spp * w;
        // Offsets may start before 0 or end past the column (clamped like 04 section 4.3).
        const double start = -0.05 * span + u01(rng) * (static_cast<double>(size) - 0.9 * span);
        for (int c = 0; c < w; ++c) {
            const double s0 = start + c * spp;
            const double s1 = start + (c + 1) * spp;
            const SampleIdx i0 = std::clamp<SampleIdx>(static_cast<SampleIdx>(std::ceil(s0)), 0, size);
            const SampleIdx i1 = std::clamp<SampleIdx>(static_cast<SampleIdx>(std::ceil(s1)), 0, size);
            const SampleIdx n = i1 - i0;
            const int level = n >= 32 ? descentLevel(n, 8) : kRawOnly; // regimes A/B read raw samples
            DescentStats stats;
            const std::optional<MinMax> got = rangeMinMax(*h, i0, i1, level, &stats);
            if (!got) {
                ++failures;
                continue;
            }
            const MinMax ref = test::bruteMinMax(d.v, mask, i0, i1);
            if (!got->bitEquals(ref)) {
                if (failures++ == 0) {
                    std::ostringstream os;
                    os << "zoom " << z << " (W " << w << ", spp " << spp << ") column " << c << " [" << i0
                       << ", " << i1 << ") level " << level << ": got " << describe(*got) << " want "
                       << describe(ref);
                    first = os.str();
                }
            }
            if constexpr (!std::is_same_v<T, float>) {
                checkEnclosure(*got, d, mask, i0, i1, failures);
            }
            // HasInvalid (04 section 4.6 item 4) against the mask.
            bool anyInvalid = false;
            for (SampleIdx i = i0; i < i1 && !anyInvalid; ++i) {
                anyInvalid = mask[static_cast<std::size_t>(i)] == 0;
            }
            if (rangeHasInvalid(*h, i0, i1) != std::optional<bool>(anyInvalid)) {
                ++failures;
            }
            // 04 section 4.5 cost: per chunk part <= 32 whole buckets + 2 edges x (3 per level + 3 raw).
            if (n >= 32 &&
                stats.bucketReads + stats.rawReads > 2u * (32u + 2u * (3u * kPyramidLevels + 3u))) {
                ++failures;
            }
            ++checks;
        }
    }
    INFO("seed " << seed << " dtype " << dtypeName(kDtypeOf<T>) << ": " << first);
    CHECK(failures == 0);
}

// SummaryPyramid: equal to brute force when the span is aligned to 256-sample buckets, an enclosure that
// equals brute force over the bucket-expanded span otherwise; buildSummary equals the summary filled by
// sealing.
template <class T> void summarySpans(int seed, int& checks) {
    std::mt19937_64 rng(test::testSeed() + 1'000'003u * static_cast<std::uint64_t>(seed) + sizeof(T));
    const SeedParams p = paramsFor(seed, rng);
    test::ColumnData<T> d =
        test::randomColumn<T>(rng, p.size, p.nanRate, p.invalidRunRate, p.allInvalidChunk, p.allNaNChunk);
    const std::vector<std::uint8_t> mask = d.effectiveMask();
    Fixture fx;
    auto provider = std::make_shared<test::TestProvider<T>>(d);

    // Column A: summary streamed by buildSummary, nothing resident.
    Column a(provider, ColumnOptions{&fx.cache});
    const ChunkCache::Stats before = fx.cache.stats();
    REQUIRE(a.buildSummary({}).ok());
    const ChunkCache::Stats after = fx.cache.stats();
    CHECK(after.residentBytes == before.residentBytes);
    CHECK(after.chunks == before.chunks);
    const SummaryPyramid& sa = a.summary();
    CHECK(sa.complete());

    // Column B: summary filled as chunks seal through require.
    Column b(provider, ColumnOptions{&fx.cache});
    CHECK_FALSE(b.summary().complete());
    {
        Result<ChunkHandle> h = b.require(0, b.size() - 1, Priority::Background, {});
        REQUIRE(h.ok());
    }
    const SummaryPyramid& sb = b.summary();
    CHECK(sb.complete());
    int mismatches = 0;
    for (unsigned level = kSummaryFirstLevel; level < kPyramidLevels; ++level) {
        const SampleIdx buckets = (b.size() + bucketSize(level) - 1) / bucketSize(level);
        for (SampleIdx j = 0; j < buckets; ++j) {
            const std::optional<MinMax> x = sa.bucket(level, j);
            const std::optional<MinMax> y = sb.bucket(level, j);
            if (!x || !y || !x->bitEquals(*y)) {
                ++mismatches;
            }
        }
        CHECK_FALSE(sa.bucket(level, buckets + 1000).has_value());
    }
    CHECK(mismatches == 0);

    int failures = 0;
    std::string first;
    const SampleIdx size = d.size();
    const SampleIdx leaves = (size + 255) / 256;
    // Log-uniform span lengths: short spans stress the edge descent, long ones the whole-chunk levels.
    auto logLength = [&rng](SampleIdx maxLen) {
        const double l =
            std::exp(std::uniform_real_distribution<double>(0.0, std::log(static_cast<double>(maxLen)))(rng));
        return std::clamp<SampleIdx>(static_cast<SampleIdx>(l), 1, maxLen);
    };
    for (int s = 0; s < 200; ++s) {
        // Aligned: [256 g0, 256 g1) (the last bucket may end at the column end).
        const auto g0 = static_cast<SampleIdx>(rng() % static_cast<std::uint64_t>(leaves));
        const SampleIdx g1 = g0 + logLength(leaves - g0);
        const SampleIdx f = g0 * 256;
        const SampleIdx l = std::min(size, g1 * 256) - 1;
        bool covered = false;
        const MinMax got = sa.minmax(f, l, &covered);
        const MinMax ref = test::bruteMinMax(d.v, mask, f, l + 1);
        if (!covered || !got.bitEquals(ref)) {
            if (failures++ == 0) {
                std::ostringstream os;
                os << "aligned [" << f << ", " << l << "]: got " << describe(got) << " want "
                   << describe(ref);
                first = os.str();
            }
        }
        // Unaligned: encloses the exact span and equals brute force over the bucket-expanded span.
        const auto uf = static_cast<SampleIdx>(rng() % static_cast<std::uint64_t>(size));
        const SampleIdx ul = uf + logLength(size - uf) - 1;
        const MinMax ug = sa.minmax(uf, ul);
        const MinMax expanded =
            test::bruteMinMax(d.v, mask, (uf / 256) * 256, std::min(size, (ul / 256 + 1) * 256));
        if (!ug.bitEquals(expanded)) {
            ++failures;
        }
        const MinMax inner = test::bruteMinMax(d.v, mask, uf, ul + 1);
        if (!inner.isEmpty() && !(ug.lo <= inner.lo && ug.hi >= inner.hi)) {
            ++failures;
        }
        checks += 2;
    }
    INFO("seed " << seed << " dtype " << dtypeName(kDtypeOf<T>) << ": " << first);
    CHECK(failures == 0);
    CHECK(sa.minmax(size, size + 10).isEmpty());
    CHECK(sa.minmax(-10, -1).isEmpty());
    CHECK(sa.minmax(-100, size + 100).bitEquals(test::bruteMinMax(d.v, mask, 0, size)));
}

} // namespace

TEST_CASE("pyramid: MinMaxPyramid::query equals brute force on 10^4 random spans per dtype",
          "[property][pyramid]") {
    int checks = 0;
    for (int seed = 0; seed < kSeeds; ++seed) {
        pyramidSpans<float>(seed, checks);
        pyramidSpans<double>(seed, checks);
        pyramidSpans<std::int32_t>(seed, checks);
    }
    CHECK(checks == 3 * kSeeds * kSpansPerSeed);
}

TEST_CASE("pyramid: exact M4 descent equals brute force at 50 random zooms x 10 seeds (AC-M0-04)",
          "[property][pyramid]") {
    int checks = 0;
    for (int seed = 0; seed < kSeeds; ++seed) {
        m4Zooms<float>(seed, checks);
        m4Zooms<double>(seed, checks);
        m4Zooms<std::int32_t>(seed, checks);
    }
    CHECK(checks > 3 * kSeeds * kZoomsPerSeed);
}

TEST_CASE("pyramid: SummaryPyramid equals brute force at bucket boundaries (AC-M0-04)",
          "[property][pyramid]") {
    int checks = 0;
    for (int seed = 0; seed < kSeeds; ++seed) {
        summarySpans<float>(seed, checks);
        summarySpans<double>(seed, checks);
        summarySpans<std::int32_t>(seed, checks);
    }
    CHECK(checks == 3 * kSeeds * 400);
}

TEST_CASE("pyramid: range queries need pinned chunks and clamp to the column", "[unit][pyramid]") {
    ChunkCache cache(std::size_t{1} << 30);
    std::vector<float> v(3 * kChunkSamples + 100);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<float>(i % 1'000);
    }
    Column col(InMemoryProvider::fromF32(v), ColumnOptions{&cache});
    Result<ChunkHandle> h = col.require(kChunkSamples, 2 * kChunkSamples - 1, Priority::Visible, {});
    REQUIRE(h.ok());
    CHECK(h->firstChunk() == 1);
    CHECK(h->lastChunk() == 1);
    // Inside the pinned chunk.
    const std::optional<MinMax> in = rangeMinMax(*h, kChunkSamples, kChunkSamples + 2'000);
    REQUIRE(in.has_value());
    CHECK(*in == (MinMax{0.0f, 999.0f}));
    // Needs chunk 0 or 2: not pinned.
    CHECK_FALSE(rangeMinMax(*h, 0, 10).has_value());
    CHECK_FALSE(rangeMinMax(*h, kChunkSamples, 2 * kChunkSamples + 1).has_value());
    CHECK_FALSE(rangeHasInvalid(*h, 0, 10).has_value());
    // Empty spans.
    CHECK(rangeMinMax(*h, 10, 10)->isEmpty());
    CHECK(rangeMinMax(*h, 10, 5)->isEmpty());
    // The last (partial) chunk clamps a span past the column end.
    Result<ChunkHandle> tail = col.require(3 * kChunkSamples, col.size() - 1, Priority::Visible, {});
    REQUIRE(tail.ok());
    CHECK(tail->endSample() == col.size());
    const std::optional<MinMax> past = rangeMinMax(*tail, 3 * kChunkSamples + 50, col.size() + 1'000);
    REQUIRE(past.has_value());
    CHECK(*past == (MinMax{static_cast<float>((3 * kChunkSamples + 50) % 1'000),
                           static_cast<float>((col.size() - 1) % 1'000)}));
}
