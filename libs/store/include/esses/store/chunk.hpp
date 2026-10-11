// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Chunk and ChunkBuilder (02 section 3.1 behaviours 1-5, D-009, D-011, D-054, D-062).
//
// A Chunk is one 64-byte-aligned allocation: a 64-byte header, then
//   samples (count * dtypeSize, padded to 64) | [validity bitmap, padded to 64] | L0 | L1 | ... | L7
// where level k holds ceil(count / 4^(k+1)) MinMax entries (float pairs for every dtype). It is immutable
// after ChunkBuilder::seal(). Validity bitmap: bit (i % 64) of 64-bit word (i / 64), 1 = valid; on the
// little-endian targets this is byte-for-byte the C ABI's LSB-first `valid_bits` (05 section 10.1 rule 5), so
// validBits() can be handed to a plugin without a copy. After seal() the bitmap is authoritative: every NaN
// sample of a float chunk has its bit cleared.
#pragma once

#include <esses/base/assert.hpp>
#include <esses/base/result.hpp>
#include <esses/store/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace esses::store {

class Chunk;
class ChunkBuilder;

inline constexpr std::size_t kChunkAlignment = 64;
inline constexpr std::size_t kChunkHeaderBytes = 64; // the Chunk object itself, padded

// Frees a sealed chunk's single allocation. Chunks inserted into a ChunkCache are freed only by the cache,
// under its lock, at pin count zero (01 section 5.3 rule 5); an OwnedChunk that never reached a cache frees
// itself.
struct ChunkDeleter {
    void operator()(const Chunk* chunk) const noexcept;
};
using OwnedChunk = std::unique_ptr<const Chunk, ChunkDeleter>;

class Chunk {
public:
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;
    ~Chunk() = default;

    // <= kChunkSamples; only the last chunk of a column is partial.
    std::uint32_t count() const noexcept { return count_; }
    Dtype dtype() const noexcept { return dtype_; }
    Validity validity() const noexcept { return validity_; }
    // Bitmask of built pyramid levels: kAllLevelsBuilt after seal(), 0 for I64 (no pyramid).
    std::uint16_t builtLevels() const noexcept { return builtLevels_; }
    // Provider version at materialisation (part of the cache key, 02 section 3.2 behaviour 9).
    std::uint64_t version() const noexcept { return version_; }
    // samples | [bitmap] | L0 ... L7; a zero-length block for AllInvalid chunks.
    const std::byte* data() const noexcept { return data_; }

    // The samples as T (T must match dtype(), asserted); empty for AllInvalid chunks, which carry no samples.
    // Invalid I32 samples hold the raw value the producer wrote (02 section 3.1 behaviour 3).
    template <class T> std::span<const T> samples() const noexcept {
        ESSES_ASSERT_MSG(kDtypeOf<T> == dtype_, "Chunk::samples<T>() with a T that does not match the dtype");
        if (kDtypeOf<T> != dtype_ || validity_ == Validity::AllInvalid) {
            return {};
        }
        return {reinterpret_cast<const T*>(data_), count_};
    }
    // Level k (bucket 4^(k+1) samples): ceil(count / 4^(k+1)) entries. An AllInvalid chunk returns the same
    // number of empty sentinels (shared static storage); I64 chunks and k >= kPyramidLevels return {}.
    std::span<const MinMax> level(unsigned k) const noexcept;
    // nullptr unless validity() == Bitmap.
    const std::uint64_t* bitmap() const noexcept {
        return validity_ == Validity::Bitmap ? reinterpret_cast<const std::uint64_t*>(data_ + bitmapOffset_)
                                             : nullptr;
    }
    // The bitmap as bytes, LSB first, 1 = valid (ceil(count / 8) bytes); empty unless validity() == Bitmap.
    std::span<const std::uint8_t> validBits() const noexcept;
    // Mode branch, then bit test (the bitmap is authoritative after seal). False outside [0, count).
    bool validAt(std::uint32_t i) const noexcept {
        if (i >= count_) {
            return false;
        }
        switch (validity_) {
        case Validity::AllValid:
            return true;
        case Validity::AllInvalid:
            return false;
        case Validity::Bitmap:
            break;
        }
        return ((bitmap()[i >> 6] >> (i & 63u)) & 1u) != 0;
    }
    // Sample i as double (exact for F32, F64, I32; I64 exact below 2^53); NaN if invalid or out of range.
    double valueAt(std::uint32_t i) const noexcept;
    // True when [begin, end) (clamped to count) holds an invalid sample.
    bool anyInvalid(std::uint32_t begin, std::uint32_t end) const noexcept;
    // Valid samples in [begin, end) (clamped).
    std::uint32_t validCount(std::uint32_t begin, std::uint32_t end) const noexcept;

