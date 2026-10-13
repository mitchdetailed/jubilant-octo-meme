// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlanStream (03 section 6.0, D-139, D-191): the 0.1 evaluation of a Plan over the layer extent, produced
// in consecutive pieces of the result so that a node's inputs, stages and result pass through small buffers
// that stay in the cache. Each Streamable stage (src/kernels/stream.hpp) steps through the extent carrying
// its state, reading its inputs from sliding windows that keep only the context it needs on each side; a
// stage that needs its whole input first (a backward filter pass, the statistics, the Variance kernels) runs
// once over the whole extent when its output is first needed, from inputs gathered over the whole extent.
// Every output sample has the value the whole-extent evaluation gives (each kernel does the same operations
// in the same order); evaluateWhole() is a PlanStream pulled to the end, and MathGraph seals a node's chunks
// from a PlanStream as they are produced (graph/stream_provider.hpp).
//
// Inputs are gathered through EvalContext::getChannel at the plan's interpolation (smooth's first argument
// Linear) into F64 windows with NaN at invalid samples; no stage buffer carries a bitmap. Worker threads
// only. Internal to esses_maths.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/plan.hpp>
#include <esses/store/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace esses::maths::lang {

class PlanStream {
public:
    // `plan` must outlive the stream; its kernels hold the stream's state (one stream per plan at a time).
    PlanStream(Plan& plan, EvalContext ctx);
    ~PlanStream();
    PlanStream(const PlanStream&) = delete;
    PlanStream& operator=(const PlanStream&) = delete;
    PlanStream(PlanStream&&) = delete;
    PlanStream& operator=(PlanStream&&) = delete;

    // Checks the plan and the context, builds the input gatherers through EvalContext::getChannel, resets
    // every stage (Kernel::reset) and fails with the first stage's status error (E-ARG-VALUE,
    // E-RANGE-UNKNOWN,
    // ...: Kernel::status). Once, before pull().
    Result<void> start();

    // True after start() when every stage streams and can restart inside the extent
    // (kern::Streamable::restartPoint): restartAt() then serves any part of the result.
    bool restartable() const noexcept;
    // Repositions a started, restartable stream so that the next pull() returns the samples from `first`:
    // every stage restarts at its latest restart point before what its readers need from `first` on (the
    // samples between are computed and dropped). The values are those of a pull from the extent start.
    Result<void> restartAt(std::uint64_t first, const CancelToken& cancel);

    std::uint64_t size() const noexcept;     // result samples (EvalContext::resultCount)
    std::uint64_t produced() const noexcept; // result samples pulled so far
    Micros t0() const noexcept;              // time of result sample 0 (valid after start())

    // The next n result samples (from produced()) into out[0, n): the last stage's doubles, NaN = invalid,
    // before the output dtype's rounding (finaliseResult). Fails with Cancelled when `cancel` or the
    // context's token fires, or with a gather's or a whole stage's error.
    Result<void> pull(double* out, std::uint64_t n, const CancelToken& cancel);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Result samples as the output dtype stores them (03 sections 5.1, 6.5, 6.6; D-185): F32 values rounded to
// float, I32 values truncated, and every sample that is not finite after that made NaN. Returns the number of
// invalid (NaN) samples.
std::uint64_t finaliseResult(double* v, std::size_t n, store::Dtype outDtype) noexcept;

} // namespace esses::maths::lang
