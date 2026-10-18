// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlanStream (plan_stream.hpp, D-191). pull() asks the last stage for the next piece of kPieceSamples
// result samples; a stage asked for outputs up to `upTo` first asks its inputs for samples up to upTo +
// ahead() (a whole-extent stage: for every sample), then steps. Plan inputs are gathered on demand, at most
// one input chunk per gather. After each piece every window drops the samples none of its readers needs any
// more (a reader needs its next output's context: next - behind(); a whole-extent reader that has not run
// needs everything), so a streamed stage holds about one piece plus its window.

#include "plan_stream.hpp"

#include "../kernels/stream.hpp"

#include <esses/maths/math_input_gatherer.hpp>
#include <esses/store/chunk.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::maths::lang {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint64_t kPieceSamples = 16'384;                // result samples per streamed step
constexpr std::uint32_t kGatherSamples = store::kChunkSamples; // at most one input chunk per gather
constexpr std::uint64_t kNoNeed = std::numeric_limits<std::uint64_t>::max();

// Samples [base, end) of a sequence over the extent: grown at the end, trimmed at the front. Storage is not
// zero-filled (every sample is written before it is read).
class Window {
public:
    std::uint64_t base() const noexcept { return base_; }
    std::uint64_t end() const noexcept { return end_; }
    const double* at(std::uint64_t i) const noexcept { return data_.get() + (i - base_); }

    // Room for the samples up to `upTo` in one allocation (a whole-extent reader's input).
    void reserve(std::uint64_t upTo) {
        if (upTo - base_ > capacity_) {
            reallocate(upTo - base_);
        }
    }

    // Room for the samples [end(), upTo); returns where sample end() goes.
    double* extend(std::uint64_t upTo) {
        if (upTo - base_ > capacity_) {
            reallocate(std::max<std::uint64_t>(upTo - base_, capacity_ + capacity_ / 2));
        }
        return data_.get() + (end_ - base_);
    }
    void commit(std::uint64_t upTo) noexcept { end_ = upTo; }
    // Empty, positioned at sample `at` (the storage is kept).
    void resetAt(std::uint64_t at) noexcept {
        base_ = at;
        end_ = at;
    }

    // Forgets the samples before `keep`; the rest moves to the front once at least as much is dead as is
    // held, so the moves cost O(1) per sample.
    void trim(std::uint64_t keep) noexcept {
        keep = std::min(keep, end_);
        if (keep <= base_) {
            return;
        }
        const std::uint64_t dead = keep - base_;
        const std::uint64_t held = end_ - keep;
        if (dead < held) {
            return;
        }
        if (held > 0) {
            std::memmove(data_.get(), data_.get() + dead, static_cast<std::size_t>(held) * sizeof(double));
        }
        base_ = keep;
    }

private:
    void reallocate(std::uint64_t capacity) {
        std::unique_ptr<double[]> fresh(new double[static_cast<std::size_t>(capacity)]);
        if (end_ > base_) {
            std::memcpy(fresh.get(), data_.get(), static_cast<std::size_t>(end_ - base_) * sizeof(double));
        }
        data_ = std::move(fresh);
        capacity_ = capacity;
    }

    std::unique_ptr<double[]> data_;
    std::uint64_t capacity_ = 0;
    std::uint64_t base_ = 0;
    std::uint64_t end_ = 0;
};

Failure cancelled() {
    return fail(ErrorCode::Cancelled, "maths evaluation cancelled");
}

} // namespace

struct PlanStream::Impl {
    struct Input {
        store::Series series;
        Rate rate;
        Micros outT0 = 0;
        store::Interp interp = store::Interp::Linear;
        units::Converter conv;
        Priority priority = Priority::Visible;
        Window window;
        std::vector<std::size_t> readers; // stages
    };
    struct StageRun {
        Kernel* kernel = nullptr;
        kern::Streamable* stream = nullptr; // null: runs once over the whole extent
        std::uint64_t next = 0;             // outputs produced
        bool ran = false;                   // a whole-extent stage has run
        std::uint64_t behind = 0;
        std::uint64_t ahead = 0;
        Window window;
        std::vector<std::size_t> readers; // stages
        std::vector<kern::StreamInput> streamIn;
        std::vector<BlockView> wholeIn;
    };

