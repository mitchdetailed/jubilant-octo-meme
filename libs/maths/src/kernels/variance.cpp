// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Variance kernels (03 section 5.6, D-023, D-081): x on the Main lap minus x on the reference overlay at
// the same position from the lap start, with the alignment offsets and stretch of both ranges (04 section
// 3.1's mapping):
//
//   variance_time(x)  position c = (t - M.start) + M.offsetUs; reference time R.start + c - R.offsetUs
//   variance_dist(x)  position c = mainDist.distAt(t) * M.stretch + M.offsetM; reference time
//                     R.start + refDist.timeAt((c - R.offsetM) / R.stretch)
//
// x_ref is x read at the reference time by linear interpolation between the samples of x at the result rate
// (the stage input, when the reference lap is in the evaluated source and layer) or, for a reference lap in
// another source, the channel named by the argument read through EvalContext::getChannel over the reference
// lap at the result rate. Invalid: outside the Main range (or when Main is not in this source), without a
// reference overlay, without both DistanceIndex tables (variance_dist), where the reference position falls
// outside the reference lap, and where either value is invalid. SelectionDependent (compile()).
//
// A reference lap in another source that the host cannot read (MathGraph serves only the node's own inputs
// in 0.1, and an expression argument names no channel to read there) fails the evaluation with
// E-REF-SOURCE through status() rather than giving an all-invalid output with status OK (D-185).

#include "kernel_util.hpp"
#include "kernels_internal.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/model/source.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::maths::kern {

namespace {

// Samples of x on a uniform grid (index i at rate.timeAt(first + i, t0)), read by linear interpolation.
struct Grid {
    std::vector<double> values;
    Rate rate;
    Micros t0 = 0;
    SampleIdx first = 0;

    double at(Micros t) const noexcept {
        if (values.empty() || !rate.valid()) {
            return kNaN;
        }
        const SampleIdx j = rate.indexAt(t, t0) - first;
        if (j < 0 || j >= static_cast<SampleIdx>(values.size())) {
            return kNaN;
        }
        const auto k = static_cast<std::size_t>(j);
        const Micros tj = rate.timeAt(first + j, t0);
        if (tj == t) {
            return values[k];
        }
        if (k + 1 >= values.size()) {
            return kNaN;
        }
        const Micros tn = rate.timeAt(first + j + 1, t0);
        const double w = static_cast<double>(t - tj) / static_cast<double>(tn - tj);
        return values[k] + w * (values[k + 1] - values[k]);
    }
};

class VarianceKernel final : public Kernel {
public:
    VarianceKernel(bool distance, std::string channel, std::optional<units::UnitId> unit)
        : distance_(distance), channel_(std::move(channel)), unit_(unit) {}

    KernelClass cls() const override { return KernelClass::Variance; }
    ExtentMode extent() const override { return ExtentMode::Whole; }

    void reset(const EvalContext& ctx) override {
        main_ = ctx.selection.main;
        reference_ = ctx.selection.reference;
        mainDist_ = ctx.mainDist;
        refDist_ = ctx.refDist;
        mainHere_ = ctx.source != nullptr && main_.ref.src == ctx.source->id &&
                    model::canonicalLayerName(main_.ref.layer) == model::canonicalLayerName(ctx.layer);
        refHere_ = reference_.has_value() && ctx.source != nullptr && reference_->ref.src == ctx.source->id &&
                   model::canonicalLayerName(reference_->ref.layer) == model::canonicalLayerName(ctx.layer);
        other_ = Grid{};
        status_ = ok();
        if (mainHere_ && reference_ && !refHere_ &&
            (channel_.empty() || !ctx.getChannel || !readOtherSource(ctx))) {
            ErrorContext c;
            c.detail = "E-REF-SOURCE";
            status_ = fail(ErrorCode::Unsupported,
                           "the reference lap is in another source; variance against another source's lap is "
                           "not evaluated in this version",
                           std::move(c));
        }
    }

