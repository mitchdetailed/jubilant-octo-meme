// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Chunk layout, ChunkBuilder and the one-pass pyramid build of ChunkBuilder::seal() (02 section 3.1).
#include <esses/store/chunk.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace esses::store {

namespace {

constexpr std::size_t pad64(std::size_t n) noexcept {
    return (n + 63u) & ~std::size_t{63};
}

void* allocBlock(std::size_t bytes) noexcept {
    return ::operator new(bytes, std::align_val_t{kChunkAlignment}, std::nothrow);
}

void freeBlock(void* block) noexcept {
    ::operator delete(block, std::align_val_t{kChunkAlignment});
}

// kChunkSamples / 4 empty sentinels: the levels of every AllInvalid chunk (level 0 is the largest).
const MinMax* emptyEntries() noexcept {
    static const std::unique_ptr<MinMax[]> entries = std::make_unique<MinMax[]>(kChunkSamples / kPyramidBase);
    return entries.get();
}

std::uint32_t bitmapWordCount(std::uint32_t count) noexcept {
    return (count + 63u) / 64u;
}

// Level 0 (bucket 4) from the samples, skipping invalid ones. Wider dtypes are folded exactly and rounded
// directionally once per bucket (rounding is monotone, so this equals folding rounded samples).
template <class T>
void buildLevel0(const T* s, std::uint32_t count, const std::uint64_t* bm, MinMax* out) noexcept {
    const std::uint32_t buckets = levelBuckets(0, count);
    for (std::uint32_t b = 0; b < buckets; ++b) {
        const std::uint32_t i0 = b * 4u;
        const std::uint32_t i1 = std::min(count, i0 + 4u);
        const unsigned bits =
            bm != nullptr ? static_cast<unsigned>((bm[i0 >> 6] >> (i0 & 63u)) & 0xFu) : 0xFu;
        if constexpr (std::is_same_v<T, float>) {
            MinMax m;
            for (std::uint32_t i = i0; i < i1; ++i) {
                if (((bits >> (i - i0)) & 1u) != 0) {
                    m.fold(s[i]);
                }
            }
            out[b] = m;
        } else {
            bool any = false;
            T lo{};
            T hi{};
            for (std::uint32_t i = i0; i < i1; ++i) {
                if (((bits >> (i - i0)) & 1u) == 0) {
                    continue;
                }
                if (!any) {
                    lo = s[i];
                    hi = s[i];
                    any = true;
                } else {
                    lo = foldMin(lo, s[i]);
                    hi = foldMax(hi, s[i]);
                }
            }
            out[b] =
                any ? encloseDirectional(static_cast<double>(lo), static_cast<double>(hi)) : MinMax::empty();
        }
    }
}

// Levels 1..7: each entry folds the (up to) four entries below it.
void reduceLevels(MinMax* pyramid, const std::array<std::uint16_t, kPyramidLevels>& start,
                  std::uint32_t count) noexcept {
    for (unsigned k = 1; k < kPyramidLevels; ++k) {
        const MinMax* below = pyramid + start[k - 1];
        const std::uint32_t nBelow = levelBuckets(k - 1, count);
        MinMax* cur = pyramid + start[k];
        const std::uint32_t n = levelBuckets(k, count);
        for (std::uint32_t j = 0; j < n; ++j) {
            MinMax m;
            const std::uint32_t t1 = std::min(nBelow, j * 4u + 4u);
            for (std::uint32_t t = j * 4u; t < t1; ++t) {
                m.fold(below[t]);
            }
            cur[j] = m;
        }
    }
}

template <class T> bool anyNaN(const T* s, std::uint32_t count) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::isnan(s[i])) {
            return true;
        }
    }
    return false;
}

template <class T> void clearNaNBits(const T* s, std::uint32_t count, std::uint64_t* bm) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::isnan(s[i])) {
            bm[i >> 6] &= ~(std::uint64_t{1} << (i & 63u));
        }
    }
}

