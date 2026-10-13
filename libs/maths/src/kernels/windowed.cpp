// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Windowed kernels (03 section 5.2, D-022): smooth and derivative over a window of n points at the result
// rate (an IntLit counts points, a DecLit is seconds converted to an odd n; n of 0 or 1 is 2). Odd windows
// are centred ([i - h, i + h]), even windows biased one point earlier ([i - n/2, i + n/2 - 1]); at the extent
// edges the window is truncated to the samples that exist and invalid samples are left out [compat:
// smooth.edges, derivative.edges in docs/maths-compat.md].
//
//   smooth      the mean of the window's valid samples; no valid sample -> invalid.
//   derivative  the least-squares slope through the window's valid points: against time (per second; the
//               default n = 2 gives (x[i] - x[i-1]) * rate) or, in the form derivative(x, y[, n]), against
//               the per-sample y (only points where both are valid); fewer than 2 points, or no spread of
//               the abscissa (dy = 0), -> invalid.
//
// Accumulators are F64. Wide windows slide running sums and re-sum the window from scratch every
// max(4,096, n) outputs counted from the extent start (03 section 5.2: bounded drift, deterministic);
// derivative windows of at most 8 points are summed directly. Sums are taken about a base value re-chosen at
// every re-summation so the least-squares terms do not cancel.
//
// Both kernels are Streamable (stream.hpp, D-191): the sliding state carries from one step to the next and
// the re-summation points stay multiples of the period from the extent start, so a streamed evaluation gives
// the whole-extent values bit for bit. Where a step's input has no invalid sample, the interior outputs take
// loops that do the same arithmetic without the validity tests: smooth's add-one-drop-one update, and the
// two-point derivative's sums written out for its two always-usable points.

#include "kernel_util.hpp"
#include "kernels_internal.hpp"
#include "stream.hpp"

#include <esses/maths/kernel.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace esses::maths::kern {

namespace {

constexpr std::uint64_t kResum = 4096;
constexpr std::uint64_t kDirectMax = 8; // derivative windows summed directly

struct Bounds {
    std::uint64_t a = 0; // first sample of the window
    std::uint64_t b = 0; // last sample of the window (inclusive)
};

Bounds boundsAt(std::uint64_t i, std::uint64_t n, const WindowShape& w) noexcept {
    Bounds out;
    out.a = i > w.before ? i - w.before : 0;
    out.b = std::min(n - 1, i + w.after);
    return out;
}

WindowSpec windowArg(const Ast& ast, std::uint32_t call, std::size_t k) {
    WindowSpec w;
    if (const Node* n = argNode(ast, call, k); isNumberLiteral(n)) {
        w.seconds = n->kind == NodeKind::DecLit;
        w.value = n->number;
        w.given = true;
    }
    return w;
}

std::uint32_t haloOf(const WindowSpec& w) noexcept {
    if (w.seconds) {
        return 0; // known only at the result rate
    }
    const WindowShape s = windowShape(windowPoints(w, Rate{}));
    const std::uint64_t h = std::max(s.before, s.after);
    return h > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(h);
}

class SmoothKernel final : public Kernel, public Streamable {
public:
    explicit SmoothKernel(WindowSpec window) : window_(window) {}

    KernelClass cls() const override { return KernelClass::Windowed; }
    std::uint32_t halo() const override { return haloOf(window_); }
    ExtentMode extent() const override { return ExtentMode::Whole; }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        runStreamed(*this, ctx, in, out);
    }