    Result<void> status() const override { return status_; }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        const Writer write(out);
        Grid here;
        if (!in.empty()) {
            here.values = loadAll(in[0]);
        }
        here.rate = ctx.rate;
        here.t0 = ctx.t0;
        here.first = ctx.first;
        const Grid& ref = refHere_ ? here : other_;
        const bool usable = mainHere_ && reference_.has_value() &&
                            (!distance_ || (mainDist_ != nullptr && refDist_ != nullptr));
        for (std::uint32_t i = 0; i < out.n; ++i) {
            double v = kNaN;
            const Micros t = ctx.rate.timeAt(ctx.first + static_cast<SampleIdx>(i), ctx.t0);
            if (usable && t >= main_.start && t < main_.end && i < here.values.size()) {
                const std::optional<Micros> tRef = referenceTime(t);
                if (tRef) {
                    v = here.values[i] - ref.at(*tRef);
                }
            }
            write(i, v);
        }
    }

private:
    std::optional<Micros> referenceTime(Micros t) const {
        const MathRangeGeom& r = *reference_;
        Micros tRef = 0;
        if (!distance_) {
            const Micros c = (t - main_.start) + main_.offsetUs;
            tRef = r.start + c - r.offsetUs;
        } else {
            const double c = mainDist_->distAt(t) * main_.stretch + main_.offsetM;
            const double stretch = r.stretch > 0.0 ? r.stretch : 1.0;
            const double d = (c - r.offsetM) / stretch;
            if (!(d >= 0.0) || d > refDist_->lengthM()) {
                return std::nullopt;
            }
            tRef = r.start + static_cast<Micros>(std::llround(refDist_->timeAt(d) * 1e6));
        }
        if (tRef < r.start || tRef >= r.end) {
            return std::nullopt;
        }
        return tRef;
    }

    // False when the host cannot read the channel in the reference lap's source.
    bool readOtherSource(const EvalContext& ctx) {
        const MathRangeGeom& r = *reference_;
        const units::UnitId unit = unit_.value_or(units::kNoUnit);
        Result<MathInputGatherer> host =
            ctx.getChannel(ChannelKey::make(r.ref.src, r.ref.layer, channel_), unit, ctx.resultRate);
        if (!host) {
            return false;
        }
        if (r.end <= r.start) {
            return true; // an empty lap: nothing to read, every sample invalid
        }
        const Rate rate = ctx.resultRate;
        const SampleIdx first = rate.indexAt(r.start, host->outT0());
        const SampleIdx last = rate.indexAt(r.end, host->outT0()) + 1;
        if (last <= first || last - first > (SampleIdx{1} << 31)) {
            return true;
        }
        const auto count = static_cast<std::uint32_t>(last - first + 1);
        MathInputGatherer g(host->series(), host->rate(), host->outT0(), store::Interp::Linear,
                            host->converter(), ctx.cancel, host->priority());
        std::vector<double> values(count);
        std::vector<std::uint8_t> bits((count + 7) / 8, 0);
        BlockView view;
        view.data = values.data();
        view.validity = bits.data();
        view.dtype = store::Dtype::F64;
        view.n = count;
        if (!g.fill(first, count, view)) {
            return false;
        }
        other_.values = loadAll(view);
        other_.rate = rate;
        other_.t0 = host->outT0();
        other_.first = first;
        return true;
    }

    bool distance_ = false;
    std::string channel_;
    std::optional<units::UnitId> unit_;
    MathRangeGeom main_;
    std::optional<MathRangeGeom> reference_;
    std::shared_ptr<const analysis::DistanceIndex> mainDist_;
    std::shared_ptr<const analysis::DistanceIndex> refDist_;
    bool mainHere_ = false;
    bool refHere_ = false;
    Grid other_;
    Result<void> status_ = ok();
};

} // namespace

KernelResult makeVariance(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    // The channel the argument names (a reference, or a reference with [unit]), for a reference lap in
    // another source; an expression of channels is read from the evaluated source only.
    std::string channel;
    std::optional<units::UnitId> unit;
    if (const Node* x = argNode(ast, call, 0); x != nullptr) {
        const Node* ref = x;
        if (x->kind == NodeKind::Convert && !x->kids.empty() && x->kids[0] < ast.nodes.size()) {
            ref = &ast.nodes[x->kids[0]];
            unit = x->unit;
        }
        if (ref->kind == NodeKind::ChannelRef) {
            channel = ref->text;
            if (!unit) {
                unit = ref->type.unit;
            }
        }
    }
    return std::unique_ptr<Kernel>(
        std::make_unique<VarianceKernel>(id == IntrinsicId::VarianceDist, std::move(channel), unit));
}

} // namespace esses::maths::kern
