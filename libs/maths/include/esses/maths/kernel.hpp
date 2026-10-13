// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Kernels (03 sections 5 and 6.1, D-022): the evaluation units a Plan is made of. Every kernel is NaN- and
// validity-aware and runs over blocks of at most kBlockSamples (4,096) samples of the result. The classes
// follow 03 section 5: Elementwise (fused EwKernel), Windowed, IIR, Stateful, Range, Variance, Table and
// External (IMathFunction).
//
// 0.1 staging (03 section 6.0, D-139): every plan is evaluated over the whole layer extent by one task;
// Elementwise subtrees still run block by block, every other stage runs sequentially over whole-extent
// buffers. ExtentMode::Parallel and Ordered and the saveState/loadState checkpoints are the v1.0 chunk path.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/store/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace esses::maths {

struct EvalContext;

inline constexpr std::uint32_t kBlockSamples = 4096; // 03 section 6.1: block size of BlockCtx

enum class KernelClass : std::uint8_t {
    Elementwise,
    Windowed,
    IIR,
    Stateful,
    Range,
    Variance,
    Table,
    External
};
enum class ExtentMode : std::uint8_t {
    Parallel, // chunk-independent (v1.0 chunk path)
    Ordered,  // chunk by chunk with state carry and checkpoints (v1.0 chunk path)
    Whole     // the whole extent before any output (every 0.1 plan)
};

// One block of samples: `n` values of `dtype` at `data` and an optional validity bitmap (LSB first; nullptr =
// all valid). Float blocks additionally carry NaN at invalid samples (03 section 6.6).
struct BlockView {
    void* data = nullptr;
    std::uint8_t* validity = nullptr;
    store::Dtype dtype = store::Dtype::F32;
    std::uint32_t n = 0;
};

// Bump allocator over caller-owned storage, reset per block (03 section 6.1 BlockCtx::temps).
struct ArenaView {
    std::span<std::byte> storage;
    std::size_t used = 0;

    // nullptr when the request does not fit; `align` must be a power of two.
    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept {
        const std::size_t start = (used + align - 1) & ~(align - 1);
        if (start > storage.size() || bytes > storage.size() - start) {
            return nullptr;
        }
        used = start + bytes;
        return storage.data() + start;
    }
    void reset() noexcept { used = 0; }
};

struct BlockCtx {
    SampleIdx first = 0;                                 // absolute index of the block's first result sample
    std::uint32_t n = 0;                                 // <= kBlockSamples
    Micros t0 = 0;                                       // time of result sample 0
    Rate rate;                                           // of the result
    std::shared_ptr<const analysis::RangeTables> ranges; // interval tables per group path (01 section 5.3)
    std::shared_ptr<const analysis::DistanceIndex> mainDist; // may be null
    std::shared_ptr<const analysis::DistanceIndex> refDist;  // may be null
    ArenaView temps;
    CancelToken cancel;
};

struct Kernel {
    Kernel() = default;
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    Kernel(Kernel&&) = delete;
    Kernel& operator=(Kernel&&) = delete;
    virtual ~Kernel() = default;

    virtual KernelClass cls() const = 0;
    // NaN/validity-aware; `in` holds the stage's inputs in Stage order, `out` has ctx.n samples.
    virtual void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) = 0;
    virtual void reset(const EvalContext& /*ctx*/) {} // Stateful/IIR: at segment/extent start
    // The outcome of the last reset()/run() (M2 addition): a kernel that cannot evaluate for a reason known
    // only at evaluation time (a cutoff at or above the Nyquist frequency of the result rate, an unknown
    // range path, Corr Dist missing) fails with its diagnostic code in context.detail; evaluateWhole stops
    // with it.
    virtual Result<void> status() const { return {}; }
    virtual std::uint32_t halo() const { return 0; } // context samples per side (Windowed)
    virtual ExtentMode extent() const { return ExtentMode::Parallel; }
};

} // namespace esses::maths
