// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Range kernels (03 section 5.5, D-014): range_is("Group:Path") is 1 while the sample time lies in
// [start, end) of any enabled range of the group, else 0; range_change("Group:Path") counts the range starts
// at or before the sample (0 before the first), so over contiguous laps it is the lap ordinal. Both read the
// interval tables of EvalContext::ranges (BlockCtx::ranges), never produce an invalid sample, and are
// chunk-independent: each range's samples follow from its bounds and the block's absolute indices
// (Rate::indexAt once per bound, no time per sample). The path is matched exactly; a path the source does not
// have is E-RANGE-UNKNOWN (Kernel::status), listing the paths it has. Streamable (D-191): begin() turns the
// group into runs of equal output over the extent and each step copies its part of them.

#include "kernel_util.hpp"
#include "kernels_internal.hpp"
#include "stream.hpp"

#include <esses/analysis/range_tables.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/kernel.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::maths::kern {

namespace {

class RangeKernel final : public Kernel, public Streamable {
public:
    RangeKernel(std::string path, bool change) : path_(std::move(path)), change_(change) {}

    KernelClass cls() const override { return KernelClass::Range; }
    ExtentMode extent() const override { return ExtentMode::Parallel; }

    void reset(const EvalContext& ctx) override { check(ctx.ranges.get()); }
    Result<void> status() const override { return status_; }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        runStreamed(*this, ctx, in, out);
    }

    // The block's runs of equal output, from the group's bounds: [previous end, end) holds `value`.
    void begin(const BlockCtx& ctx) override {
        if (!checked_) {
            check(ctx.ranges.get());
        }
        runs_.clear();
        cursor_ = 0;
        const std::uint64_t n = ctx.n;
        if (!status_) {
            runs_.push_back(Run{n, kNaN});
            return;
        }
        const std::span<const analysis::RangeInterval> group =
            ctx.ranges != nullptr ? ctx.ranges->group(path_) : std::span<const analysis::RangeInterval>{};
        // The block's first sample at or after time t (n when none): the samples of [start, end) are the
        // indices [firstAt(start), firstAt(end)), so no sample time is computed (Rate::timeAt per sample is
        // the cost).
        const auto firstAt = [&](Micros t) -> std::uint64_t {
            const SampleIdx i = ctx.rate.indexAt(t - 1, ctx.t0) + 1 - ctx.first;
            return i <= 0 ? 0 : (i >= static_cast<SampleIdx>(n) ? n : static_cast<std::uint64_t>(i));
        };
        std::uint64_t i = 0;
        if (change_) { // the count of range starts at or before each sample (the group is sorted by start)
            double count = 0.0;
            for (const analysis::RangeInterval& r : group) {
                if (const std::uint64_t at = firstAt(r.start); i < at) {
                    runs_.push_back(Run{at, count});
                    i = at;
                }
                count += 1.0;
            }
            if (i < n) {
                runs_.push_back(Run{n, count});
            }
            return;
        }
        for (const analysis::RangeInterval& r : group) { // 1 inside any range, 0 elsewhere
            if (const std::uint64_t a = firstAt(r.start); i < a) {
                runs_.push_back(Run{a, 0.0});
                i = a;
            }
            if (const std::uint64_t b = firstAt(r.end); i < b) {
                runs_.push_back(Run{b, 1.0});
                i = b;
            }
        }
        if (i < n) {
            runs_.push_back(Run{n, 0.0});
        }
    }

    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override { return need; }

    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> /*in*/, double* out,
              std::uint64_t from, std::uint64_t to) override {
        std::uint64_t i = from;
        while (i < to) {
            while (cursor_ < runs_.size() && runs_[cursor_].end <= i) {
                ++cursor_;
            }
            if (cursor_ == runs_.size()) {
                std::fill(out + (i - from), out + (to - from), kNaN); // not reached: the runs cover the block
                return;
            }
            const std::uint64_t end = std::min(to, runs_[cursor_].end);
            std::fill(out + (i - from), out + (end - from), runs_[cursor_].value);
            i = end;
        }
    }

private:
    void check(const analysis::RangeTables* tables) {
        checked_ = true;
        status_ = Result<void>{};
        if (tables != nullptr && tables->contains(path_)) {
            return;
        }
        std::string known;
        if (tables != nullptr) {
            for (const std::string& p : tables->paths()) {
                known += known.empty() ? "\"" + p + "\"" : ", \"" + p + "\"";
            }
        }
        status_ =
            kernelError("E-RANGE-UNKNOWN", "the range group \"" + path_ + "\" does not exist in this source" +
                                               (known.empty() ? std::string(" (it has no range groups)")
                                                              : "; available: " + known));
    }

    struct Run {
        std::uint64_t end = 0; // exclusive
        double value = 0.0;
    };

    std::string path_;
    bool change_ = false;
    bool checked_ = false;
    Result<void> status_;
    std::vector<Run> runs_;
    std::size_t cursor_ = 0;
};

} // namespace

KernelResult makeRange(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    const Node* path = argNode(ast, call, 0);
    if (path == nullptr || path->kind != NodeKind::StrLit || path->text.empty()) {
        return kernelError("E-ARG-TYPE",
                           std::string(intrinsicName(id)) + "(): the argument must be a range path");
    }
    return std::unique_ptr<Kernel>(std::make_unique<RangeKernel>(path->text, id == IntrinsicId::RangeChange));
}

} // namespace esses::maths::kern
