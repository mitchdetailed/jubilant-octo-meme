// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DistanceIndex: the per-(source, lap) distance table of D-016 (04 section 3.1, D-099 time unit, D-140 key;
// acceptance AC-M1-08: distAt(timeAt(d)) == d +- 0.25 m on 1,000 random distances per lap, and a stopped-car
// plateau returns the FIRST time each distance is reached).
//
//   distAt(t)    metres since the lap start: CorrDist(t) - CorrDist(lap.start), from the Corr Dist F64
//                column
//   timeAt(d)    SECONDS since the lap start (D-099: tAtD[] stays F64 seconds), the first time d is reached:
//                tAtD[] at kStepM = 0.25 m locates the sample segment, which is interpolated (exact between
//                grid points too, also when the car stops inside a step); DomainMapper converts with
//                llround(x * 1e6)
//   indexAt(d, rate)  the sample index (from the lap start) of timeAt(d) at `rate`
// All three are O(1). Lap stretching is a scalar held here (stretch()); the Corr Dist channel is never
// stretched (03 section 9.1 item 7). Built on a worker at the requester's priority under the task key
// distidx/<sourceId>/<layer>/<rangeId> and published as std::shared_ptr<const DistanceIndex> (01
// section 5.2).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>
#include <esses/store/series.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esses::analysis {

// The cache key of D-140: never DataSource::version, so annotations and track-section rebuilds do not
// invalidate it.
struct DistanceIndexKey {
    std::uint64_t corrDistVersion = 0; // Column::version() of the Corr Dist column
    model::MarkerId startMarker;
    model::MarkerId endMarker;
    Micros start = 0;
    Micros end = 0;
    double stretch = 1.0; // stretch settings in effect
    friend bool operator==(const DistanceIndexKey&, const DistanceIndexKey&) = default;
};

class DistanceIndex {
public:
    static constexpr double kStepM = 0.25;

    DistanceIndex() = default; // empty: lengthM() == 0, every query returns 0

    // From the Corr Dist series over the lap [key.start, key.end). Worker only: pins through Column::require
    // at `prio` (the lap's samples plus one on each side for the interpolation at the bounds). Invalid
    // distance samples hold the previous valid value (the first valid value before any). Errors: Invalid for
    // an empty lap (end <= start), NotFound when the lap holds no valid Corr Dist sample, Cancelled.
    static Result<DistanceIndex> build(const store::Series& corrDist, const DistanceIndexKey& key,
                                       Priority prio, const CancelToken& cancel);
    // From plain arrays (tests, property tests): sample times (absolute layer time, strictly increasing) and
    // cumulative distances in metres (finite). Errors: Invalid for mismatched or empty arrays, times that do
    // not increase, a non-finite distance or an empty lap.
    static Result<DistanceIndex> fromSamples(std::span<const Micros> timesUs, std::span<const double> distM,
                                             const DistanceIndexKey& key);

    // Linear interpolation between the samples; the distance at the lap bounds is interpolated too, so
    // distAt(lapStart()) == 0. Distances may decrease where the input does (a noisy logged distance).
    double distAt(Micros t) const noexcept; // metres since the lap start; t clamped to the lap
    // The first time d is reached (running maximum of the distance): linear on the sample segment that first
    // reaches d, found from tAtD[]'s 0.25 m step (a binary search over that step's segments, usually one or
    // two), so a stop inside a step does not move d
    // later than its first reach; tAtD[k] is this at k * 0.25 m. The table ends at the time lengthM() is
    // first reached.
    double timeAt(double d) const noexcept; // seconds since the lap start; d clamped to [0, length]
    // rate.indexAt(llround(timeAt(d) * 1e6), 0): the last sample at or before timeAt(d); 0 for an invalid
    // rate.
    SampleIdx indexAt(double d, Rate rate) const noexcept; // sample index from the lap start
    double lengthM() const noexcept { return lengthM_; }   // the maximum distance reached in the lap
    double stretch() const noexcept { return key_.stretch; }
    Micros lapStart() const noexcept { return key_.start; }
    Micros lapEnd() const noexcept { return key_.end; }
    std::size_t steps() const noexcept { return tAtD_.size(); }
    const DistanceIndexKey& key() const noexcept { return key_; }

private:
    static Result<DistanceIndex> fromPoints(std::vector<Micros> times, std::vector<double> dist,
                                            const DistanceIndexKey& key);

    DistanceIndexKey key_;
    double lengthM_ = 0.0;
    double tLengthS_ = 0.0;    // seconds since the lap start when lengthM_ is first reached
    std::vector<double> tAtD_; // seconds since the lap start at k * kStepM (first time reached)
    std::vector<Micros>
        timeT_;                // distAt support: the lap start, the Corr Dist samples inside the lap, the end
    std::vector<double> dAtT_; // metres since the lap start at timeT_[i]
    // timeAt support: the sample index i of each rise segment (i-1 -> i sets a new maximum), ascending, and
    // per 0.25 m step the first rise segment reaching it.
    std::vector<std::uint32_t> riseIdx_;
    std::vector<std::uint32_t> stepRise_;
    Micros stepUs_ = 0; // > 0: the inner samples are evenly spaced (O(1) distAt); 0: binary search
};

} // namespace esses::analysis
