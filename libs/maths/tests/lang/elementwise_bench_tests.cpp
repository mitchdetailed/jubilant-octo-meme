// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The elementwise budget of 07 section 12 / AC-M2-14 (2.16 M samples <= 10 ms on one core), as a hidden
// measurement: `esses_maths_tests "[.elementwise-bench]"` in a release build prints the median of 7 runs of
// evaluateWhole (gather + fused kernel + result) and of the EwKernel alone. No timing is asserted here (the
// gate is bench/maths_bench on the reference machine, D-042); the values are checked.

#include "lang_test_support.hpp"

#include <esses/maths/eval_context.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/plan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

class OneChannelScope final : public Scope {
public:
    OneChannelScope(const model::DataSource& src)
        : Scope(FunctionRegistry::builtin(), *shippedUnits()), src_(src) {}
    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        const model::Channel* ch = src_.layers.front().find(nameCf);
        if (ch == nullptr) {
            return std::nullopt;
        }
        ScopeEntry e;
        e.type.kind = Kind::Series;
        e.type.unit = ch->desc.unit;
        e.type.rate = ch->desc.rate;
        e.key = ChannelKey::make(src_.id, "Normal", ch->desc.name);
        return e;
    }

private:
    const model::DataSource& src_;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

TEST_CASE("elementwise budget: 2.16 M samples through evaluateWhole (measurement)", "[.elementwise-bench]") {
    constexpr std::size_t kSamples = 2'160'000;
    Runtime rt(1);
    std::vector<float> xs(kSamples);
    for (std::size_t i = 0; i < kSamples; ++i) {
        xs[i] = static_cast<float>(std::sin(static_cast<double>(i) * 1e-4) * 50.0 + 50.0);
    }
    auto src = makeSource({uniformF32(rt.cache, "Ground Speed", unitOf("m/s"), Rate::fromHz(1000), xs)},
                          static_cast<Micros>(kSamples) * 1000);
    const OneChannelScope scope(*src);
    for (const char* expr : {"'Ground Speed'[m/s] * 2", "'Ground Speed'[km/h] * 2 + 1",
                             "choose('Ground Speed'[km/h] > 100, 'Ground Speed'[km/h] / 3.6, 0) * 2 - 1"}) {
        Diagnostics diags;
        Result<Ast> ast = Parser::parse(expr, {}, diags);
        REQUIRE(ast.ok());
        Result<Plan> plan = compile(*ast, scope, MathItem{}, {}, diags);
        REQUIRE(plan.ok());
        EvalContext ctx;
        ctx.source = src;
        ctx.layer = "Normal";
        ctx.resultRate = Rate::fromHz(1000);
        ctx.resultCount = kSamples;
        ctx.units = shippedUnits();
        ctx.getChannel = [src](const ChannelKey& key, units::UnitId unit,
                               Rate r) -> Result<MathInputGatherer> {
            const model::Channel* ch = src->layers.front().find(key.nameCf);
            units::Converter conv;
            if (unit != ch->desc.unit) {
                conv = *shippedUnits()->converter(ch->desc.unit, unit);
            }
            return MathInputGatherer(ch->series, r, 0, ch->desc.interp, conv, CancelToken{});
        };
        std::vector<double> whole;
        double last = 0.0;
        for (int run = 0; run < 7; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            Result<analysis::UniformSignal> out = evaluateWhole(*plan, ctx);
            const auto t1 = std::chrono::steady_clock::now();
            REQUIRE(out.ok());
            REQUIRE(out->values.size() == kSamples);
            last = out->values[kSamples / 2];
            whole.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        CHECK(std::isfinite(last));

        // The fused kernel alone over pre-gathered F64 blocks.
        Kernel& k = *plan->stages.back().kernel;
        std::vector<double> in(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i) {
            in[i] = static_cast<double>(xs[i]);
        }
        std::vector<double> out(kSamples);
        std::vector<std::uint8_t> bits(kSamples / 8 + 1);
        std::vector<double> kernelOnly;
        for (int run = 0; run < 7; ++run) {
            BlockCtx b;
            b.n = static_cast<std::uint32_t>(kSamples);
            b.rate = Rate::fromHz(1000);
            const BlockView iv{in.data(), nullptr, store::Dtype::F64, static_cast<std::uint32_t>(kSamples)};
            const BlockView ov{out.data(), bits.data(), store::Dtype::F64,
                               static_cast<std::uint32_t>(kSamples)};
            const auto t0 = std::chrono::steady_clock::now();
            k.run(b, std::span<const BlockView>(&iv, 1), ov);
            const auto t1 = std::chrono::steady_clock::now();
            kernelOnly.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::printf("elementwise %s: evaluateWhole median %.2f ms, kernel median %.2f ms (%zu samples)\n",
                    expr, median(whole), median(kernelOnly), kSamples);
    }
}
