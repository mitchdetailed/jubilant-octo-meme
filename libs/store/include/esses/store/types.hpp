// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Core types of the column store (02 section 3.1; D-009, D-011, D-012, D-054, D-062): sample dtypes,
// per-chunk validity modes, interpolation policies, sample kinds, the chunk and pyramid geometry, and the
// MinMax pyramid entry with the directional float rounding used for wider dtypes.
#pragma once

#include <esses/base/priority.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/time.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace esses::store {

using esses::Micros;
using esses::Rate;
using esses::SampleIdx;
using Priority = esses::Priority; // the TaskPool enum of 01 section 5.1; there is no second enum

// F32 default; F64 for GPS, timestamps and integrators; I32 for raw bitfields, enums and counters; I64 only
// for the exact microsecond time column of a TimestampedSeries (D-062): never a channel dtype, no pyramid,
// always AllValid.
enum class Dtype : std::uint8_t { F32, F64, I32, I64 };
enum class Validity : std::uint8_t { AllValid, Bitmap, AllInvalid };
enum class Interp : std::uint8_t { Linear, Closest, Previous, Next }; // D-009 policy names
// Descriptor-level kind (D-009). 02 section 4.1 lists it in esses_model; it is declared here because
// Resampler::defaultFor (layer 1, below esses_model) needs it. esses_model re-exports it with a using.
enum class SampleKind : std::uint8_t { Real, Integer, Enum, Bool, Bitfield, Text };

std::string_view dtypeName(Dtype d) noexcept;           // "F32", "F64", "I32", "I64"
std::string_view validityName(Validity v) noexcept;     // "AllValid", "Bitmap", "AllInvalid"
std::string_view interpName(Interp i) noexcept;         // "Linear", "Closest", "Previous", "Next"
std::string_view sampleKindName(SampleKind k) noexcept; // "Real", "Integer", ...

constexpr std::size_t dtypeSize(Dtype d) noexcept {
    return (d == Dtype::F64 || d == Dtype::I64) ? 8u : 4u;
}
constexpr bool isFloatDtype(Dtype d) noexcept {
    return d == Dtype::F32 || d == Dtype::F64;
}
constexpr bool hasPyramid(Dtype d) noexcept {
    return d != Dtype::I64;
}

// The dtype of each C++ sample type.
template <class T> struct DtypeOf;
template <> struct DtypeOf<float> {
    static constexpr Dtype value = Dtype::F32;
};
template <> struct DtypeOf<double> {
    static constexpr Dtype value = Dtype::F64;
};
template <> struct DtypeOf<std::int32_t> {
    static constexpr Dtype value = Dtype::I32;
};
template <> struct DtypeOf<std::int64_t> {
    static constexpr Dtype value = Dtype::I64;
};
template <class T> inline constexpr Dtype kDtypeOf = DtypeOf<T>::value;

// ---- chunk and pyramid geometry (02 section 3.1, 04 section 4.1)
// --------------------------------------------
inline constexpr std::uint32_t kChunkSamples = 65'536;
inline constexpr unsigned kChunkShift = 16;                    // kChunkSamples == 1 << kChunkShift
inline constexpr unsigned kPyramidBase = 4;                    // buckets 4, 16, 64, ..., 65536
inline constexpr unsigned kPyramidLevels = 8;                  // level k has bucket size 4^(k+1)
inline constexpr unsigned kSummaryFirstLevel = 3;              // bucket 256 and up form the SummaryPyramid
inline constexpr std::size_t kBitmapBytes = kChunkSamples / 8; // 8 KiB for a full chunk
inline constexpr std::uint16_t kAllLevelsBuilt = 0xFF;         // Chunk::builtLevels() after seal()
inline constexpr std::uint32_t kSummaryLeafSamples = 256;      // bucket size of kSummaryFirstLevel
static_assert(kChunkSamples == (1u << kChunkShift));