    Plan& plan;
    EvalContext ctx;
    std::uint64_t count = 0;
    Micros gridT0 = 0;
    bool started = false;
    std::uint64_t produced = 0;
    BlockCtx extent;
    CancelToken cancel;
    std::vector<Input> inputs;
    std::vector<StageRun> stages;

    Impl(Plan& p, EvalContext c) : plan(p), ctx(std::move(c)) {}

    bool cancelledNow() const { return cancel.cancelled() || ctx.cancel.cancelled(); }

    Result<void> start() {
        if (plan.stages.empty()) {
            return fail(ErrorCode::Invalid, "evaluateWhole: the plan has no stage");
        }
        if (!ctx.resultRate.valid()) {
            return fail(ErrorCode::Invalid, "evaluateWhole: the result rate is not valid");
        }
        if (ctx.resultCount > std::numeric_limits<std::uint32_t>::max() - kGatherSamples) {
            return fail(ErrorCode::Unsupported, "evaluateWhole: more than 2^32 result samples");
        }
        count = ctx.resultCount;
        const std::size_t n = plan.stages.size();
        for (std::size_t s = 0; s < n; ++s) {
            if (plan.stages[s].kernel == nullptr) {
                return fail(ErrorCode::Invalid, "evaluateWhole: stage without a kernel");
            }
            for (const Stage::Ref& r : plan.stages[s].inputs) {
                const bool visible = (r.from == Stage::Ref::From::Input && r.index < plan.inputs.size()) ||
                                     (r.from == Stage::Ref::From::Stage && r.index < s);
                if (!visible) {
                    return fail(ErrorCode::Invalid, "evaluateWhole: a stage reads an input it cannot see");
                }
            }
        }
        // The gatherers: the host chooses series, unit and grid; the interpolation is the plan's.
        inputs.resize(plan.inputs.size());
        for (std::size_t k = 0; k < plan.inputs.size(); ++k) {
            const PlanInput& pi = plan.inputs[k];
            if (!ctx.getChannel) {
                return fail(ErrorCode::Invalid,
                            "evaluateWhole: the context cannot read channels (getChannel unset)");
            }
            Result<MathInputGatherer> host = ctx.getChannel(pi.key, pi.unit, ctx.resultRate);
            if (!host) {
                return Failure{host.error()};
            }
            Input& in = inputs[k];
            in.series = host->series();
            in.rate = host->rate();
            in.outT0 = host->outT0();
            in.interp = pi.interp;
            in.conv = host->converter();
            in.priority = host->priority();
            if (k == 0) {
                gridT0 = host->outT0();
            }
        }
        extent.first = ctx.resultFirst;
        extent.n = static_cast<std::uint32_t>(count);
        extent.t0 = gridT0;
        extent.rate = ctx.resultRate;
        extent.ranges = ctx.ranges;
        extent.mainDist = ctx.mainDist;
        extent.refDist = ctx.refDist;
        extent.cancel = ctx.cancel;
        stages.resize(n);
        for (std::size_t s = 0; s < n; ++s) {
            StageRun& st = stages[s];
            st.kernel = plan.stages[s].kernel.get();
            st.kernel->reset(ctx);
            // E-ARG-VALUE (Nyquist), E-RANGE-UNKNOWN, ... (Kernel::status)
            if (Result<void> status = st.kernel->status(); !status) {
                return Failure{status.error()};
            }
            st.stream = dynamic_cast<kern::Streamable*>(st.kernel);
            if (st.stream != nullptr && !st.stream->canStream()) {
                st.stream = nullptr;
            }
            if (st.stream != nullptr) {
                st.stream->begin(extent);
                st.behind = st.stream->behind();
                st.ahead = st.stream->ahead();
            }
            for (const Stage::Ref& r : plan.stages[s].inputs) {
                std::vector<std::size_t>& readers =
                    r.from == Stage::Ref::From::Input ? inputs[r.index].readers : stages[r.index].readers;
                if (std::find(readers.begin(), readers.end(), s) == readers.end()) {
                    readers.push_back(s);
                }
            }
            st.streamIn.resize(plan.stages[s].inputs.size());
            st.wholeIn.resize(plan.stages[s].inputs.size());
        }
        started = true;
        return ok();
    }

