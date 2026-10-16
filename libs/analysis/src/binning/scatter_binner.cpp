// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterBinner (04 section 10.1 items 1, 2, 5, 9).

#include "../series_access.hpp"
#include "series_cursor.hpp"

#include <esses/analysis/scatter_binner.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::analysis {

namespace {

Failure invalid(std::string message) {
    return Failure{Error{ErrorCode::Invalid, std::move(message), {}}};
}

const store::UniformSeries* uniformOf(const store::Series& s) noexcept {
    const auto* u = std::get_if<store::UniformSeries>(&s);
    return u != nullptr && u->rate.valid() ? u : nullptr;
}

// Intervals of an X bin index beyond which a dense accumulator is not used.
constexpr std::int64_t kDenseBins = 1 << 20;

} // namespace

Result<std::vector<ScatterSeries>> ScatterBinner::gather(const ScatterInput& input, Priority prio,
                                                         const CancelToken& cancel) {
    // The union grid (04 section 10.1 item 1): the highest rate among the uniform channels (x, the y traces,
    // the colour and the gating channel; the first such channel's sample times, x first, win a tie); when no
    // channel is uniform, the x channel's own sample times (a timestamped x, D-194).
    std::optional<Rate> rate;
    Micros t0 = 0;
    const auto consider = [&](const store::Series& s) {
        if (const store::UniformSeries* u = uniformOf(s); u != nullptr && (!rate || u->rate > *rate)) {
            rate = u->rate;
            t0 = u->t0;
        }
    };
    consider(input.x.series);
    for (const ScatterChannel& y : input.y) {
        consider(y.series);
    }
    // Bound once (bugprone-unchecked-optional-access cannot follow a check into the loops below).
    const ScatterChannel* colour = input.colour ? &*input.colour : nullptr;
    const store::Series* gating = input.gating ? &*input.gating : nullptr;
    if (colour != nullptr) {
        consider(colour->series);
    }
    if (gating != nullptr) {
        consider(*gating);
    }
    const std::size_t ny = input.y.size();
    std::vector<ScatterSeries> out(ny);
    if (ny == 0) {
        return out;
    }
    for (const SampleSpan& span : input.spans) {
        if (span.end <= span.start) {
            continue;
        }
        // Fresh cursors per span: spans need not be in time order.
        detail::SeriesCursor xs(input.x.series, input.x.interp, prio, cancel);
        std::vector<detail::SeriesCursor> ys;
        ys.reserve(ny);
        for (const ScatterChannel& y : input.y) {
            ys.emplace_back(y.series, y.interp, prio, cancel);
        }
        std::optional<detail::SeriesCursor> zs;
        if (colour != nullptr) {
            zs.emplace(colour->series, colour->interp, prio, cancel);
        }
        std::optional<detail::SeriesCursor> gs;
        if (gating != nullptr) {
            gs.emplace(*gating, input.gatingInterp, prio, cancel);
        }
        // One grid time: the points of every y trace there.
        const auto visit = [&](Micros t) -> Result<void> {
            if (gs) {
                auto g = gs->at(t);
                if (!g) {
                    return Failure{g.error()};
                }
                if (!(*g > 0.0)) { // <= 0 or invalid (NaN)
                    return ok();
                }
            }
            auto xv = xs.at(t);
            if (!xv) {
                return Failure{xv.error()};
            }
            if (std::isnan(*xv)) {
                return ok();
            }
            const double xd = input.x.toDisplay(*xv);
            double zd = std::numeric_limits<double>::quiet_NaN();
            if (zs) {
                auto zv = zs->at(t);
                if (!zv) {
                    return Failure{zv.error()};
                }
                if (colour != nullptr && !std::isnan(*zv)) {
                    zd = colour->toDisplay(*zv);
                }
            }
            for (std::size_t k = 0; k < ny; ++k) {
                const Micros delay = k < input.yDelayUs.size() ? input.yDelayUs[k] : 0;
                auto yv = ys[k].at(t + delay);
                if (!yv) {
                    return Failure{yv.error()};
                }
                if (std::isnan(*yv)) {
                    continue;
                }
                ScatterSeries& s = out[k];
                s.x.push_back(xd);
                s.y.push_back(input.y[k].toDisplay(*yv));
                if (zs) {
                    s.z.push_back(zd);
                }
                s.t.push_back(t);
            }
            return ok();
        };
        if (rate) {
            const SampleIdx first = rate->indexAt(span.start - 1, t0) + 1;
            const SampleIdx last = rate->indexAt(span.end - 1, t0);
            for (SampleIdx i = first; i <= last; ++i) {
                if ((i - first) % detail::kVisitBlock == 0 && cancel.checkpoint()) {
                    return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
                }
                if (Result<void> r = visit(rate->timeAt(i, t0)); !r) {
                    return Failure{r.error()};
                }
            }
            continue;
        }
        std::vector<Micros> times;
        Result<void> listed = detail::forEachSample(input.x.series, span.start, span.end, prio, cancel,
                                                    [&times](Micros t, double, bool) { times.push_back(t); });
        if (!listed) {
            return Failure{listed.error()};
        }
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (i % detail::kVisitBlock == 0 && cancel.checkpoint()) {
                return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
            }
            if (Result<void> r = visit(times[i]); !r) {
                return Failure{r.error()};
            }
        }
    }
    return out;
}