// Mask of the bits [i, i + n) within one word, n in 1..64.
std::uint64_t wordMask(std::uint32_t bit, std::uint32_t n) noexcept {
    return n >= 64 ? ~std::uint64_t{0} : (((std::uint64_t{1} << n) - 1u) << bit);
}

} // namespace

// ---- Chunk
// ---------------------------------------------------------------------------------------------------

void ChunkDeleter::operator()(const Chunk* chunk) const noexcept {
    if (chunk != nullptr) {
        Chunk* c = const_cast<Chunk*>(chunk); // NOLINT: the chunk owns its block
        c->~Chunk();
        freeBlock(c);
    }
}

std::size_t Chunk::sampleBytes(std::uint32_t count, Dtype dtype) noexcept {
    return pad64(static_cast<std::size_t>(count) * dtypeSize(dtype));
}

std::size_t Chunk::bitmapBytes(std::uint32_t count) noexcept {
    return pad64(static_cast<std::size_t>(bitmapWordCount(count)) * sizeof(std::uint64_t));
}

std::size_t Chunk::layoutBytes(std::uint32_t count, Dtype dtype, bool bitmap) noexcept {
    std::size_t bytes = sampleBytes(count, dtype);
    if (bitmap) {
        bytes += bitmapBytes(count);
    }
    if (hasPyramid(dtype)) {
        bytes += static_cast<std::size_t>(pyramidEntries(count)) * sizeof(MinMax);
    }
    return pad64(bytes);
}

std::span<const MinMax> Chunk::level(unsigned k) const noexcept {
    if (k >= kPyramidLevels || (builtLevels_ & (1u << k)) == 0) {
        return {};
    }
    const std::uint32_t n = levelBuckets(k, count_);
    if (validity_ == Validity::AllInvalid) {
        return {emptyEntries(), n};
    }
    return {reinterpret_cast<const MinMax*>(data_ + pyramidOffset_) + levelStart_[k], n};
}

std::span<const std::uint8_t> Chunk::validBits() const noexcept {
    if (validity_ != Validity::Bitmap) {
        return {};
    }
    return {reinterpret_cast<const std::uint8_t*>(data_ + bitmapOffset_), (count_ + 7u) / 8u};
}