    bool restartable() const noexcept {
        if (!started) {
            return false;
        }
        for (const StageRun& st : stages) {
            if (st.stream == nullptr || !st.stream->restartPoint(count).has_value()) {
                return false;
            }
        }
        return true;
    }

    Result<void> restartAt(std::uint64_t first, const CancelToken& token) {
        if (!restartable()) {
            return fail(ErrorCode::Unsupported,
                        "maths evaluation: the plan cannot restart inside the extent");
        }
        if (first > count) {
            return fail(ErrorCode::Invalid, "maths evaluation: restart past the end of the extent");
        }
        // From the result back to the inputs: each stage starts at its restart point before the first sample
        // its readers need, and needs its own inputs from there minus its context.
        const std::size_t n = stages.size();
        std::vector<std::uint64_t> need(n, kNoNeed);
        std::vector<std::uint64_t> inputNeed(inputs.size(), kNoNeed);
        std::vector<std::uint64_t> startAt(n, 0);
        need[n - 1] = first;
        for (std::size_t s = n; s-- > 0;) {
            const std::uint64_t want = need[s] == kNoNeed ? count : need[s];
            const std::optional<std::uint64_t> point = stages[s].stream->restartPoint(want);
            startAt[s] = point.value_or(0);
            const std::uint64_t from = startAt[s] > stages[s].behind ? startAt[s] - stages[s].behind : 0;
            for (const Stage::Ref& r : plan.stages[s].inputs) {
                std::uint64_t& slot = r.from == Stage::Ref::From::Input ? inputNeed[r.index] : need[r.index];
                slot = std::min(slot, from);
            }
        }
        for (std::size_t s = 0; s < n; ++s) {
            StageRun& st = stages[s];
            st.kernel->reset(ctx);
            if (Result<void> status = st.kernel->status(); !status) {
                return Failure{status.error()};
            }
            st.stream->beginAt(extent, startAt[s]);
            st.behind = st.stream->behind();
            st.ahead = st.stream->ahead();
            st.next = startAt[s];
            st.window.resetAt(startAt[s]);
        }
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            inputs[k].window.resetAt(inputNeed[k] == kNoNeed ? 0 : inputNeed[k]);
        }
        produced = startAt[n - 1];
        // The samples of the result before `first` (none when the last stage restarts there).
        std::vector<double> dropped;
        while (produced < first) {
            const std::uint64_t m = std::min(kPieceSamples, first - produced);
            dropped.resize(static_cast<std::size_t>(m));
            if (Result<void> r = pull(dropped.data(), m, token); !r) {
                return r;
            }
        }
        return ok();
    }

    Window& windowOf(const Stage::Ref& r) {
        return r.from == Stage::Ref::From::Input ? inputs[r.index].window : stages[r.index].window;
    }

    Result<void> ensure(const Stage::Ref& r, std::uint64_t upTo) {
        return r.from == Stage::Ref::From::Input ? gather(r.index, upTo) : ensureStage(r.index, upTo);
    }

    // Input k gathered up to sample upTo.
    Result<void> gather(std::size_t k, std::uint64_t upTo) {
        Input& in = inputs[k];
        upTo = std::min(upTo, count);
        while (in.window.end() < upTo) {
            if (cancelledNow()) {
                return cancelled();
            }
            const std::uint64_t first = in.window.end();
            const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(upTo - first, kGatherSamples));
            double* dst = in.window.extend(first + n);
            MathInputGatherer g(in.series, in.rate, in.outT0, in.interp, in.conv, ctx.cancel, in.priority);
            if (Result<void> r = g.fill(ctx.resultFirst + static_cast<SampleIdx>(first), n,
                                        BlockView{dst, nullptr, store::Dtype::F64, n});
                !r) {
                return r;
            }
            in.window.commit(first + n);
        }
        return ok();
    }

    // Runs whole-extent stage s once over every sample, from inputs gathered or produced over the whole
    // extent, into its window or (the last stage, when all of the result is pulled at once) into `target`.
    Result<void> runWhole(std::size_t s, double* target) {
        StageRun& st = stages[s];
        const Stage& stage = plan.stages[s];
        for (std::size_t i = 0; i < stage.inputs.size(); ++i) {
            windowOf(stage.inputs[i]).reserve(count);
            if (Result<void> r = ensure(stage.inputs[i], count); !r) {
                return r;
            }
            const Window& w = windowOf(stage.inputs[i]);
            if (w.base() != 0) {
                return fail(ErrorCode::Internal, "maths evaluation: a whole-extent stage lost its input");
            }
            st.wholeIn[i] = BlockView{const_cast<double*>(w.at(0)), nullptr, store::Dtype::F64,
                                      static_cast<std::uint32_t>(count)};
        }
        double* out = target != nullptr ? target : st.window.extend(count);
        BlockCtx b = extent;
        st.kernel->run(b, st.wholeIn,
                       BlockView{out, nullptr, store::Dtype::F64, static_cast<std::uint32_t>(count)});
        if (cancelledNow()) {
            return cancelled();
        }
        if (Result<void> status = st.kernel->status(); !status) {
            return Failure{status.error()};
        }
        if (target == nullptr) {
            st.window.commit(count);
        }
        st.next = count;
        st.ran = true;
        return ok();
    }

    // Stage s's outputs up to upTo (all of them for a whole-extent stage).
    Result<void> ensureStage(std::size_t s, std::uint64_t upTo) {
        StageRun& st = stages[s];
        const Stage& stage = plan.stages[s];
        upTo = std::min(upTo, count);
        if (st.stream == nullptr) {
            return st.ran ? ok() : runWhole(s, nullptr);
        }
        if (st.next >= upTo) {
            return ok();
        }
        const std::uint64_t need = std::min(count, upTo + st.ahead);
        for (std::size_t i = 0; i < stage.inputs.size(); ++i) {
            if (Result<void> r = ensure(stage.inputs[i], need); !r) {
                return r;
            }
            const Window& w = windowOf(stage.inputs[i]);
            st.streamIn[i] = kern::StreamInput{w.at(w.base()), w.base(), w.end()};
        }
        double* out = st.window.extend(upTo);
        st.stream->step(extent, st.streamIn, out, st.next, upTo);
        st.window.commit(upTo);
        st.next = upTo;
        return ok();
    }

    // The result samples [produced, upTo) into out. The last stage is read by no other stage: a streamed one
    // steps straight into `out`, a whole-extent one is copied from its window.
    Result<void> ensureLast(std::uint64_t upTo, double* out) {
        const std::size_t last = stages.size() - 1;
        StageRun& st = stages[last];
        if (st.stream == nullptr || st.next != produced) {
            if (Result<void> r = ensureStage(last, upTo); !r) {
                return r;
            }
            std::memcpy(out, st.window.at(produced),
                        static_cast<std::size_t>(upTo - produced) * sizeof(double));
            return ok();
        }
        const Stage& stage = plan.stages[last];
        const std::uint64_t need = std::min(count, upTo + st.ahead);
        for (std::size_t i = 0; i < stage.inputs.size(); ++i) {
            if (Result<void> r = ensure(stage.inputs[i], need); !r) {
                return r;
            }
            const Window& w = windowOf(stage.inputs[i]);
            st.streamIn[i] = kern::StreamInput{w.at(w.base()), w.base(), w.end()};
        }
        st.stream->step(extent, st.streamIn, out, st.next, upTo);
        st.next = upTo;
        st.window.resetAt(upTo);
        return ok();
    }

    // The first sample stage `reader` will still read from its inputs.
    std::uint64_t needOf(std::size_t reader) const noexcept {
        const StageRun& st = stages[reader];
        if (st.stream == nullptr) {
            return st.ran ? kNoNeed : 0;
        }
        return st.next > st.behind ? st.next - st.behind : 0;
    }

    void trimAll() {
        for (Input& in : inputs) {
            std::uint64_t keep = kNoNeed;
            for (const std::size_t r : in.readers) {
                keep = std::min(keep, needOf(r));
            }
            in.window.trim(keep);
        }
        for (std::size_t s = 0; s < stages.size(); ++s) {
            std::uint64_t keep = s + 1 == stages.size() ? produced : kNoNeed;
            for (const std::size_t r : stages[s].readers) {
                keep = std::min(keep, needOf(r));
            }
            stages[s].window.trim(keep);
        }
    }

    Result<void> pull(double* out, std::uint64_t n, const CancelToken& token) {
        if (!started) {
            return fail(ErrorCode::Invalid, "maths evaluation: pull() before start()");
        }
        if (n > count - produced) {
            return fail(ErrorCode::Invalid, "maths evaluation: pull() past the end of the extent");
        }
        cancel = token;
        const std::size_t last = stages.size() - 1;
        std::uint64_t done = 0;
        if (produced == 0 && n == count && count > 0 && stages[last].stream == nullptr && !stages[last].ran) {
            // All of a whole-extent result at once: the last stage writes the caller's buffer directly.
            if (Result<void> r = runWhole(last, out); !r) {
                return r;
            }
            produced = count;
            done = n;
            trimAll();
        }
        while (done < n) {
            if (cancelledNow()) {
                return cancelled();
            }
            const std::uint64_t upTo = produced + std::min(kPieceSamples, n - done);
            if (Result<void> r = ensureLast(upTo, out + done); !r) {
                return r;
            }
            done += upTo - produced;
            produced = upTo;
            trimAll();
        }
        if (produced == count) {
            for (const StageRun& st : stages) {
                if (Result<void> status = st.kernel->status(); !status) {
                    return Failure{status.error()};
                }
            }
        }
        return ok();
    }
};

