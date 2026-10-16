// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Stateful kernels (03 section 5.4, D-022, D-080, D-082): sequential over the whole extent on one worker.
// Shared conventions (kernel_util.hpp): a gate `cond` is true when valid and non-zero; a `reset` starts a new
// segment where its integer part increases; the extent start is always a segment start. Bool operands of
// flip_flop, edge_delay, time_valid and pulse_width count an invalid sample as false, and their outputs are
// always valid. Before the first sample every Bool operand is taken as false, so a high first sample is a
// rising edge [compat: stateful.before-start].
//
//   integrate(x[, cond[, reset]])        running sum of x[i] * dt over the gated samples of the segment,
//                                        sample i included (rectangle rule, F64); an invalid x adds 0 and the
//                                        output stays valid; it holds while the gate is off.
//   integrate_dist(x[, cond[, reset]])   the same with the Corr Dist increment since the previous valid Corr
//                                        Dist sample in place of dt (the last stage input, appended by
//                                        compile()).
//   stat_min/max/mean/std_dev/start/end  two-pass: the statistic of the segment's gated valid samples on
//   every
//                                        sample of the segment (population std-dev; no sample -> invalid).
//                                        Without reset there is one segment, the math window (EvalContext
//                                        mathStartUs/mathEndUs; an empty window is the whole extent), and the
//                                        value is written over the whole extent.
//   running_min/max                      the extreme so far within the segment (the whole extent without
//                                        reset); invalid until the first gated valid sample; holds while the
//                                        gate is off.
//   flip_flop(set, reset)                SR latch from 0: a rising edge of set sets it, a rising edge of
//   reset
//                                        clears it, reset wins on the same sample.
//   edge_delay(x, t, rise|fall)          m = ceil(t * rate) samples: rise -> 1 when x has been true on every
//                                        sample of [i - m, i]; fall -> 1 when x was true on any sample of
//                                        [i - m, i] (samples before the extent are false). time_valid(x, t)
//                                        is edge_delay(x, t, rise).
//   time_shift(x, t)                     y[i] = x[i - round(t * rate)] (half away from zero; positive t
//                                        delays); outside the extent -> invalid.
//   pulse_width(x)                       seconds of the latest completed high run of x (high samples x dt),
//                                        from the sample where it fell until the next one completes; invalid
//                                        before the first.
//   previous_sample(x[, n])              y[i] = x[i - n] (default 1); i < n -> invalid.
//
// Every kernel but the statistics is Streamable (stream.hpp, D-191): its state (sum, extreme, latch, run
// lengths, the segment tracker) carries from one step to the next; time_shift and previous_sample read
// behind (or, for a negative shift, ahead of) their outputs. The statistics need a whole segment before its
// first output and run over the whole extent.

#include "kernel_util.hpp"
#include "kernels_internal.hpp"
#include "stream.hpp"

#include <esses/maths/eval_context.hpp>
#include <esses/maths/kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace esses::maths::kern {

namespace {

// m = ceil(t * rate) samples; the 1e-9 keeps t * rate that lands just above a whole number on it.
std::uint64_t ceilSamples(double seconds, Rate rate) noexcept {
    const double s = seconds * rate.hz() - 1e-9;
    if (!(s > 0.0)) {
        return 0;
    }
    if (s > 1e15) {
        return std::uint64_t{1} << 50;
    }
    return static_cast<std::uint64_t>(std::ceil(s));
}

double literal(const Ast& ast, std::uint32_t call, std::size_t k, double fallback) {
    const Node* n = argNode(ast, call, k);
    return isNumberLiteral(n) ? n->number : fallback;
}

// The inputs x, [cond], [reset] of the gated functions, read in place (the statistics' whole-extent run).
struct Gated {
    std::optional<Reader> x;
    std::optional<Reader> cond;
    std::optional<Reader> reset;

