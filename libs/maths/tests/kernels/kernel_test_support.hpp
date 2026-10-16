// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers for the kernel tests (tests/kernels): a one-layer source of in-memory F64 channels (NaN = invalid),
// a scope over it, and parse + compile + evaluateWhole of an expression over the whole layer extent with an
// optional range table and selection. Expected values in these tests come from the per-sample loops written
// in the tests themselves (03 section 14.2), never from the kernels.
#pragma once

#include "../maths_test_support.hpp"

#include <esses/analysis/range_tables.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/env.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/scope.hpp>
#include <esses/store/memory_provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths::test {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1012ull;
}

class KernelFixture {
public:
    explicit KernelFixture(unsigned workers = 1) : rt_(std::make_shared<Runtime>(workers)) {}

    // An F32 channel at `rate` from t = 0 (the values rounded to float); NaN samples are invalid.
    void addF32(const std::string& name, const std::vector<double>& values, Rate rate = Rate::fromHz(100)) {
        model::Channel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.unit = units::kNoUnit;
        ch.desc.quantity = shippedUnits()->quantityOf(ch.desc.unit);
        ch.desc.dtype = store::Dtype::F32;
        ch.desc.rate = rate;
        std::vector<float> samples;
        std::vector<bool> valid;
        for (const double v : values) {
            samples.push_back(static_cast<float>(v));
            valid.push_back(!std::isnan(v));
        }
        store::ColumnOptions options;
        options.cache = &rt_->cache;
        ch.series = store::UniformSeries{
            rate, 0,
            store::Column(store::InMemoryProvider::fromF32(
                              samples, valid, "kernel-test:" + name + ":" + std::to_string(++serial_)),
                          options)};
        channels_.push_back(std::move(ch));
        duration_ = std::max(duration_, rate.timeAt(static_cast<SampleIdx>(values.size()), 0));
    }

    // An F64 channel at `rate` from t = 0; NaN samples are invalid.
    void add(const std::string& name, const std::vector<double>& values, Rate rate = Rate::fromHz(100),
             std::string_view unit = "", store::Interp interp = store::Interp::Linear) {
        model::Channel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.unit = unit.empty() ? units::kNoUnit : unitOf(unit);
        ch.desc.quantity = shippedUnits()->quantityOf(ch.desc.unit);
        ch.desc.dtype = store::Dtype::F64;
        ch.desc.rate = rate;
        ch.desc.interp = interp;
        std::vector<bool> valid;
        for (const double v : values) {
            valid.push_back(!std::isnan(v));
        }
        store::ColumnOptions options;
        options.cache = &rt_->cache;
        ch.series = store::UniformSeries{
            rate, 0,
            store::Column(store::InMemoryProvider::fromF64(
                              values, valid, "kernel-test:" + name + ":" + std::to_string(++serial_)),
                          options)};
        channels_.push_back(std::move(ch));
        duration_ = std::max(duration_, rate.timeAt(static_cast<SampleIdx>(values.size()), 0));
    }

    model::RangeGroupTree& ranges() { return ranges_; }
    MathSelection& selection() { return selection_; }
    Micros duration() const { return duration_; }
    std::shared_ptr<const model::DataSource> source() {
        build();
        return src_;
    }

    struct Outcome {
        Result<analysis::UniformSignal> signal = fail(ErrorCode::Internal, "not evaluated");
        Diagnostics diags;
        std::optional<Plan> plan;
    };

    // A compiled plan and the context evaluateWhole() gets for it (prepare()).
    struct Prepared {
        Plan plan;
        EvalContext ctx;
    };

    // Parse, compile and evaluate over the layer extent at the plan's rate (else the highest input rate).
    Outcome eval(std::string_view expr, const MathItem& item = {}, const MathsCompat& compat = {}) {
        Outcome o;
        Result<Prepared> p = prepare(expr, o.diags, item, compat);
        if (!p) {
            o.signal = Failure{p.error()};
            return o;
        }
        o.signal = evaluateWhole(p->plan, p->ctx);
        o.plan = std::move(p->plan);
        return o;
    }