    // The whole allocation counted against the ChunkCache budget: header + samples + bitmap + pyramid; the
    // header only for an AllInvalid chunk (02 section 3.1 behaviour 4).
    std::size_t bytes() const noexcept { return kChunkHeaderBytes + layoutBytes_; }
    // Bytes after the header: samples (padded to 64) + bitmap (padded to 64) + pyramid, rounded up to 64.
    // layoutBytes(65536, F32, true) == 262,144 + 8,192 + 174,760 rounded up to 64 (AC 02 section 8 item 3).
    static std::size_t layoutBytes(std::uint32_t count, Dtype dtype, bool bitmap) noexcept;
    static std::size_t sampleBytes(std::uint32_t count, Dtype dtype) noexcept;
    static std::size_t bitmapBytes(std::uint32_t count) noexcept;

private:
    friend class ChunkBuilder;
    friend struct ChunkDeleter;
    Chunk() noexcept = default;

    std::byte* data_ = nullptr;
    std::uint64_t version_ = 0;
    std::size_t layoutBytes_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t bitmapOffset_ = 0;                         // bytes from data_
    std::uint32_t pyramidOffset_ = 0;                        // bytes from data_
    std::array<std::uint16_t, kPyramidLevels> levelStart_{}; // first entry of each level (< 21,845)
    std::uint16_t builtLevels_ = 0;
    Dtype dtype_ = Dtype::F32;
    Validity validity_ = Validity::AllValid;
};

// Produces chunks (02 section 3.1 behaviour 1): allocate(count, dtype, wantBitmap) -> write samples ->
// optionally clear valid bits -> seal(). seal() downgrades AllValid to Bitmap when a float sample is NaN
// (behaviour 2), clears the bit of every NaN sample, builds all 8 pyramid levels in one pass over the samples
// plus 7 reductions and hands the chunk over. A provider fills the builder its Column passes to
// IChunkProvider::materialise; the Column seals it. Not thread-safe; one builder per materialisation.
class ChunkBuilder {
public:
    ChunkBuilder() noexcept = default;
    ~ChunkBuilder();
    ChunkBuilder(ChunkBuilder&& other) noexcept;
    ChunkBuilder& operator=(ChunkBuilder&& other) noexcept;
    ChunkBuilder(const ChunkBuilder&) = delete;
    ChunkBuilder& operator=(const ChunkBuilder&) = delete;

    // Allocates the chunk under construction (samples zeroed; with wantBitmap an all-valid bitmap). Replaces
    // whatever the builder held; reuses the allocation when it is large enough. I64 takes no bitmap.
    Result<void> allocate(std::uint32_t count, Dtype dtype, bool wantBitmap = false);
    // A header-only chunk that seals as AllInvalid (decode fault, reader error: 05 section 2, 3.11).
    Result<void> allocateAllInvalid(std::uint32_t count, Dtype dtype);
    // Turns the chunk under construction into AllInvalid, dropping its samples.
    void markAllInvalid() noexcept;
    // Adds an all-valid bitmap to an AllValid chunk under construction (keeps the samples).
    Result<void> addBitmap();
    void reset() noexcept;

    bool allocated() const noexcept { return state_ == State::Building || state_ == State::AllInvalid; }
    std::uint32_t count() const noexcept { return count_; }
    Dtype dtype() const noexcept { return dtype_; }
    bool hasBitmap() const noexcept { return bitmap_; }
    bool allInvalid() const noexcept { return state_ == State::AllInvalid; }

    // Writable samples of the chunk under construction (T must match dtype(), asserted); empty when
    // AllInvalid.
    template <class T> std::span<T> samples() noexcept {
        ESSES_ASSERT_MSG(state_ == State::Building && kDtypeOf<T> == dtype(),
                         "ChunkBuilder::samples<T>() needs an allocated chunk of the matching dtype");
        if (state_ != State::Building || kDtypeOf<T> != dtype()) {
            return {};
        }
        return {reinterpret_cast<T*>(sampleData()), count()};
    }
    std::span<std::byte> sampleBytes() noexcept;
    // 1 = valid; empty without a bitmap.
    std::span<std::uint64_t> bitmapWords() noexcept;
    // The bitmap as bytes, LSB first, 1 = valid (ceil(count / 8) bytes): the importer ABI's validBits.
    std::span<std::uint8_t> validBits() noexcept;
    // Requires a bitmap (asserted; ignored without one).
    void setValid(std::uint32_t i, bool valid) noexcept;

    // Seals and hands the chunk over; the builder is empty afterwards. Fails only on allocation failure
    // (the NaN downgrade adds a bitmap) or when nothing was allocated.
    Result<OwnedChunk> seal(std::uint64_t version);
    // Seals in place: the chunk stays owned by the builder and is valid until the next allocate(), reset() or
    // destruction. Used by Column::buildSummary, which folds the levels and discards the samples.
    Result<const Chunk*> sealScratch(std::uint64_t version);

private:
    enum class State : std::uint8_t { Empty, Building, AllInvalid, Sealed };

    Result<void> sealInPlace(std::uint64_t version);
    std::byte* sampleData() noexcept;
    Chunk* chunk() noexcept;
    void release() noexcept;

    void* block_ = nullptr;    // header + layout, 64-byte aligned
    std::size_t capacity_ = 0; // bytes of block_
    std::uint32_t count_ = 0;
    Dtype dtype_ = Dtype::F32;
    State state_ = State::Empty; // AllInvalid: header-only intent; Sealed: block_ holds a sealed chunk
    bool bitmap_ = false;
};

} // namespace esses::store