    Gated(std::span<const BlockView> in, std::size_t argc) {
        if (!in.empty()) {
            x.emplace(in[0]);
        }
        if (argc >= 2 && in.size() >= 2) {
            cond.emplace(in[1]);
        }
        if (argc >= 3 && in.size() >= 3) {
            reset.emplace(in[2]);
        }
    }
    double value(std::uint32_t i) const noexcept { return x ? (*x)(i) : kNaN; }
    bool gate(std::uint32_t i) const noexcept { return !cond || truthy((*cond)(i)); }
    // The gated valid value of sample i, or NaN.
    double gated(std::uint32_t i) const noexcept { return gate(i) ? value(i) : kNaN; }
};

// The same inputs of a streamed step.
struct GatedStep {
    const StreamInput* x = nullptr;
    const StreamInput* cond = nullptr;
    const StreamInput* reset = nullptr;

    GatedStep(std::span<const StreamInput> in, std::size_t argc) noexcept {
        if (!in.empty()) {
            x = &in[0];
        }
        if (argc >= 2 && in.size() >= 2) {
            cond = &in[1];
        }
        if (argc >= 3 && in.size() >= 3) {
            reset = &in[2];
        }
    }
    bool gate(std::uint64_t i) const noexcept { return cond == nullptr || truthy((*cond)[i]); }
    double gated(std::uint64_t i) const noexcept { return gate(i) ? (*x)[i] : kNaN; }
};

// Segment starts in sample order (03 section 5.4): sample 0, and every sample whose reset has a larger
// integer part (toward zero) than the last valid reset sample before it; invalid reset samples are skipped.
// Call starts() for i = 0, 1, 2, ... in order; the state carries across steps.
class Segments {
public:
    void restart() noexcept {
        prev_ = 0.0;
        have_ = false;
    }
    bool starts(double reset, std::uint64_t i) noexcept {
        bool start = i == 0;
        if (!isInvalid(reset)) {
            const double whole = std::trunc(reset);
            start = start || (have_ && whole > prev_);
            prev_ = whole;
            have_ = true;
        }
        return start;
    }

private:
    double prev_ = 0.0;
    bool have_ = false;
};

// The statistics' segment tracker over a whole-extent view.
class SegmentTracker {
public:
    explicit SegmentTracker(const std::optional<Reader>& reset) : reset_(reset) {}

    // Call for i = 0, 1, 2, ... in order.
    bool starts(std::uint32_t i) noexcept {
        if (!reset_) {
            return i == 0;
        }
        return segments_.starts((*reset_)(i), i);
    }

private:
    std::optional<Reader> reset_;
    Segments segments_;
};

// Shared parts of the streamed Stateful kernels.
class StatefulStream : public Kernel, public Streamable {
public:
    KernelClass cls() const override { return KernelClass::Stateful; }
    ExtentMode extent() const override { return ExtentMode::Whole; }
    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        runStreamed(*this, ctx, in, out);
    }
};

class IntegrateKernel final : public StatefulStream {
public:
    IntegrateKernel(std::size_t argc, bool distance) : argc_(argc), distance_(distance) {}

    void begin(const BlockCtx& extent) override {
        dt_ = 1.0 / extent.rate.hz();
        acc_ = 0.0;
        lastDist_ = kNaN;
        segments_.restart();
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        const GatedStep g(in, argc_);
        if (g.x == nullptr) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        double acc = acc_;
        if (!distance_ && g.cond == nullptr && g.reset == nullptr) {
            // integrate(x): one segment from sample 0 (acc_ starts at 0), dt per sample.
            const double* x = g.x->at(from);
            const double dt = dt_;
            const std::uint64_t m = to - from;
            for (std::uint64_t k = 0; k < m; ++k) {
                const double v = x[k];
                if (!isInvalid(v)) {
                    acc += v * dt;
                }
                out[k] = acc;
            }
            acc_ = acc;
            return;
        }
        const StreamInput* dist = distance_ && in.size() > argc_ ? &in[argc_] : nullptr;
        for (std::uint64_t i = from; i < to; ++i) {
            if (segments_.starts(g.reset != nullptr ? (*g.reset)[i] : kNaN, i)) {
                acc = 0.0;
            }
            double step = dt_;
            if (distance_) {
                step = 0.0;
                const double d = dist != nullptr ? (*dist)[i] : kNaN;
                if (!isInvalid(d)) {
                    step = isInvalid(lastDist_) ? 0.0 : d - lastDist_;
                    lastDist_ = d;
                }
            }
            const double v = g.gated(i);
            if (!isInvalid(v)) {
                acc += v * step;
            }
            out[i - from] = acc;
        }
        acc_ = acc;
    }

private:
    std::size_t argc_ = 1;
    bool distance_ = false;
    double dt_ = 0.0;
    double acc_ = 0.0;
    double lastDist_ = kNaN;
    Segments segments_;
};

