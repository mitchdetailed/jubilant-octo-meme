// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Table kernel of a table2d item (03 sections 4.4 and 5.7): the stage input is the axis expression's
// value in the axis unit (the numbers are compared with the breakpoints as they are, so the axis expression
// is written with the axis unit, `'Damper Pos FL'[mm]`). Breakpoints are strictly ascending, at least 2, one
// value each; the lookup is a binary search (a uniform-spacing fast path when the breakpoints are evenly
// spaced):
//
//   linear    interpolates between the two breakpoints around x
//   next      the value at the first breakpoint >= x
//   previous  the value at the last breakpoint <= x
//
// x outside [first, last] breakpoint: clamp (default) returns the edge value, invalid gives an invalid
// sample; an invalid axis sample gives an invalid sample [compat: table.out-of-range]. The values are
// converted once from their unit into the base unit of its quantity, so the stage output is in base units
// like every maths output (interpolation commutes with the affine conversion).

#include "kernel_util.hpp"
#include "kernels_internal.hpp"
#include "stream.hpp"

#include <esses/maths/kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::maths::kern {

namespace {

class TableKernel final : public Kernel, public Streamable {
public:
    TableKernel(std::vector<double> breakpoints, std::vector<double> values, TableSpec::Interp interp,
                TableSpec::OutOfRange outOfRange)
        : x_(std::move(breakpoints)), v_(std::move(values)), interp_(interp), outOfRange_(outOfRange) {
        const double step = (x_.back() - x_.front()) / static_cast<double>(x_.size() - 1);
        uniform_ = step > 0.0;
        for (std::size_t k = 0; k < x_.size() && uniform_; ++k) {
            const double want = x_.front() + step * static_cast<double>(k);
            uniform_ = std::abs(x_[k] - want) <= 1e-12 * std::max(1.0, std::abs(want));
        }
        step_ = step;
    }

    KernelClass cls() const override { return KernelClass::Table; }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        runStreamed(*this, ctx, in, out);
    }
    void begin(const BlockCtx& /*extent*/) override {}
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override { return need; }
    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        for (std::uint64_t i = from; i < to; ++i) {
            out[i - from] = in.empty() ? kNaN : lookup(in[0][i]);
        }
    }

private:
    // The index k of the segment [x_k, x_{k+1}] holding x (x within the breakpoints).
    std::size_t segment(double x) const noexcept {
        const std::size_t last = x_.size() - 2;
        if (uniform_) {
            const double f = std::floor((x - x_.front()) / step_);
            std::size_t k = f <= 0.0 ? 0 : std::min(last, static_cast<std::size_t>(f));
            // Rounding of the division: settle on the true segment.
            while (k > 0 && x < x_[k]) {
                --k;
            }
            while (k < last && x > x_[k + 1]) {
                ++k;
            }
            return k;
        }
        const auto it = std::upper_bound(x_.begin(), x_.end(), x); // first breakpoint > x
        const auto k = static_cast<std::size_t>(it - x_.begin());
        return std::min(last, k == 0 ? 0 : k - 1);
    }

    double lookup(double x) const noexcept {
        if (isInvalid(x)) {
            return kNaN;
        }
        if (x < x_.front() || x > x_.back()) {
            if (outOfRange_ == TableSpec::OutOfRange::Invalid) {
                return kNaN;
            }
            return x < x_.front() ? v_.front() : v_.back();
        }
        const std::size_t k = segment(x);
        switch (interp_) {
        case TableSpec::Interp::Linear: {
            const double w = (x - x_[k]) / (x_[k + 1] - x_[k]);
            return v_[k] + w * (v_[k + 1] - v_[k]);
        }
        case TableSpec::Interp::Next:
            return x <= x_[k] ? v_[k] : v_[k + 1];
        case TableSpec::Interp::Previous:
            return x >= x_[k + 1] ? v_[k + 1] : v_[k];
        }
        return kNaN;
    }

    std::vector<double> x_;
    std::vector<double> v_;
    TableSpec::Interp interp_;
    TableSpec::OutOfRange outOfRange_;
    bool uniform_ = false;
    double step_ = 0.0;
};

} // namespace

KernelResult makeTableKernel(const TableSpec& table, const units::UnitRegistry& units) {
    if (table.y.has_value()) {
        return kernelError("E-KIND-UNSUPPORTED", "3D tables are evaluated from v1.0");
    }
    const std::vector<double>& x = table.x.breakpoints;
    if (x.size() < 2) {
        return kernelError("E-TABLE-AXIS", "a table needs at least 2 breakpoints");
    }
    for (std::size_t k = 0; k < x.size(); ++k) {
        if (!std::isfinite(x[k]) || (k > 0 && !(x[k] > x[k - 1]))) {
            return kernelError("E-TABLE-AXIS", "the breakpoints must be finite and strictly ascending");
        }
    }
    if (table.values.size() != x.size()) {
        return kernelError("E-TABLE-AXIS", "the table has " + std::to_string(x.size()) + " breakpoints and " +
                                               std::to_string(table.values.size()) + " values");
    }
    const units::Converter toBase =
        units.contains(table.valuesUnit) ? units.toBase(table.valuesUnit) : units::Converter{};
    std::vector<double> values;
    values.reserve(table.values.size());
    for (const double v : table.values) {
        if (!std::isfinite(v)) {
            return kernelError("E-TABLE-AXIS", "the table values must be finite");
        }
        values.push_back(toBase(v));
    }
    return std::unique_ptr<Kernel>(
        std::make_unique<TableKernel>(x, std::move(values), table.interpolation, table.outOfRange));
}

} // namespace esses::maths::kern
