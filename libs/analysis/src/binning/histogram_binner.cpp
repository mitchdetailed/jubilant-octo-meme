// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HistogramBinner (04 section 11.1 items 2, 5, 9; AC-M3-07).

#include "series_cursor.hpp"

#include <esses/analysis/histogram_binner.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

namespace esses::analysis {

namespace {

Failure invalid(std::string message) {
    return Failure{Error{ErrorCode::Invalid, std::move(message), {}}};
}

bool increasing(std::span<const Band> bands) noexcept {
    for (std::size_t k = 0; k < bands.size(); ++k) {
        if (!(bands[k].lo < bands[k].hi) || (k > 0 && bands[k - 1].hi > bands[k].lo)) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<HistogramResult> HistogramBinner::bin(const HistogramInput& input, const HistogramRequest& request,
                                             Priority prio, const CancelToken& cancel) {
    if (request.bins.empty()) {
        return invalid("the histogram has no bins");
    }
    if (!increasing(request.bins)) {
        return invalid("the histogram bins are not increasing");
    }
    // Bound once (bugprone-unchecked-optional-access cannot follow a check into the visitor below).
    const store::Series* gatingSeries = input.gating ? &*input.gating : nullptr;
    const store::Series* colourSeries = input.colour && !input.colourBands.empty() ? &*input.colour : nullptr;
    const bool split = colourSeries != nullptr;
    if (split && !increasing(input.colourBands)) {
        return invalid("the colour bands are not increasing");
    }
    const std::size_t nBins = request.bins.size();
    const std::size_t nBands = split ? input.colourBands.size() : 0;
    HistogramResult out;
    out.counts.assign(nBins, 0.0);
    std::vector<double> seconds(nBins, 0.0);
    std::vector<std::vector<double>> splitCounts;
    std::vector<std::vector<double>> splitSeconds;
    if (split) {
        splitCounts.assign(nBins, std::vector<double>(nBands, 0.0));
        splitSeconds.assign(nBins, std::vector<double>(nBands, 0.0));
    }
    // A uniform sample is credited one period; a timestamped one the time to the next sample, cut at the span
    // end (0 for the last sample of the series).
    const auto* uniform = std::get_if<store::UniformSeries>(&input.series);
    const double uniformPeriod =
        uniform != nullptr && uniform->rate.valid()
            ? static_cast<double>(uniform->rate.den) / static_cast<double>(uniform->rate.num)
            : 0.0;
    for (const SampleSpan& span : input.spans) {
        if (span.end <= span.start || (request.excludeUntrusted && !span.trusted)) {
            continue;
        }
        // Fresh cursors per span: spans need not be in time order.
        std::optional<detail::SeriesCursor> gating;
        if (gatingSeries != nullptr) {
            gating.emplace(*gatingSeries, store::Interp::Previous, prio, cancel);
        }
        std::optional<detail::SeriesCursor> colour;
        if (colourSeries != nullptr) {
            colour.emplace(*colourSeries, input.colourInterp, prio, cancel);
        }
        auto visited = detail::visitSamples(
            input.series, span.start, span.end, prio, cancel,
            [&](Micros t, double v, bool valid, std::optional<Micros> next) -> Result<void> {
                if (!valid) {
                    return ok();
                }
                if (gating) {
                    auto g = gating->at(t);
                    if (!g) {
                        return Failure{g.error()};
                    }
                    if (!(*g > 0.0)) { // <= 0 or invalid (NaN)
                        return ok();
                    }
                }
                const std::optional<std::size_t> b = binOf(input.toBins(v), request.bins);
                if (!b) {
                    return ok();
                }
                const double period =
                    uniform != nullptr
                        ? uniformPeriod
                        : (next ? static_cast<double>(std::min(*next, span.end) - t) / 1e6 : 0.0);
                out.counts[*b] += 1.0;
                seconds[*b] += period;
                ++out.accepted;
                out.seconds += period;
                if (colour) {
                    auto c = colour->at(t);
                    if (!c) {
                        return Failure{c.error()};
                    }
                    if (const std::optional<std::size_t> band =
                            binOf(input.colourToBands(*c), input.colourBands)) {
                        splitCounts[*b][*band] += 1.0;
                        splitSeconds[*b][*band] += period;
                    }
                }
                return ok();
            });
        if (!visited) {
            return Failure{visited.error()};
        }
    }
    const auto total = static_cast<double>(out.accepted);
    const auto inMode = [&](double count, double secs) {
        switch (request.yMode) {
        case HistogramYMode::Count:
            break;
        case HistogramYMode::Percent:
            return total > 0.0 ? 100.0 * count / total : 0.0;
        case HistogramYMode::Time:
            return secs;
        }
        return count;
    };
    out.values.resize(nBins);
    for (std::size_t b = 0; b < nBins; ++b) {
        out.values[b] = inMode(out.counts[b], seconds[b]);
    }
    if (split) {
        out.colourSplit.assign(nBins, std::vector<double>(nBands, 0.0));
        for (std::size_t b = 0; b < nBins; ++b) {
            for (std::size_t k = 0; k < nBands; ++k) {
                out.colourSplit[b][k] = inMode(splitCounts[b][k], splitSeconds[b][k]);
            }
        }
    }
    return out;
}

std::optional<std::size_t> HistogramBinner::binOf(double value, std::span<const Band> bins) noexcept {
    if (std::isnan(value) || bins.empty()) {
        return std::nullopt;
    }
    // The last bin whose lo <= value; it holds the value when value < hi (an open greaterMax band reaches
    // +inf, an open lessMin band starts at -inf).
    const auto it = std::upper_bound(bins.begin(), bins.end(), value,
                                     [](double v, const Band& band) { return v < band.lo; });
    if (it == bins.begin()) {
        return std::nullopt;
    }
    const auto k = static_cast<std::size_t>(std::prev(it) - bins.begin());
    if (value < bins[k].hi || bins[k].greaterMax) {
        return k;
    }
    return std::nullopt;
}

} // namespace esses::analysis