double Chunk::valueAt(std::uint32_t i) const noexcept {
    if (!validAt(i)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    switch (dtype_) {
    case Dtype::F32:
        return static_cast<double>(reinterpret_cast<const float*>(data_)[i]);
    case Dtype::F64:
        return reinterpret_cast<const double*>(data_)[i];
    case Dtype::I32:
        return static_cast<double>(reinterpret_cast<const std::int32_t*>(data_)[i]);
    case Dtype::I64:
        return static_cast<double>(reinterpret_cast<const std::int64_t*>(data_)[i]);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool Chunk::anyInvalid(std::uint32_t begin, std::uint32_t end) const noexcept {
    end = std::min(end, count_);
    if (begin >= end) {
        return false;
    }
    switch (validity_) {
    case Validity::AllValid:
        return false;
    case Validity::AllInvalid:
        return true;
    case Validity::Bitmap:
        break;
    }
    const std::uint64_t* bm = bitmap();
    for (std::uint32_t i = begin; i < end;) {
        const std::uint32_t bit = i & 63u;
        const std::uint32_t n = std::min(64u - bit, end - i);
        const std::uint64_t mask = wordMask(bit, n);
        if ((bm[i >> 6] & mask) != mask) {
            return true;
        }
        i += n;
    }
    return false;
}

std::uint32_t Chunk::validCount(std::uint32_t begin, std::uint32_t end) const noexcept {
    end = std::min(end, count_);
    if (begin >= end) {
        return 0;
    }
    switch (validity_) {
    case Validity::AllValid:
        return end - begin;
    case Validity::AllInvalid:
        return 0;
    case Validity::Bitmap:
        break;
    }
    const std::uint64_t* bm = bitmap();
    std::uint32_t valid = 0;
    for (std::uint32_t i = begin; i < end;) {
        const std::uint32_t bit = i & 63u;
        const std::uint32_t n = std::min(64u - bit, end - i);
        valid += static_cast<std::uint32_t>(std::popcount(bm[i >> 6] & wordMask(bit, n)));
        i += n;
    }
    return valid;
}

// ---- ChunkBuilder
// --------------------------------------------------------------------------------------------

ChunkBuilder::~ChunkBuilder() {
    release();
}

ChunkBuilder::ChunkBuilder(ChunkBuilder&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)), capacity_(std::exchange(other.capacity_, 0)),
      count_(std::exchange(other.count_, 0)), dtype_(other.dtype_),
      state_(std::exchange(other.state_, State::Empty)), bitmap_(std::exchange(other.bitmap_, false)) {}

ChunkBuilder& ChunkBuilder::operator=(ChunkBuilder&& other) noexcept {
    if (this != &other) {
        release();
        block_ = std::exchange(other.block_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        count_ = std::exchange(other.count_, 0);
        dtype_ = other.dtype_;
        state_ = std::exchange(other.state_, State::Empty);
        bitmap_ = std::exchange(other.bitmap_, false);
    }
    return *this;
}

void ChunkBuilder::release() noexcept {
    if (block_ != nullptr) {
        freeBlock(block_);
    }
    block_ = nullptr;
    capacity_ = 0;
    count_ = 0;
    state_ = State::Empty;
    bitmap_ = false;
}

void ChunkBuilder::reset() noexcept {
    state_ = State::Empty; // keeps the allocation for reuse
    bitmap_ = false;
    count_ = 0;
}

Chunk* ChunkBuilder::chunk() noexcept {
    return static_cast<Chunk*>(block_);
}

std::byte* ChunkBuilder::sampleData() noexcept {
    return static_cast<std::byte*>(block_) + kChunkHeaderBytes;
}

Result<void> ChunkBuilder::allocate(std::uint32_t count, Dtype dtype, bool wantBitmap) {
    if (count == 0 || count > kChunkSamples) {
        return fail(ErrorCode::Invalid, "chunk sample count out of range");
    }
    if (dtype == Dtype::I64 && wantBitmap) {
        return fail(ErrorCode::Invalid, "I64 time chunks are always AllValid");
    }
    const std::size_t layout = Chunk::layoutBytes(count, dtype, wantBitmap);
    const std::size_t need = kChunkHeaderBytes + layout;
    if (block_ == nullptr || capacity_ != need) { // exact size, so a handed-over chunk wastes nothing
        release();
        block_ = allocBlock(need);
        if (block_ == nullptr) {
            return fail(ErrorCode::OutOfMemory, "chunk allocation failed");
        }
        capacity_ = need;
    }
    auto* c = ::new (block_) Chunk();
    c->data_ = sampleData();
    c->count_ = count;
    c->dtype_ = dtype;
    c->validity_ = wantBitmap ? Validity::Bitmap : Validity::AllValid;
    c->layoutBytes_ = layout;
    const std::size_t sBytes = Chunk::sampleBytes(count, dtype);
    c->bitmapOffset_ = static_cast<std::uint32_t>(sBytes);
    c->pyramidOffset_ = static_cast<std::uint32_t>(sBytes + (wantBitmap ? Chunk::bitmapBytes(count) : 0u));
    std::uint32_t start = 0;
    for (unsigned k = 0; k < kPyramidLevels; ++k) {
        c->levelStart_[k] = static_cast<std::uint16_t>(start);
        start += levelBuckets(k, count);
    }
    std::memset(c->data_, 0, layout);
    if (wantBitmap) {
        auto* words = reinterpret_cast<std::uint64_t*>(c->data_ + c->bitmapOffset_);
        const std::uint32_t nWords = bitmapWordCount(count);
        std::fill(words, words + nWords, ~std::uint64_t{0});
        if ((count & 63u) != 0) {
            words[nWords - 1] = (std::uint64_t{1} << (count & 63u)) - 1u; // bits past the end are invalid
        }
    }
    count_ = count;
    dtype_ = dtype;
    bitmap_ = wantBitmap;
    state_ = State::Building;
    return ok();
}

Result<void> ChunkBuilder::allocateAllInvalid(std::uint32_t count, Dtype dtype) {
    if (count == 0 || count > kChunkSamples) {
        return fail(ErrorCode::Invalid, "chunk sample count out of range");
    }
    if (block_ == nullptr || capacity_ != kChunkHeaderBytes) {
        release();
        block_ = allocBlock(kChunkHeaderBytes);
        if (block_ == nullptr) {
            return fail(ErrorCode::OutOfMemory, "chunk allocation failed");
        }
        capacity_ = kChunkHeaderBytes;
    }
    auto* c = ::new (block_) Chunk();
    c->data_ = sampleData(); // zero-length block
    c->count_ = count;
    c->dtype_ = dtype;
    c->validity_ = Validity::AllInvalid;
    c->layoutBytes_ = 0;
    count_ = count;
    dtype_ = dtype;
    bitmap_ = false;
    state_ = State::AllInvalid;
    return ok();
}

void ChunkBuilder::markAllInvalid() noexcept {
    if (state_ != State::Building) {
        return;
    }
    Chunk* c = chunk();
    c->validity_ = Validity::AllInvalid;
    c->layoutBytes_ = 0;
    bitmap_ = false;
    state_ = State::AllInvalid;
}

Result<void> ChunkBuilder::addBitmap() {
    if (state_ != State::Building) {
        return fail(ErrorCode::Invalid, "addBitmap() without an allocated chunk");
    }
    if (bitmap_) {
        return ok();
    }
    if (dtype_ == Dtype::I64) {
        return fail(ErrorCode::Invalid, "I64 time chunks are always AllValid");
    }
    ChunkBuilder grown;
    if (auto r = grown.allocate(count_, dtype_, true); !r) {
        return r;
    }
    std::memcpy(grown.sampleData(), sampleData(), static_cast<std::size_t>(count_) * dtypeSize(dtype_));
    *this = std::move(grown);
    return ok();
}

std::span<std::byte> ChunkBuilder::sampleBytes() noexcept {
    if (state_ != State::Building) {
        return {};
    }
    return {sampleData(), static_cast<std::size_t>(count_) * dtypeSize(dtype_)};
}

std::span<std::uint64_t> ChunkBuilder::bitmapWords() noexcept {
    if (state_ != State::Building || !bitmap_) {
        return {};
    }
    Chunk* c = chunk();
    return {reinterpret_cast<std::uint64_t*>(c->data_ + c->bitmapOffset_), bitmapWordCount(count_)};
}

std::span<std::uint8_t> ChunkBuilder::validBits() noexcept {
    if (state_ != State::Building || !bitmap_) {
        return {};
    }
    Chunk* c = chunk();
    return {reinterpret_cast<std::uint8_t*>(c->data_ + c->bitmapOffset_), (count_ + 7u) / 8u};
}

void ChunkBuilder::setValid(std::uint32_t i, bool valid) noexcept {
    ESSES_ASSERT_MSG(state_ == State::Building && bitmap_ && i < count_,
                     "ChunkBuilder::setValid needs a bitmap and an index inside the chunk");
    if (state_ != State::Building || !bitmap_ || i >= count_) {
        return;
    }
    Chunk* c = chunk();
    auto* words = reinterpret_cast<std::uint64_t*>(c->data_ + c->bitmapOffset_);
    const std::uint64_t bit = std::uint64_t{1} << (i & 63u);
    if (valid) {
        words[i >> 6] |= bit;
    } else {
        words[i >> 6] &= ~bit;
    }
}

Result<void> ChunkBuilder::sealInPlace(std::uint64_t version) {
    if (state_ == State::Empty) {
        return fail(ErrorCode::Invalid, "seal() without an allocated chunk");
    }
    if (state_ == State::Sealed) {
        return fail(ErrorCode::Invalid, "seal() called twice");
    }
    if (state_ == State::AllInvalid) {
        Chunk* c = chunk();
        c->version_ = version;
        c->builtLevels_ = hasPyramid(dtype_) ? kAllLevelsBuilt : 0;
        state_ = State::Sealed;
        return ok();
    }
    // Behaviour 2: NaN is invalid regardless of mode; AllValid is downgraded to Bitmap when a NaN is present,
    // and the bitmap is made authoritative by clearing the bit of every NaN sample.
    if (isFloatDtype(dtype_)) {
        if (!bitmap_) {
            const bool nan = dtype_ == Dtype::F32
                                 ? anyNaN(reinterpret_cast<const float*>(sampleData()), count_)
                                 : anyNaN(reinterpret_cast<const double*>(sampleData()), count_);
            if (nan) {
                if (auto r = addBitmap(); !r) {
                    return r;
                }
            }
        }
        if (bitmap_) {
            auto* bm = bitmapWords().data();
            if (dtype_ == Dtype::F32) {
                clearNaNBits(reinterpret_cast<const float*>(sampleData()), count_, bm);
            } else {
                clearNaNBits(reinterpret_cast<const double*>(sampleData()), count_, bm);
            }
        }
    }
    Chunk* c = chunk();
    c->version_ = version;
    if (hasPyramid(dtype_)) {
        auto* pyramid = reinterpret_cast<MinMax*>(c->data_ + c->pyramidOffset_);
        const std::uint64_t* bm =
            bitmap_ ? reinterpret_cast<const std::uint64_t*>(c->data_ + c->bitmapOffset_) : nullptr;
        switch (dtype_) {
        case Dtype::F32:
            buildLevel0(reinterpret_cast<const float*>(c->data_), count_, bm, pyramid);
            break;
        case Dtype::F64:
            buildLevel0(reinterpret_cast<const double*>(c->data_), count_, bm, pyramid);
            break;
        case Dtype::I32:
            buildLevel0(reinterpret_cast<const std::int32_t*>(c->data_), count_, bm, pyramid);
            break;
        case Dtype::I64:
            break;
        }
        reduceLevels(pyramid, c->levelStart_, count_);
        c->builtLevels_ = kAllLevelsBuilt;
    } else {
        c->builtLevels_ = 0;
    }
    state_ = State::Sealed;
    return ok();
}

Result<OwnedChunk> ChunkBuilder::seal(std::uint64_t version) {
    if (auto r = sealInPlace(version); !r) {
        return Failure{r.error()};
    }
    if (chunk()->validity_ == Validity::AllInvalid && capacity_ > kChunkHeaderBytes) {
        // A chunk marked AllInvalid after allocate(): hand over a header-only block (behaviour 4).
        void* small = allocBlock(kChunkHeaderBytes);
        if (small == nullptr) {
            return fail(ErrorCode::OutOfMemory, "chunk allocation failed");
        }
        const Chunk* old = chunk();
        auto* c = ::new (small) Chunk();
        c->data_ = static_cast<std::byte*>(small) + kChunkHeaderBytes;
        c->version_ = old->version_;
        c->count_ = old->count_;
        c->dtype_ = old->dtype_;
        c->validity_ = Validity::AllInvalid;
        c->builtLevels_ = old->builtLevels_;
        release();
        return OwnedChunk(c);
    }
    OwnedChunk out(chunk());
    block_ = nullptr;
    capacity_ = 0;
    count_ = 0;
    state_ = State::Empty;
    bitmap_ = false;
    return out;
}

Result<const Chunk*> ChunkBuilder::sealScratch(std::uint64_t version) {
    if (auto r = sealInPlace(version); !r) {
        return Failure{r.error()};
    }
    return static_cast<const Chunk*>(chunk());
}

} // namespace esses::store