class StatKernel final : public Kernel {
public:
    StatKernel(IntrinsicId id, std::size_t argc) : id_(id), argc_(argc) {}

    KernelClass cls() const override { return KernelClass::Stateful; }
    ExtentMode extent() const override { return ExtentMode::Whole; }

    void reset(const EvalContext& ctx) override {
        windowStart_ = ctx.mathStartUs;
        windowEnd_ = ctx.mathEndUs;
    }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        const Writer write(out);
        const std::uint32_t n = out.n;
        const Gated g(in, argc_);
        if (!g.x) {
            return;
        }
        if (argc_ < 3) {
            // One segment, the math window [windowStart, windowEnd) (an empty window: the whole extent); the
            // value goes over the whole extent.
            std::uint32_t a = 0;
            std::uint32_t b = n;
            if (windowEnd_ > windowStart_) {
                a = firstAtOrAfter(ctx, windowStart_, n);
                b = firstAtOrAfter(ctx, windowEnd_, n);
            }
            const double v = statistic(g, a, b);
            for (std::uint32_t i = 0; i < n; ++i) {
                write(i, v);
            }
            return;
        }
        std::vector<std::uint32_t> starts;
        SegmentTracker segments(g.reset);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (segments.starts(i)) {
                starts.push_back(i);
            }
        }
        starts.push_back(n);
        for (std::size_t s = 0; s + 1 < starts.size(); ++s) {
            const double v = statistic(g, starts[s], starts[s + 1]);
            for (std::uint32_t i = starts[s]; i < starts[s + 1]; ++i) {
                write(i, v);
            }
        }
    }

private:
    // The first sample of the block at or after time t (n when none).
    static std::uint32_t firstAtOrAfter(const BlockCtx& ctx, Micros t, std::uint32_t n) noexcept {
        const SampleIdx i = ctx.rate.indexAt(t - 1, ctx.t0) + 1 - ctx.first;
        return i <= 0 ? 0 : (i >= static_cast<SampleIdx>(n) ? n : static_cast<std::uint32_t>(i));
    }

    // The statistic of the gated valid samples of [a, b); NaN when there is none.
    double statistic(const Gated& g, std::uint32_t a, std::uint32_t b) const {
        double count = 0.0;
        double sum = 0.0;
        double lo = kNaN;
        double hi = kNaN;
        double first = kNaN;
        double last = kNaN;
        for (std::uint32_t i = a; i < b; ++i) {
            const double v = g.gated(i);
            if (isInvalid(v)) {
                continue;
            }
            if (count == 0.0) {
                first = v;
                lo = v;
                hi = v;
            }
            count += 1.0;
            sum += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            last = v;
        }
        if (count == 0.0) {
            return kNaN;
        }
        switch (id_) {
        case IntrinsicId::StatMin:
            return lo;
        case IntrinsicId::StatMax:
            return hi;
        case IntrinsicId::StatMean:
            return sum / count;
        case IntrinsicId::StatStart:
            return first;
        case IntrinsicId::StatEnd:
            return last;
        case IntrinsicId::StatStdDev: {
            const double mean = sum / count;
            double ss = 0.0;
            for (std::uint32_t i = a; i < b; ++i) {
                const double v = g.gated(i);
                if (!isInvalid(v)) {
                    ss += (v - mean) * (v - mean);
                }
            }
            return std::sqrt(ss / count);
        }
        default:
            return kNaN;
        }
    }

    IntrinsicId id_;
    std::size_t argc_ = 1;
    Micros windowStart_ = 0;
    Micros windowEnd_ = 0;
};