    void begin(const BlockCtx& extent) override {
        n_ = extent.n;
        const std::uint64_t points = windowPoints(window_, extent.rate);
        shape_ = windowShape(points);
        resum_ = std::max(kResum, points);
        nextResum_ = 0;
        sum_ = 0.0;
        count_ = 0;
        cur_ = Bounds{};
    }
    // The sliding update drops the sample one before the window.
    std::uint64_t behind() const noexcept override { return shape_.before + 1; }
    std::uint64_t ahead() const noexcept override { return shape_.after; }
    // Every re-summation point starts from scratch.
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override {
        return need / resum_ * resum_;
    }
    void beginAt(const BlockCtx& extent, std::uint64_t first) override {
        begin(extent);
        nextResum_ = first;
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty()) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        const StreamInput& x = in[0];
        const std::uint64_t lo = from > shape_.before + 1 ? from - shape_.before - 1 : 0;
        const std::uint64_t hi = std::min(n_, to + shape_.after);
        const bool clean = noNaN(x.at(lo), hi - lo);
        std::uint64_t i = from;
        while (i < to) {
            if (clean && i != nextResum_ && i > shape_.before && i + shape_.after < n_) {
                // Interior outputs without an invalid sample: the window gains x[i + after] and loses
                // x[i - before - 1] and the count stays (the update of at(), in the same order).
                const std::uint64_t stop = std::min({to, nextResum_, n_ - shape_.after});
                const double* add = x.at(i + shape_.after);
                const double* drop = x.at(i - shape_.before - 1);
                const auto count = static_cast<double>(count_);
                double sum = sum_;
                double* o = out + (i - from);
                const std::uint64_t m = stop - i;
                for (std::uint64_t k = 0; k < m; ++k) {
                    sum += add[k];
                    sum -= drop[k];
                    o[k] = sum / count;
                }
                sum_ = sum;
                cur_ = Bounds{stop - 1 - shape_.before, stop - 1 + shape_.after};
                i = stop;
                continue;
            }
            out[i - from] = at(x, i);
            ++i;
        }
    }

private:
    // Output i, the outputs before it done in order; any validity.
    double at(const StreamInput& x, std::uint64_t i) noexcept {
        const Bounds want = boundsAt(i, n_, shape_);
        if (i == nextResum_) {
            nextResum_ += resum_;
            sum_ = 0.0;
            count_ = 0;
            for (std::uint64_t j = want.a; j <= want.b; ++j) {
                const double v = x[j];
                if (!isInvalid(v)) {
                    sum_ += v;
                    ++count_;
                }
            }
            cur_ = want;
        } else {
            while (cur_.b < want.b) {
                ++cur_.b;
                const double v = x[cur_.b];
                if (!isInvalid(v)) {
                    sum_ += v;
                    ++count_;
                }
            }
            while (cur_.a < want.a) {
                const double v = x[cur_.a];
                if (!isInvalid(v)) {
                    sum_ -= v;
                    --count_;
                }
                ++cur_.a;
            }
        }
        return count_ > 0 ? sum_ / static_cast<double>(count_) : kNaN;
    }

    WindowSpec window_;
    std::uint64_t n_ = 0;
    WindowShape shape_;
    std::uint64_t resum_ = kResum;
    std::uint64_t nextResum_ = 0; // the next output that re-sums its window from scratch
    double sum_ = 0.0;
    std::uint64_t count_ = 0;
    Bounds cur_;
};

// Least-squares sums about a base point (u = abscissa - baseU, v = ordinate - baseV).
struct LsSums {
    double c = 0.0;
    double su = 0.0;
    double sv = 0.0;
    double suu = 0.0;
    double suv = 0.0;

    void add(double u, double v) noexcept {
        c += 1.0;
        su += u;
        sv += v;
        suu += u * u;
        suv += u * v;
    }
    void remove(double u, double v) noexcept {
        c -= 1.0;
        su -= u;
        sv -= v;
        suu -= u * u;
        suv -= u * v;
    }
    // NaN when fewer than two points or no spread of the abscissa.
    double slope() const noexcept {
        if (c < 2.0) {
            return kNaN;
        }
        const double den = c * suu - su * su;
        if (!(den > 1e-12 * c * suu)) {
            return kNaN;
        }
        return (c * suv - su * sv) / den;
    }
};

class DerivativeKernel final : public Kernel, public Streamable {
public:
    DerivativeKernel(WindowSpec window, bool againstChannel)
        : window_(window), againstChannel_(againstChannel) {}

