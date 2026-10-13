// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathInputGatherer (03 section 6.3): one per PlanInput. It wraps esses_store's ResampleView
// (Resampler::pinFor + Resampler::block, 02 section 3.3): fill() pins the input chunks it needs through
// Column::require (materialising an absent chunk inline on the calling worker, Rule W (a)), gathers the block
// at the result rate with the input's Interp, converts it with the input's unit Converter and writes it into
// a BlockView with validity. Worker threads only. Movable; holds the series by value (Columns are handles).
//
// M2 contract deviations from 03 section 6.3 (libs/maths/README.md): the constructor takes the series alone
// (its values Column is the `src` of 03) plus the result grid's t0, and fill() returns Result<void> so a
// cancelled or failed pin reaches the caller.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>

#include <cstdint>

namespace esses::maths {

class MathInputGatherer {
public:
    MathInputGatherer(store::Series series, Rate out, Micros outT0, store::Interp interp,
                      units::Converter conv, CancelToken cancel, Priority priority = Priority::Visible);

    // Output samples [first, first + n) of the grid (out, outT0) into `out` (out.n >= n; out.dtype F32 or
    // F64). Samples outside the input extent, and any gather touching an invalid input sample, are invalid.
    // Errors: Cancelled, Invalid (bad block), the pin's error.
    Result<void> fill(SampleIdx first, std::uint32_t n, BlockView out);

    const store::Series& series() const noexcept { return series_; }
    Rate rate() const noexcept { return out_; }
    Micros outT0() const noexcept { return outT0_; }
    store::Interp interp() const noexcept { return interp_; }
    const units::Converter& converter() const noexcept { return conv_; }
    const CancelToken& cancel() const noexcept { return cancel_; }
    Priority priority() const noexcept { return priority_; }

private:
    store::Series series_;
    Rate out_;
    Micros outT0_ = 0;
    store::Interp interp_ = store::Interp::Linear;
    units::Converter conv_;
    CancelToken cancel_;
    Priority priority_ = Priority::Visible;
};

} // namespace esses::maths
