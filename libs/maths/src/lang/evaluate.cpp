// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The 0.1 whole-node evaluation of a Plan (03 section 6.0, D-139) and MathInputGatherer::fill (03
// section 6.3).
//
// evaluateWhole() is a PlanStream (plan_stream.hpp, D-191) pulled from the first result sample to the last:
//   1. every PlanInput is gathered through EvalContext::getChannel at the plan's interpolation (the host
//   builds
//      the gatherer for the key, unit and rate; the interpolation is the plan's, so smooth's first argument
//      is resampled Linear) into F64 windows with NaN at invalid samples;
//   2. the stages run in order of need: a Streamable stage steps through the extent in pieces with its state
//      carried, a whole-extent stage (03 section 6.0 item 3) runs once over the whole extent; every stage was
//      reset(ctx) first, and a kernel whose status() fails ends the evaluation with that error (its code in
//      context.detail);
//   3. the last stage is the result, rounded to the plan's output dtype (F32 values as F32 would store them),
//      with one validity flag per sample (empty when every sample is valid). A sample that is not finite
//      after the rounding (an overflow to +-infinity in any stage or in the F32 cast) is invalid (03 sections
//      5.1 and 6.6, D-185).
// The CancelToken of the context is checked between pieces and gathers; a cancelled evaluation fails with
// Cancelled.

#include "plan_stream.hpp"

#include <esses/maths/eval_context.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/plan.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace esses::maths {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void setBit(std::uint8_t* bits, std::uint64_t i, bool valid) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << (i & 7u));
    bits[i >> 3] = valid ? static_cast<std::uint8_t>(bits[i >> 3] | bit)
                         : static_cast<std::uint8_t>(bits[i >> 3] & ~bit);
}

// Sets the validity bits [from, from + count) to `valid`, whole bytes at a time where it can.
void setBits(std::uint8_t* bits, std::uint32_t from, std::uint32_t count, bool valid) noexcept {
    std::uint32_t i = from;
    const std::uint32_t end = from + count;
    while (i < end && (i & 7u) != 0) {
        setBit(bits, i++, valid);
    }
    while (i + 8 <= end) {
        bits[i >> 3] = valid ? 0xFF : 0x00;
        i += 8;
    }
    while (i < end) {
        setBit(bits, i++, valid);
    }
}

void writeSample(const BlockView& out, std::uint32_t k, double v) noexcept {
    if (out.dtype == store::Dtype::F32) {
        static_cast<float*>(out.data)[k] = static_cast<float>(v);
    } else {
        static_cast<double*>(out.data)[k] = v;
    }
    if (out.validity != nullptr) {
        setBit(out.validity, k, !std::isnan(v));
    }
}

template <class Out, class In>
void copyValues(Out* dst, const In* src, std::uint32_t count, const units::Converter& conv) noexcept {
    if (conv.identity()) {
        for (std::uint32_t j = 0; j < count; ++j) {
            dst[j] = static_cast<Out>(src[j]);
        }
    } else {
        const double a = conv.a;
        const double b = conv.b;
        for (std::uint32_t j = 0; j < count; ++j) {
            dst[j] = static_cast<Out>(static_cast<double>(src[j]) * a + b);
        }
    }
}

template <class T> unsigned validByte(const T* v) noexcept {
    unsigned byte = 0;
    for (unsigned b = 0; b < 8; ++b) {
        byte |= (v[b] == v[b] ? 1u : 0u) << b; // not NaN
    }
    return byte;
}

// The validity bits of out[at, at + count) from the NaN marks of the values, a byte at a time where aligned.
template <class T> std::uint32_t countNaN(const T* v, std::uint32_t n) noexcept {
    std::uint32_t nans = 0;
    for (std::uint32_t j = 0; j < n; ++j) {
        nans += v[j] != v[j] ? 1u : 0u;
    }
    return nans;
}

void markValidity(const BlockView& out, std::uint32_t at, std::uint32_t count) noexcept {
    const std::uint32_t nans = out.dtype == store::Dtype::F32
                                   ? countNaN(static_cast<const float*>(out.data) + at, count)
                                   : countNaN(static_cast<const double*>(out.data) + at, count);
    if (nans == 0) {
        setBits(out.validity, at, count, true); // the common case
        return;
    }
    std::uint32_t i = at;
    const std::uint32_t end = at + count;
    const auto valueAt = [&](std::uint32_t k) {
        return out.dtype == store::Dtype::F32 ? static_cast<double>(static_cast<const float*>(out.data)[k])
                                              : static_cast<const double*>(out.data)[k];
    };
    while (i < end) {
        if ((i & 7u) == 0 && i + 8 <= end) {
            out.validity[i >> 3] = static_cast<std::uint8_t>(
                out.dtype == store::Dtype::F32 ? validByte(static_cast<const float*>(out.data) + i)
                                               : validByte(static_cast<const double*>(out.data) + i));
            i += 8;
            continue;
        }
        setBit(out.validity, i, !std::isnan(valueAt(i)));
        ++i;
    }
}