    KernelClass cls() const override { return KernelClass::Windowed; }
    std::uint32_t halo() const override { return haloOf(window_); }
    ExtentMode extent() const override { return ExtentMode::Whole; }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        runStreamed(*this, ctx, in, out);
    }

    void begin(const BlockCtx& extent) override {
        n_ = extent.n;
        points_ = window_.given ? windowPoints(window_, extent.rate) : 2;
        shape_ = windowShape(points_);
        perSecond_ = againstChannel_ ? 1.0 : extent.rate.hz();
        resum_ = std::max(kResum, points_);
        nextResum_ = 0;
        sums_ = LsSums{};
        cur_ = Bounds{};
        baseU_ = 0.0;
        baseV_ = 0.0;
    }
    std::uint64_t behind() const noexcept override {
        return points_ <= kDirectMax ? shape_.before : shape_.before + 1;
    }
    std::uint64_t ahead() const noexcept override { return shape_.after; }
    // The direct form keeps no state; the sliding form starts from scratch at every re-summation point.
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override {
        return points_ <= kDirectMax ? need : need / resum_ * resum_;
    }
    void beginAt(const BlockCtx& extent, std::uint64_t first) override {
        begin(extent);
        nextResum_ = points_ <= kDirectMax ? 0 : first;
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty() || (againstChannel_ && in.size() < 2)) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        const StreamInput& x = in[0];
        const StreamInput& y = againstChannel_ ? in[1] : in[0];
        std::uint64_t i = from;
        if (points_ == 2 && !againstChannel_ && to > 1) {
            const std::uint64_t first = std::max<std::uint64_t>(from, 1);
            if (noNaN(x.at(first - 1), to - first + 1)) {
                for (; i < first; ++i) {
                    out[i - from] = direct(x, y, i);
                }
                twoPoint(x.at(first - 1), out + (first - from), to - first);
                return;
            }
        }
        for (; i < to; ++i) {
            out[i - from] = points_ <= kDirectMax ? direct(x, y, i) : sliding(x, y, i);
        }
    }

