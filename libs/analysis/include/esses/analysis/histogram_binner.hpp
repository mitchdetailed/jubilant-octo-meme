// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HistogramBinner (04 section 11.1 items 2, 5, 9; ARCHITECTURE_OUTLINE section 4 esses_analysis; AC-M3-07):
// one pass over the pinned chunks of a channel per trace and slot, at the channel's own rate:
//   bin = the band containing the value (Band::lo <= v < Band::hi; the open bands take what lies outside);
//   a sample in no bin (outside every band, or in a gap between arbitrary bands) is not counted;
//   values: count, percent of the accepted samples (the samples counted in some bin, so the bins sum to
//   100), or time (sum of sample periods in seconds: one period per uniform sample; a timestamped sample
//   holds until the next sample of the series, cut at the span end, and the series' last sample adds 0);
//   gating: the gating channel resampled Previous to the channel's rate (store::Resampler's rule: invalid
//   outside the gating channel's first..last sample); a sample is dropped where gating is <= 0 or invalid;
//   invalid samples are dropped; spans with trusted == false are skipped when excludeUntrusted;
//   colour split: each accepted sample also counts in the colour channel's band at its time (the colour
//   channel resampled with colourInterp; a sample whose colour value is in no band adds to no sub-bar), in
//   the same y mode as the bars (percent of all accepted samples, so the sub-bars of a bar sum to it).
// Runs on a worker (Priority::Visible, coalesced by component key); pins at most two chunks per column at a
// time. AC-M3-07: counts/percent/time equal brute force for width- and count-defined bins with gating and
// untrusted exclusion (tests/histogram_binner_tests.cpp).
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace esses::analysis {

enum class HistogramYMode : std::uint8_t { Count, Percent, Time };

struct HistogramInput {
    store::Series series;
    units::Converter toBins;             // series values (base units) -> the bins' unit
    std::optional<store::Series> gating; // dropped where <= 0 or invalid
    std::optional<store::Series> colour; // colour channel (sub-bars)
    units::Converter colourToBands;      // colour values -> the colour bands' unit
    std::vector<Band> colourBands;
    std::vector<SampleSpan> spans;                      // what to read
    store::Interp colourInterp = store::Interp::Linear; // the colour channel's own policy
};

struct HistogramRequest {
    std::vector<Band> bins; // increasing; the open bands first/last when present
    HistogramYMode yMode = HistogramYMode::Count;
    bool excludeUntrusted = true;
};

struct HistogramResult {
    std::vector<double> counts; // per bin
    std::vector<double> values; // per bin in yMode
    // Per bin, per colour band (yMode values); empty without a colour channel.
    std::vector<std::vector<double>> colourSplit;
    std::uint64_t accepted = 0; // samples counted (gating and trust applied)
    double seconds = 0.0;       // their total period
};

class HistogramBinner {
public:
    // Worker only (pins through Column::require at `prio`). Invalid for no bins or bins (or colour bands)
    // that are not increasing; Cancelled; the Column::require errors.
    static Result<HistogramResult> bin(const HistogramInput& input, const HistogramRequest& request,
                                       Priority prio, const CancelToken& cancel);
    // The bin of a value in increasing bins (binary search); nullopt outside every bin (no open band), in a
    // gap between bins, or NaN.
    static std::optional<std::size_t> binOf(double value, std::span<const Band> bins) noexcept;
};

} // namespace esses::analysis