// Identity gather (same rate and phase, 02 section 3.3 rule 14): copy the pinned chunk's samples directly.
template <class T>
void copyRun(const store::Chunk& c, std::uint32_t local, std::uint32_t count, const units::Converter& conv,
             const BlockView& out, std::uint32_t at) {
    const T* src = c.samples<T>().data() + local;
    if (out.dtype == store::Dtype::F32) {
        copyValues(static_cast<float*>(out.data) + at, src, count, conv);
    } else {
        copyValues(static_cast<double*>(out.data) + at, src, count, conv);
    }
    const bool floatInput = std::is_floating_point_v<T>;
    if (c.validity() == store::Validity::Bitmap) {
        for (std::uint32_t j = 0; j < count; ++j) {
            if (!c.validAt(local + j)) {
                if (out.dtype == store::Dtype::F32) {
                    static_cast<float*>(out.data)[at + j] = std::numeric_limits<float>::quiet_NaN();
                } else {
                    static_cast<double*>(out.data)[at + j] = kNaN;
                }
            }
        }
    }
    if (out.validity == nullptr) {
        return;
    }
    if (c.validity() == store::Validity::Bitmap || floatInput) {
        markValidity(out, at, count); // NaN marks every invalid sample now (a float NaN is invalid, 03 6.6)
    } else {
        setBits(out.validity, at, count, true);
    }
}

} // namespace

Result<void> MathInputGatherer::fill(SampleIdx first, std::uint32_t n, BlockView out) {
    if (cancel_.cancelled()) {
        return fail(ErrorCode::Cancelled, "maths input gather cancelled");
    }
    if (n == 0) {
        return ok();
    }
    if (out.data == nullptr || out.n < n ||
        (out.dtype != store::Dtype::F32 && out.dtype != store::Dtype::F64)) {
        return fail(ErrorCode::Invalid,
                    "maths input gather: the output block is not an F32/F64 block of n samples");
    }
    Result<store::Resampler::PinnedInput> pins =
        store::Resampler::pinFor(series_, out_, outT0_, first, n, priority_, cancel_);
    if (!pins) {
        return Failure{pins.error()};
    }
    const auto* uniform = std::get_if<store::UniformSeries>(&series_);
    Result<store::ResampleView> view = store::Resampler::block(
        series_, out_, outT0_, first, n, interp_, pins->values, uniform != nullptr ? nullptr : &pins->times);
    if (!view) {
        return Failure{view.error()};
    }
    if (uniform != nullptr && view->identity() && !pins->values.empty()) {
        const SampleIdx size = uniform->values.size();
        std::uint32_t k = 0;
        while (k < n) {
            const SampleIdx i = first + static_cast<SampleIdx>(k) + view->identityOffset();
            const store::Chunk* c = (i >= 0 && i < size) ? pins->values.chunk(store::chunkOf(i)) : nullptr;
            if (c == nullptr) {
                writeSample(out, k, kNaN);
                ++k;
                continue;
            }
            const std::uint32_t local = store::chunkLocal(i);
            const std::uint32_t count =
                std::min<std::uint32_t>(n - k, c->count() - std::min(local, c->count()));
            if (count == 0 || c->validity() == store::Validity::AllInvalid) {
                const std::uint32_t skip = std::max<std::uint32_t>(count, 1);
                for (std::uint32_t j = 0; j < skip; ++j) {
                    writeSample(out, k + j, kNaN);
                }
                k += skip;
                continue;
            }
            switch (c->dtype()) {
            case store::Dtype::F32:
                copyRun<float>(*c, local, count, conv_, out, k);
                break;
            case store::Dtype::F64:
                copyRun<double>(*c, local, count, conv_, out, k);
                break;
            case store::Dtype::I32:
                copyRun<std::int32_t>(*c, local, count, conv_, out, k);
                break;
            case store::Dtype::I64:
                copyRun<std::int64_t>(*c, local, count, conv_, out, k);
                break;
            }
            k += count;
        }
        return ok();
    }
    const bool identity = conv_.identity();
    for (std::uint32_t k = 0; k < n; ++k) {
        double v = view->f64(static_cast<SampleIdx>(k));
        if (!identity && !std::isnan(v)) {
            v = conv_(v);
        }
        writeSample(out, k, v);
    }
    return ok();
}

Result<analysis::UniformSignal> evaluateWhole(Plan& plan, const EvalContext& ctx) {
    lang::PlanStream stream(plan, ctx);
    if (Result<void> r = stream.start(); !r) {
        return Failure{r.error()};
    }
    const std::uint64_t count = stream.size();
    analysis::UniformSignal result;
    result.rate = ctx.resultRate;
    result.t0 = stream.t0();
    result.values.resize(static_cast<std::size_t>(count));
    if (Result<void> r = stream.pull(result.values.data(), count, ctx.cancel); !r) {
        return Failure{r.error()};
    }
    // The result as the output dtype stores it; one validity flag per sample when any is invalid.
    if (lang::finaliseResult(result.values.data(), result.values.size(), plan.outDtype) > 0) {
        result.valid.resize(result.values.size());
        for (std::size_t i = 0; i < result.values.size(); ++i) {
            result.valid[i] = result.values[i] != result.values[i] ? 0 : 1; // NaN
        }
    }
    return result;
}

} // namespace esses::maths