    // Parse and compile, and build the evaluation context of eval() (the layer extent at the plan's rate).
    Result<Prepared> prepare(std::string_view expr, Diagnostics& diags, const MathItem& item = {},
                             const MathsCompat& compat = {}) {
        build();
        Result<Ast> ast = Parser::parse(expr, compat, diags);
        if (!ast) {
            return Failure{ast.error()};
        }
        const ChannelScope scope(src_);
        Result<Plan> plan = compile(*ast, scope, item, compat, diags);
        if (!plan) {
            return Failure{plan.error()};
        }
        Rate rate = plan->out.rate && plan->out.rate->valid() ? *plan->out.rate : Rate{};
        if (!rate.valid()) { // no series input: the layer's highest logged rate (03 section 6.4 rule 3)
            for (const model::Channel& c : channels_) {
                if (!rate.valid() || rate < *c.desc.rate) {
                    rate = *c.desc.rate;
                }
            }
        }
        EvalContext ctx;
        ctx.source = src_;
        ctx.layer = "Normal";
        ctx.resultRate = rate;
        ctx.resultCount = static_cast<std::uint64_t>(rate.indexAt(duration_ - 1, 0) + 1);
        ctx.units = shippedUnits();
        ctx.ranges = analysis::RangeTables::build(src_->layers.front());
        ctx.selection = selection_;
        ctx.mathStartUs = selection_.main.end > selection_.main.start ? selection_.main.start : 0;
        ctx.mathEndUs = selection_.main.end > selection_.main.start ? selection_.main.end : duration_;
        const std::shared_ptr<const model::DataSource> source = src_;
        ctx.getChannel = [source](const ChannelKey& key, units::UnitId unit,
                                  Rate r) -> Result<MathInputGatherer> {
            const model::Channel* ch = source->layers.front().find(key.nameCf);
            if (ch == nullptr) {
                return fail(ErrorCode::NotFound, "no channel " + key.nameCf);
            }
            units::Converter conv;
            if (unit != ch->desc.unit) {
                Result<units::Converter> c = shippedUnits()->converter(ch->desc.unit, unit);
                if (!c) {
                    return Failure{c.error()};
                }
                conv = *c;
            }
            return MathInputGatherer(ch->series, r, 0, ch->desc.interp, conv, CancelToken{});
        };
        return Prepared{std::move(plan).value(), std::move(ctx)};
    }

    // The runtime's chunk cache and task pool.
    Runtime& runtime() { return *rt_; }

    // The values of a successful evaluation (NaN at invalid samples); fails the test otherwise.
    std::vector<double> values(std::string_view expr, const MathItem& item = {},
                               const MathsCompat& compat = {}) {
        Outcome o = eval(expr, item, compat);
        INFO(expr);
        if (!o.signal) {
            UNSCOPED_INFO(o.signal.error().message);
        }
        REQUIRE(o.signal.ok());
        std::vector<double> v = o.signal->values;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!o.signal->validAt(i)) {
                v[i] = kNaN;
            }
        }
        return v;
    }

private:
    class ChannelScope final : public Scope {
    public:
        explicit ChannelScope(std::shared_ptr<const model::DataSource> src)
            : Scope(FunctionRegistry::builtin(), *shippedUnits()), src_(std::move(src)) {}
        std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                         const ChannelKey& /*requester*/) const override {
            const model::Channel* ch = src_->layers.front().find(nameCf);
            if (ch == nullptr) {
                return std::nullopt;
            }
            ScopeEntry e;
            e.what = ScopeEntry::Channel;
            e.type.kind = Kind::Series;
            e.type.unit = ch->desc.unit;
            e.type.rate = ch->desc.rate;
            e.key = ChannelKey::make(src_->id, "Normal", ch->desc.name);
            e.resolvedName = ch->desc.name;
            e.interp = ch->desc.interp;
            e.dtype = ch->desc.dtype;
            return e;
        }

    private:
        std::shared_ptr<const model::DataSource> src_;
    };

    void build() {
        if (src_ != nullptr && built_ == channels_.size()) {
            return;
        }
        std::vector<model::Channel> copy = channels_;
        auto src = makeSource(std::move(copy), duration_, "kernel-test-" + std::to_string(++serial_));
        src->layers.front().ranges = ranges_;
        src_ = src;
        built_ = channels_.size();
        selection_.main.ref.src = src_->id;
        selection_.main.ref.layer = "Normal";
        if (selection_.reference) {
            selection_.reference->ref.src = src_->id;
            selection_.reference->ref.layer = "Normal";
        }
    }

    std::shared_ptr<Runtime> rt_;
    std::vector<model::Channel> channels_;
    model::RangeGroupTree ranges_;
    MathSelection selection_;
    Micros duration_ = 0;
    std::shared_ptr<model::DataSource> src_;
    std::size_t built_ = 0;
    inline static std::uint64_t serial_ = 0;
};

// Relative-or-absolute closeness with NaN == NaN.
inline bool close(double got, double want, double tol = 1e-9) {
    if (std::isnan(want) || std::isnan(got)) {
        return std::isnan(want) && std::isnan(got);
    }
    return std::abs(got - want) <= tol * std::max(1.0, std::abs(want));
}

} // namespace esses::maths::test
