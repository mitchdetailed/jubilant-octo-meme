// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Resampler and ResampleView (02 section 3.3 rules 13-16, D-009): a view-layer service, never called by
// importers.
//
// Policies at output time t (rule 13), with tFirst/tLast the first and last sample time:
//   Linear    interpolates between the bracketing samples (the sample itself at an exact hit); invalid if
//   either
//             is invalid
//   Closest   the nearer sample, the earlier one on a tie
//   Previous  the last sample at or before t
//   Next      the first sample at or after t
// Outside [tFirst, tLast] every policy is invalid. The chosen sample is never replaced by a valid neighbour.
// Uniform -> uniform at the same rate and phase is an identity gather; other uniform inputs map indices with
// Rate arithmetic; timestamped inputs use one binary search and then a monotone cursor (rule 14).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>

#include <memory>
#include <vector>

namespace esses::store {

enum class SampleState : std::uint8_t { Valid, Invalid, Absent };
struct ValueSample {
    double value = 0.0;                       // NaN unless state == Valid
    SampleState state = SampleState::Invalid; // Absent: a needed chunk is not resident (now scheduled)
};

// A lazily gathered block at an output rate (no copy of the values; esses_maths wraps it as
// MathInputGatherer, 03 section 6.3). Output sample k (0 <= k < n) is at time out.timeAt(first + k, t0).
// Reads the pinned handles given to Resampler::block, which must outlive the view.
struct ResampleView {
    Rate out;
    Micros t0 = 0;
    SampleIdx first = 0;
    SampleIdx n = 0;
    Interp interp = Interp::Linear;

    float f32(SampleIdx k) const noexcept { return static_cast<float>(f64(k)); } // NaN when invalid
    double f64(SampleIdx k) const noexcept;                                      // NaN when invalid
    bool valid(SampleIdx k) const noexcept;
    Micros timeAt(SampleIdx k) const noexcept { return out.timeAt(first + k, t0); }
    // Same rate and phase as a uniform input: output k reads input sample first + k + identityOffset().
    bool identity() const noexcept { return identity_; }
    SampleIdx identityOffset() const noexcept { return identityOffset_; }

private:
    friend class Resampler;

    const ChunkHandle* values_ = nullptr;
    const ChunkHandle* times_ = nullptr; // timestamped input only
    bool uniformInput_ = true;
    bool identity_ = false;
    SampleIdx identityOffset_ = 0;
    Rate inRate_;
    Micros inT0_ = 0;
    SampleIdx inSize_ = 0;
    Micros tFirst_ = 0;
    Micros tLast_ = -1;
    // Timestamped input: for each k the last input index with time <= timeAt(k); -1 before the first sample,
    // inSize_ after the last.
    std::shared_ptr<const std::vector<SampleIdx>> prev_;
};

class Resampler {
public:
    // Single value at t, NaN if invalid or not resident. Any thread, never blocks: reads resident chunks
    // through Column::tryPin (an absent chunk is scheduled at Visible).
    static double valueAt(const Series& series, Micros t, Interp interp) noexcept;
    // As valueAt, distinguishing "invalid" from "not resident yet" (the GUI draws "..." for Absent).
    static ValueSample sampleAt(const Series& series, Micros t, Interp interp) noexcept;

    // Worker: the view of n output samples starting at output index `first` of the grid (out, t0). The caller
    // pins the input (pinFor); `pinnedTimes` is required for a timestamped series. Fails with Invalid when
    // the pins do not cover the input samples the block needs, or on a bad rate or size.
    static Result<ResampleView> block(const Series& series, Rate out, Micros t0, SampleIdx first, SampleIdx n,
                                      Interp interp, const ChunkHandle& pinnedValues,
                                      const ChunkHandle* pinnedTimes = nullptr);

    // Worker: pins exactly the input samples block() needs for this output range (values, and times for a
    // timestamped series; both empty when the range lies outside the series).
    struct PinnedInput {
        ChunkHandle values;
        ChunkHandle times;
    };
    static Result<PinnedInput> pinFor(const Series& series, Rate out, Micros t0, SampleIdx first, SampleIdx n,
                                      Priority prio, const CancelToken& cancel);

    // Real -> Linear; Integer, Enum, Bool, Bitfield, Text -> Previous (rule 15).
    static Interp defaultFor(SampleKind kind) noexcept;
};

} // namespace esses::store