// Bucket size of pyramid level k: 4^(k+1).
constexpr std::uint32_t bucketSize(unsigned level) noexcept {
    return 4u << (2u * level);
}
// Number of buckets of level k over `count` samples; the last one may be partial.
constexpr std::uint32_t levelBuckets(unsigned level, std::uint32_t count) noexcept {
    const std::uint32_t b = bucketSize(level);
    return count / b + (count % b != 0 ? 1u : 0u);
}
// Pyramid entries of a chunk of `count` samples over all levels.
constexpr std::uint32_t pyramidEntries(std::uint32_t count) noexcept {
    std::uint32_t total = 0;
    for (unsigned k = 0; k < kPyramidLevels; ++k) {
        total += levelBuckets(k, count);
    }
    return total;
}
// Entries of the summary levels (kSummaryFirstLevel and up) of one chunk of `count` samples.
constexpr std::uint32_t summaryEntries(std::uint32_t count) noexcept {
    std::uint32_t total = 0;
    for (unsigned k = kSummaryFirstLevel; k < kPyramidLevels; ++k) {
        total += levelBuckets(k, count);
    }
    return total;
}
inline constexpr std::uint32_t kChunkPyramidEntries = pyramidEntries(kChunkSamples);
inline constexpr std::uint32_t kSummaryEntriesPerChunk = summaryEntries(kChunkSamples);
static_assert(kChunkPyramidEntries == 21'845); // 174,760 B of float pairs per full chunk (01 section 7.1)
static_assert(kSummaryEntriesPerChunk == 341); // 2,728 B per chunk

// Chunk number of sample i (floor, so negative indices map to negative chunks).
constexpr SampleIdx chunkOf(SampleIdx i) noexcept {
    return i >> kChunkShift;
}
constexpr std::uint32_t chunkLocal(SampleIdx i) noexcept {
    return static_cast<std::uint32_t>(i & static_cast<SampleIdx>(kChunkSamples - 1));
}
constexpr SampleIdx chunkFirstSample(SampleIdx chunkNo) noexcept {
    return chunkNo << kChunkShift;
}
constexpr SampleIdx chunkCountFor(SampleIdx size) noexcept {
    return size <= 0 ? 0 : ((size - 1) >> kChunkShift) + 1;
}
// Samples in chunk `chunkNo` of a column of `size` samples (only the last chunk is partial); 0 outside.
constexpr std::uint32_t chunkSampleCount(SampleIdx size, SampleIdx chunkNo) noexcept {
    if (chunkNo < 0) {
        return 0;
    }
    const SampleIdx begin = chunkFirstSample(chunkNo);
    if (begin >= size) {
        return 0;
    }
    const SampleIdx rest = size - begin;
    return rest >= static_cast<SampleIdx>(kChunkSamples) ? kChunkSamples : static_cast<std::uint32_t>(rest);
}

// The pyramid level exact M4 folds whole buckets at (04 section 4.5): the largest L with
// bucketSize(L) * minWholeBuckets <= n, capped at the top level; -1 when no level qualifies (raw scan).
// M4 passes minWholeBuckets = 8 ("at least 8 whole buckets per column"), so n < 32 gives -1.
constexpr int descentLevel(std::int64_t n, std::int64_t minWholeBuckets = 1) noexcept {
    int level = -1;
    for (unsigned k = 0; k < kPyramidLevels; ++k) {
        if (static_cast<std::int64_t>(bucketSize(k)) * minWholeBuckets <= n) {
            level = static_cast<int>(k);
        }
    }
    return level;
}
inline constexpr int kAutoLevel = -2; // descent argument: start at descentLevel(span length)
inline constexpr int kRawOnly = -1;   // descent argument: no bucket reads, every sample is read

// ---- MinMax pyramid entry
// ------------------------------------------------------------------------------------

constexpr bool signBit(float v) noexcept {
    return (std::bit_cast<std::uint32_t>(v) >> 31) != 0;
}
constexpr bool signBit(double v) noexcept {
    return (std::bit_cast<std::uint64_t>(v) >> 63) != 0;
}
constexpr bool signBit(std::int32_t v) noexcept {
    return v < 0;
}
constexpr bool signBit(std::int64_t v) noexcept {
    return v < 0;
}

// Order-independent min/max folds: -0 sorts below +0, so the result does not depend on the order in which
// equal values are folded and every decomposition of a span gives bit-identical results. A NaN operand `v`
// never changes the accumulator (both comparisons are false).
template <class T> constexpr T foldMin(T acc, T v) noexcept {
    return (v < acc || (v == acc && signBit(v))) ? v : acc;
}
template <class T> constexpr T foldMax(T acc, T v) noexcept {
    return (v > acc || (v == acc && !signBit(v))) ? v : acc;
}

// One pyramid bucket: float for every dtype (D-054, 01 section 7.1). Wider dtypes (F64, I32 above 2^24) are
// rounded directionally (lo toward -inf, hi toward +inf), so a bucket is a conservative enclosure. A bucket
// with no valid sample is the empty sentinel {+inf, -inf}, which every consumer tests with lo > hi; NaN never
// enters a bucket.
struct MinMax {
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();

    static constexpr MinMax empty() noexcept { return MinMax{}; }
    constexpr bool isEmpty() const noexcept { return lo > hi; }
    constexpr void fold(float v) noexcept {
        lo = foldMin(lo, v);
        hi = foldMax(hi, v);
    }
    constexpr void fold(const MinMax& o) noexcept {
        lo = foldMin(lo, o.lo);
        hi = foldMax(hi, o.hi);
    }
    // Value equality (-0 == +0); bitEquals compares the representation.
    friend constexpr bool operator==(const MinMax&, const MinMax&) = default;
    constexpr bool bitEquals(const MinMax& o) const noexcept {
        return std::bit_cast<std::uint32_t>(lo) == std::bit_cast<std::uint32_t>(o.lo) &&
               std::bit_cast<std::uint32_t>(hi) == std::bit_cast<std::uint32_t>(o.hi);
    }
};
static_assert(sizeof(MinMax) == 8);

// Next float toward -inf / +inf of a finite float (nextafter without the libm call).
constexpr float nextFloatDown(float f) noexcept {
    if (f == 0.0f) {
        return -std::numeric_limits<float>::denorm_min();
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(f);
    return std::bit_cast<float>(f > 0.0f ? bits - 1u : bits + 1u);
}
constexpr float nextFloatUp(float f) noexcept {
    if (f == 0.0f) {
        return std::numeric_limits<float>::denorm_min();
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(f);
    return std::bit_cast<float>(f > 0.0f ? bits + 1u : bits - 1u);
}

// The largest float <= v (v not NaN).
inline float roundFloatDown(double v) noexcept {
    constexpr double kMax = static_cast<double>(std::numeric_limits<float>::max());
    if (v > kMax) {
        return std::isinf(v) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::max();
    }
    if (v < -kMax) {
        return -std::numeric_limits<float>::infinity();
    }
    // In range: round to nearest, finite. The redundant clamp keeps MSVC from constant-folding an overflowing
    // cast in the unreachable path when v is a constant (C4756).
    const float f = static_cast<float>(v > kMax ? kMax : (v < -kMax ? -kMax : v));
    return static_cast<double>(f) > v ? nextFloatDown(f) : f;
}
// The smallest float >= v (v not NaN).
inline float roundFloatUp(double v) noexcept {
    constexpr double kMax = static_cast<double>(std::numeric_limits<float>::max());
    if (v < -kMax) {
        return std::isinf(v) ? -std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::max();
    }
    if (v > kMax) {
        return std::numeric_limits<float>::infinity();
    }
    const float f = static_cast<float>(v > kMax ? kMax : (v < -kMax ? -kMax : v)); // see roundFloatDown
    return static_cast<double>(f) < v ? nextFloatUp(f) : f;
}
// The float enclosure of the exact interval [lo, hi] (neither NaN).
inline MinMax encloseDirectional(double lo, double hi) noexcept {
    return MinMax{roundFloatDown(lo), roundFloatUp(hi)};
}

// Counters a descent can report (04 section 4.9 cost bound and tests).
struct DescentStats {
    std::uint32_t bucketReads = 0; // pyramid entries folded
    std::uint32_t rawReads = 0;    // raw samples read
};

} // namespace esses::store