Result<IntervalAverage> ScatterBinner::intervalAverage(std::span<const double> x, std::span<const double> y,
                                                       double binWidth) {
    if (!std::isfinite(binWidth) || !(binWidth > 0.0)) {
        return invalid("the interval average needs a positive bin width");
    }
    if (x.size() != y.size()) {
        return invalid("the interval average needs as many y values as x values");
    }
    // Bin k holds k * binWidth <= x < (k + 1) * binWidth; pairs with a non-finite value, or an x whose bin
    // index does not fit in 2^53, are skipped.
    std::vector<std::pair<std::int64_t, double>> binned;
    binned.reserve(x.size());
    std::int64_t lo = std::numeric_limits<std::int64_t>::max();
    std::int64_t hi = std::numeric_limits<std::int64_t>::min();
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            continue;
        }
        const double k = std::floor(x[i] / binWidth);
        if (!(std::abs(k) < 9007199254740992.0)) {
            continue;
        }
        const auto bin = static_cast<std::int64_t>(k);
        binned.emplace_back(bin, y[i]);
        lo = std::min(lo, bin);
        hi = std::max(hi, bin);
    }
    IntervalAverage out;
    if (binned.empty()) {
        return out;
    }
    std::vector<std::pair<std::int64_t, std::pair<double, std::size_t>>> sums; // bin, (sum, count)
    if (hi - lo < kDenseBins) {
        std::vector<double> sum(static_cast<std::size_t>(hi - lo + 1), 0.0);
        std::vector<std::size_t> count(sum.size(), 0);
        for (const auto& [bin, v] : binned) {
            const auto at = static_cast<std::size_t>(bin - lo);
            sum[at] += v;
            ++count[at];
        }
        for (std::size_t at = 0; at < sum.size(); ++at) {
            if (count[at] > 0) {
                sums.push_back({lo + static_cast<std::int64_t>(at), {sum[at], count[at]}});
            }
        }
    } else {
        std::stable_sort(binned.begin(), binned.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [bin, v] : binned) {
            if (sums.empty() || sums.back().first != bin) {
                sums.push_back({bin, {0.0, 0}});
            }
            sums.back().second.first += v;
            ++sums.back().second.second;
        }
    }
    out.binLo.reserve(sums.size());
    out.mean.reserve(sums.size());
    for (const auto& [bin, acc] : sums) {
        out.binLo.push_back(static_cast<double>(bin) * binWidth);
        out.mean.push_back(acc.first / static_cast<double>(acc.second));
    }
    return out;
}

DensityGrid ScatterBinner::density(const ScatterSeries& series, int nx, int ny, double x0, double x1,
                                   double y0, double y1) {
    DensityGrid grid;
    if (nx < 1 || ny < 1 || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) ||
        !std::isfinite(y1) || !(x1 > x0) || !(y1 > y0)) {
        return grid;
    }
    grid.nx = nx;
    grid.ny = ny;
    grid.x0 = x0;
    grid.x1 = x1;
    grid.y0 = y0;
    grid.y1 = y1;
    grid.counts.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    const std::size_t n = std::min(series.x.size(), series.y.size());
    const double sx = static_cast<double>(nx) / (x1 - x0);
    const double sy = static_cast<double>(ny) / (y1 - y0);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = series.x[i];
        const double y = series.y[i];
        // Cells are half open; the upper edges x1 and y1 belong to the last column and row.
        if (!(x >= x0 && x <= x1 && y >= y0 && y <= y1)) {
            continue;
        }
        const int cx = std::min(nx - 1, static_cast<int>((x - x0) * sx));
        const int cy = std::min(ny - 1, static_cast<int>((y - y0) * sy));
        ++grid.counts[static_cast<std::size_t>(cy) * static_cast<std::size_t>(nx) +
                      static_cast<std::size_t>(cx)];
    }
    return grid;
}

} // namespace esses::analysis