PlanStream::PlanStream(Plan& plan, EvalContext ctx) : impl_(std::make_unique<Impl>(plan, std::move(ctx))) {}

PlanStream::~PlanStream() = default;

Result<void> PlanStream::start() {
    return impl_->start();
}

bool PlanStream::restartable() const noexcept {
    return impl_->restartable();
}

Result<void> PlanStream::restartAt(std::uint64_t first, const CancelToken& cancel) {
    return impl_->restartAt(first, cancel);
}

std::uint64_t PlanStream::size() const noexcept {
    return impl_->ctx.resultCount;
}

std::uint64_t PlanStream::produced() const noexcept {
    return impl_->produced;
}

Micros PlanStream::t0() const noexcept {
    return impl_->ctx.resultRate.timeAt(impl_->ctx.resultFirst, impl_->gridT0);
}

Result<void> PlanStream::pull(double* out, std::uint64_t n, const CancelToken& cancel) {
    return impl_->pull(out, n, cancel);
}

std::uint64_t finaliseResult(double* v, std::size_t n, store::Dtype outDtype) noexcept {
    if (outDtype == store::Dtype::F32) {
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = static_cast<double>(static_cast<float>(v[i]));
        }
    } else if (outDtype == store::Dtype::I32) {
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = std::trunc(v[i]);
        }
    }
    std::uint64_t invalid = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // +-infinity is invalid like NaN; v - v is 0 for every finite value and NaN otherwise.
        v[i] = v[i] - v[i] == 0.0 ? v[i] : kNaN; // NOLINT(misc-redundant-expression)
        invalid += v[i] != v[i] ? 1u : 0u;       // NaN
    }
    return invalid;
}

} // namespace esses::maths::lang