class RunningKernel final : public StatefulStream {
public:
    RunningKernel(bool max, std::size_t argc) : max_(max), argc_(argc) {}

    void begin(const BlockCtx& /*extent*/) override {
        cur_ = kNaN;
        segments_.restart();
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        const GatedStep g(in, argc_);
        if (g.x == nullptr) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        double cur = cur_;
        for (std::uint64_t i = from; i < to; ++i) {
            if (segments_.starts(g.reset != nullptr ? (*g.reset)[i] : kNaN, i)) {
                cur = kNaN;
            }
            const double v = g.gated(i);
            if (!isInvalid(v)) {
                cur = isInvalid(cur) ? v : (max_ ? std::max(cur, v) : std::min(cur, v));
            }
            out[i - from] = cur;
        }
        cur_ = cur;
    }

private:
    bool max_ = false;
    std::size_t argc_ = 1;
    double cur_ = kNaN;
    Segments segments_;
};

class FlipFlopKernel final : public StatefulStream {
public:
    void begin(const BlockCtx& /*extent*/) override {
        state_ = false;
        prevSet_ = false;
        prevReset_ = false;
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.size() < 2) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        for (std::uint64_t i = from; i < to; ++i) {
            const bool s = truthy(in[0][i]);
            const bool r = truthy(in[1][i]);
            if (r && !prevReset_) {
                state_ = false;
            } else if (s && !prevSet_) {
                state_ = true;
            }
            prevSet_ = s;
            prevReset_ = r;
            out[i - from] = state_ ? 1.0 : 0.0;
        }
    }

private:
    bool state_ = false;
    bool prevSet_ = false;
    bool prevReset_ = false;
};

class EdgeDelayKernel final : public StatefulStream {
public:
    EdgeDelayKernel(double seconds, bool rise) : seconds_(seconds), rise_(rise) {}

    void begin(const BlockCtx& extent) override {
        m_ = ceilSamples(seconds_, extent.rate);
        run_ = 0;
        lastTrue_.reset();
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty()) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        for (std::uint64_t i = from; i < to; ++i) {
            const bool v = truthy(in[0][i]);
            run_ = v ? run_ + 1 : 0; // consecutive true samples ending here
            if (v) {
                lastTrue_ = i;
            }
            const bool high = rise_ ? run_ >= m_ + 1 : (lastTrue_.has_value() && i - *lastTrue_ <= m_);
            out[i - from] = high ? 1.0 : 0.0;
        }
    }

private:
    double seconds_ = 0.0;
    bool rise_ = true;
    std::uint64_t m_ = 0;
    std::uint64_t run_ = 0;
    std::optional<std::uint64_t> lastTrue_; // the latest true sample
};

class TimeShiftKernel final : public StatefulStream {
public:
    explicit TimeShiftKernel(double seconds) : seconds_(seconds) {}

    void begin(const BlockCtx& extent) override {
        n_ = static_cast<std::int64_t>(extent.n);
        const double shift = seconds_ * extent.rate.hz();
        // A shift beyond the extent (or not a number) leaves no sample inside it.
        s_ = !(std::abs(shift) <= 9e15) ? n_ + 1 : std::llround(shift);
    }
    std::uint64_t behind() const noexcept override {
        return s_ > 0 && s_ <= n_ ? static_cast<std::uint64_t>(s_) : 0;
    }
    std::uint64_t ahead() const noexcept override {
        return s_ < 0 && -s_ <= n_ ? static_cast<std::uint64_t>(-s_) : 0;
    }
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override { return need; }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty()) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        for (std::uint64_t i = from; i < to; ++i) {
            const std::int64_t j = static_cast<std::int64_t>(i) - s_;
            out[i - from] = (j >= 0 && j < n_) ? in[0][static_cast<std::uint64_t>(j)] : kNaN;
        }
    }