private:
    bool usable(const StreamInput& x, const StreamInput& y, std::uint64_t j) const noexcept {
        return !isInvalid(x[j]) && (!againstChannel_ || !isInvalid(y[j]));
    }
    double abscissa(const StreamInput& y, std::uint64_t j) const noexcept {
        return againstChannel_ ? y[j] : static_cast<double>(j);
    }

    // The default derivative against time over outputs [first, first + m), first >= 1, where x[first - 1 ..
    // first + m - 1] are all valid: direct()'s two passes over the window {i - 1, i} with both points usable
    // (c = 2), without the tests and with the abscissa terms evaluated. For i < 2^52 (the extent has fewer
    // than 2^32 samples) mu = ((i - 1) + i) / 2 = i - 0.5 exactly, so du is -0.5 and 0.5 exactly, suu =
    // 0.25 + 0.25 = 0.5 and suv / suu = suv * 2 exactly; the products with +-0.5 and the sums of mv and suv
    // are direct()'s own operations in its order, and x / 2 = x * 0.5 exactly. So every output is the value
    // direct() computes, bit for bit (the --digest of esses-maths-bench and the kernel property tests
    // compare them).
    void twoPoint(const double* x, double* out, std::uint64_t m) const noexcept {
        const double perSecond = perSecond_;
        for (std::uint64_t k = 0; k < m; ++k) {
            const double x0 = x[k];
            const double x1 = x[k + 1];
            const double mv = ((0.0 + x0) + x1) * 0.5; // (0 + x0 + x1) / c
            const double suv =
                (0.0 + -0.5 * (x0 - mv)) + 0.5 * (x1 - mv); // 0 + du0 (x0 - mv) + du1 (x1 - mv)
            out[k] = (suv * 2.0) * perSecond;               // suv / suu * rate
        }
    }

    // Two passes over the window: the means first, then the centred sums.
    double direct(const StreamInput& x, const StreamInput& y, std::uint64_t i) const noexcept {
        const Bounds want = boundsAt(i, n_, shape_);
        double c = 0.0;
        double mu = 0.0;
        double mv = 0.0;
        for (std::uint64_t j = want.a; j <= want.b; ++j) {
            if (usable(x, y, j)) {
                c += 1.0;
                mu += abscissa(y, j);
                mv += x[j];
            }
        }
        double slope = kNaN;
        if (c >= 2.0) {
            mu /= c;
            mv /= c;
            double suu = 0.0;
            double suv = 0.0;
            for (std::uint64_t j = want.a; j <= want.b; ++j) {
                if (usable(x, y, j)) {
                    const double du = abscissa(y, j) - mu;
                    suu += du * du;
                    suv += du * (x[j] - mv);
                }
            }
            if (suu > 0.0) {
                slope = suv / suu;
            }
        }
        return slope * perSecond_;
    }

    // Sliding sums with a re-summation every resum_ outputs; the outputs before i done in order.
    double sliding(const StreamInput& x, const StreamInput& y, std::uint64_t i) noexcept {
        const Bounds want = boundsAt(i, n_, shape_);
        if (i == nextResum_) {
            nextResum_ += resum_;
            sums_ = LsSums{};
            baseU_ = againstChannel_ ? kNaN : static_cast<double>(i);
            baseV_ = kNaN;
            for (std::uint64_t j = want.a; j <= want.b; ++j) {
                if (usable(x, y, j)) {
                    if (isInvalid(baseU_)) {
                        baseU_ = abscissa(y, j);
                    }
                    if (isInvalid(baseV_)) {
                        baseV_ = x[j];
                    }
                    sums_.add(abscissa(y, j) - baseU_, x[j] - baseV_);
                }
            }
            if (isInvalid(baseU_)) {
                baseU_ = 0.0;
            }
            if (isInvalid(baseV_)) {
                baseV_ = 0.0;
            }
            cur_ = want;
        } else {
            while (cur_.b < want.b) {
                ++cur_.b;
                if (usable(x, y, cur_.b)) {
                    sums_.add(abscissa(y, cur_.b) - baseU_, x[cur_.b] - baseV_);
                }
            }
            while (cur_.a < want.a) {
                if (usable(x, y, cur_.a)) {
                    sums_.remove(abscissa(y, cur_.a) - baseU_, x[cur_.a] - baseV_);
                }
                ++cur_.a;
            }
        }
        return sums_.slope() * perSecond_;
    }

    WindowSpec window_;
    bool againstChannel_ = false;
    std::uint64_t n_ = 0;
    std::uint64_t points_ = 2;
    WindowShape shape_;
    double perSecond_ = 1.0;
    std::uint64_t resum_ = kResum;
    std::uint64_t nextResum_ = 0; // the next output that re-sums its window from scratch
    LsSums sums_;
    Bounds cur_;
    double baseU_ = 0.0;
    double baseV_ = 0.0;
};

} // namespace

KernelResult makeWindowed(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    if (id == IntrinsicId::Smooth) {
        return std::unique_ptr<Kernel>(std::make_unique<SmoothKernel>(windowArg(ast, call, 1)));
    }
    // derivative(x), (x, n|t), (x, y), (x, y, n|t): a literal second argument is the window.
    const std::size_t argc = argCount(ast, call);
    if (argc >= 2 && isNumberLiteral(argNode(ast, call, 1))) {
        return std::unique_ptr<Kernel>(std::make_unique<DerivativeKernel>(windowArg(ast, call, 1), false));
    }
    if (argc >= 2) {
        return std::unique_ptr<Kernel>(std::make_unique<DerivativeKernel>(windowArg(ast, call, 2), true));
    }
    return std::unique_ptr<Kernel>(std::make_unique<DerivativeKernel>(WindowSpec{}, false));
}

} // namespace esses::maths::kern
