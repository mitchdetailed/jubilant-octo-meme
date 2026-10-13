// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Streamed kernel steps (D-191): the 0.1 evaluator (src/lang/plan_stream.hpp) runs a plan over the layer
// extent in consecutive pieces of a few thousand samples, so a node's inputs, intermediate stages and result
// stay in the cache instead of travelling through whole-extent buffers (03 section 6.0 keeps its semantics:
// every output sample is the value one whole-extent run gives, bit for bit).
//
// A kernel that can do this implements Streamable beside Kernel: begin() at the extent start, then step()
// for outputs [from, to) with from = the previous step's to, carrying its state (running sums, filter state,
// latches) from one step to the next. A step reads its inputs at absolute sample indices (0 = the extent's
// first sample) in [from - behind(), to + ahead()) clamped to the extent; the evaluator keeps that much of
// every input. The values are doubles with NaN at invalid samples (no bitmap: in the evaluator's buffers a
// NaN is exactly an invalid sample, 03 section 6.6). Kernel::run() of such a kernel is one step over the
// whole view (runStreamed), so there is one implementation of each kernel's arithmetic.
//
// A kernel that needs its whole input before any output (a backward filter pass, the two-pass statistics,
// the Variance kernels) is not Streamable, or answers canStream() == false after reset(); the evaluator runs
// it once over the whole extent. Internal to esses_maths.
#pragma once

#include <esses/maths/kernel.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace esses::maths::kern {

// Samples [base, end) of one stage input over the evaluated extent (data[0] is sample `base`).
struct StreamInput {
    const double* data = nullptr;
    std::uint64_t base = 0;
    std::uint64_t end = 0;

    double operator[](std::uint64_t i) const noexcept { return data[i - base]; }
    const double* at(std::uint64_t i) const noexcept { return data + (i - base); }
};

class Streamable {
public:
    Streamable() = default;
    Streamable(const Streamable&) = delete;
    Streamable& operator=(const Streamable&) = delete;
    Streamable(Streamable&&) = delete;
    Streamable& operator=(Streamable&&) = delete;
    virtual ~Streamable() = default;

    // After Kernel::reset(): false when this configuration needs the whole extent at once.
    virtual bool canStream() const noexcept { return true; }
    // At the extent start, after Kernel::reset(): `extent` describes every sample of the evaluation (first,
    // n, t0, rate, ranges, distance tables, cancel token).
    virtual void begin(const BlockCtx& extent) = 0;
    // Valid after begin(): the input context a step needs on each side of its outputs.
    virtual std::uint64_t behind() const noexcept { return 0; }
    virtual std::uint64_t ahead() const noexcept { return 0; }
    // Outputs [from, to) into out[0 .. to - from), NaN = invalid. The inputs follow Stage::inputs and hold at
    // least the samples [from - behind(), to + ahead()) of the extent.
    virtual void step(const BlockCtx& extent, std::span<const StreamInput> in, double* out,
                      std::uint64_t from, std::uint64_t to) = 0;

    // Restarting inside the extent (D-191: the chunks of a node whose stages all restart are sealed in
    // parallel). Valid after begin(): the largest sample s <= `need` such that beginAt(extent, s) and steps
    // from s give, from s on, exactly the outputs of steps from 0 (the kernel's state at s does not depend on
    // the samples before it); nullopt when the kernel carries state along the whole extent.
    virtual std::optional<std::uint64_t> restartPoint(std::uint64_t /*need*/) const noexcept {
        return std::nullopt;
    }
    // begin() for a first step at `first`, a value restartPoint() returned.
    virtual void beginAt(const BlockCtx& extent, std::uint64_t /*first*/) { begin(extent); }
};

// Kernel::run() of a Streamable kernel: begin() and one step over the whole view. Inputs of any dtype (and
// bitmap) are read as doubles with NaN at invalid samples; the output goes through Writer (its bitmap from
// the NaN marks).
void runStreamed(Streamable& kernel, BlockCtx& ctx, std::span<const BlockView> in, BlockView out);

// True when none of the n doubles at p is NaN (a vectorisable scan the kernels use to pick their fast loops).
bool noNaN(const double* p, std::uint64_t n) noexcept;

} // namespace esses::maths::kern