private:
    double seconds_ = 0.0;
    std::int64_t n_ = 0;
    std::int64_t s_ = 0;
};

class PulseWidthKernel final : public StatefulStream {
public:
    void begin(const BlockCtx& extent) override {
        dt_ = 1.0 / extent.rate.hz();
        prev_ = false;
        rise_ = 0;
        width_ = kNaN;
    }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty()) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        for (std::uint64_t i = from; i < to; ++i) {
            const bool v = truthy(in[0][i]);
            if (v && !prev_) {
                rise_ = i;
            } else if (!v && prev_) {
                width_ = static_cast<double>(i - rise_) * dt_;
            }
            prev_ = v;
            out[i - from] = width_;
        }
    }

private:
    double dt_ = 0.0;
    bool prev_ = false;
    std::uint64_t rise_ = 0;
    double width_ = kNaN;
};

class PreviousSampleKernel final : public StatefulStream {
public:
    explicit PreviousSampleKernel(std::uint64_t n) : n_(n) {}

    void begin(const BlockCtx& extent) override { extentN_ = extent.n; }
    std::uint64_t behind() const noexcept override { return std::min(n_, extentN_); }
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override { return need; }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        if (in.empty()) {
            std::fill(out, out + (to - from), kNaN);
            return;
        }
        for (std::uint64_t i = from; i < to; ++i) {
            out[i - from] = i >= n_ ? in[0][i - n_] : kNaN;
        }
    }

private:
    std::uint64_t n_ = 1;
    std::uint64_t extentN_ = 0;
};

} // namespace

KernelResult makeStateful(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    const std::size_t argc = argCount(ast, call);
    switch (id) {
    case IntrinsicId::Integrate:
    case IntrinsicId::IntegrateDist:
        return std::unique_ptr<Kernel>(
            std::make_unique<IntegrateKernel>(argc, id == IntrinsicId::IntegrateDist));
    case IntrinsicId::StatMin:
    case IntrinsicId::StatMax:
    case IntrinsicId::StatMean:
    case IntrinsicId::StatStdDev:
    case IntrinsicId::StatStart:
    case IntrinsicId::StatEnd:
        return std::unique_ptr<Kernel>(std::make_unique<StatKernel>(id, argc));
    case IntrinsicId::RunningMin:
    case IntrinsicId::RunningMax:
        return std::unique_ptr<Kernel>(std::make_unique<RunningKernel>(id == IntrinsicId::RunningMax, argc));
    case IntrinsicId::FlipFlop:
        return std::unique_ptr<Kernel>(std::make_unique<FlipFlopKernel>());
    case IntrinsicId::EdgeDelay: {
        const Node* edge = argNode(ast, call, 2);
        const bool rise = edge == nullptr || edge->kind != NodeKind::Keyword || edge->text != "fall";
        return std::unique_ptr<Kernel>(std::make_unique<EdgeDelayKernel>(literal(ast, call, 1, 0.0), rise));
    }
    case IntrinsicId::TimeValid:
        return std::unique_ptr<Kernel>(std::make_unique<EdgeDelayKernel>(literal(ast, call, 1, 0.0), true));
    case IntrinsicId::TimeShift:
        return std::unique_ptr<Kernel>(std::make_unique<TimeShiftKernel>(literal(ast, call, 1, 0.0)));
    case IntrinsicId::PulseWidth:
        return std::unique_ptr<Kernel>(std::make_unique<PulseWidthKernel>());
    case IntrinsicId::PreviousSample: {
        const double n = literal(ast, call, 1, 1.0);
        if (!(n >= 1.0) || std::trunc(n) != n) {
            return kernelError("E-ARG-VALUE", "previous_sample(): n must be a whole number of at least 1");
        }
        return std::unique_ptr<Kernel>(std::make_unique<PreviousSampleKernel>(
            n > 1e15 ? std::uint64_t{1} << 50 : static_cast<std::uint64_t>(n)));
    }
    default:
        return fail(ErrorCode::Invalid, "makeStateful: not a Stateful intrinsic");
    }
}

} // namespace esses::maths::kern
