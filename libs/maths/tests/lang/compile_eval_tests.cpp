// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// compile() and the 0.1 whole-node evaluation (03 sections 4.4, 6.0-6.6, 7.2; D-022, D-139): the plan's
// stages, inputs, dtype, defHash; golden values of elementwise plans over in-memory channels (unit
// conversions, the result unit to base conversion, validity rules of 03 section 6.6, mixed rates, constants,
// text details, deterministic random functions); block-size independence (03 section 6.7) and cancellation.
// Expected values are written from 03 section 5.1's semantics, not taken from the code under test (D-041).

#include "lang/codes.hpp"
#include "lang/ew_kernel.hpp"
#include "lang_test_support.hpp"

#include <esses/maths/eval_context.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/plan.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A data-backed scope over one source's "Normal" layer: channels by name, then numeric and text details.
class SourceScope final : public Scope {
public:
    explicit SourceScope(std::shared_ptr<const model::DataSource> src)
        : Scope(FunctionRegistry::builtin(), *shippedUnits()), src_(std::move(src)) {}

    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        const model::DataLayer& layer = src_->layers.front();
        if (const model::Channel* ch = layer.find(nameCf)) {
            ScopeEntry e;
            e.what = ScopeEntry::Channel;
            e.type.kind = Kind::Series;
            e.type.unit = ch->desc.unit;
            e.type.rate = ch->desc.rate;
            e.type.integer = ch->desc.dtype == store::Dtype::I32;
            e.key = ChannelKey::make(src_->id, "Normal", ch->desc.name);
            e.interp = ch->desc.interp;
            e.dtype = ch->desc.dtype;
            return e;
        }
        if (const model::Detail* d = model::findDetail(src_->details, nameCf)) {
            ScopeEntry e;
            if (const auto* text = std::get_if<std::string>(&d->value)) {
                e.what = ScopeEntry::DetailText;
                e.type.kind = Kind::Text;
                e.text = *text;
            } else if (const auto* q = std::get_if<model::Quantityd>(&d->value)) {
                e.what = ScopeEntry::DetailNumber;
                e.type.kind = Kind::Scalar;
                e.number = q->v;
            }
            return e;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<const model::DataSource> src_;
};

struct Fixture {
    Runtime rt;
    std::shared_ptr<model::DataSource> src;

    explicit Fixture(std::vector<model::Channel> channels, Micros duration = 800'000,
                     std::string_view tag = "lang-eval") {
        src = makeSource(std::move(channels), duration, tag);
    }

    model::Channel f32(const std::string& name, std::string_view unit, Rate rate, const std::vector<float>& v,
                       std::vector<bool> valid = {}) {
        return uniformF32(rt.cache, name, unit.empty() ? units::kNoUnit : unitOf(unit), rate, v,
                          std::move(valid));
    }

    EvalContext context(Rate rate, std::uint64_t count) const {
        EvalContext ctx;
        ctx.source = src;
        ctx.layer = "Normal";
        ctx.resultRate = rate;
        ctx.resultFirst = 0;
        ctx.resultCount = count;
        ctx.units = shippedUnits();
        const std::shared_ptr<const model::DataSource> source = src;
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
        return ctx;
    }
};

struct Compiled {
    Result<Plan> plan = fail(ErrorCode::Internal, "not compiled");
    Diagnostics diags;
};

Compiled compileExpr(std::string_view src, const Scope& scope, const MathItem& item = {},
                     const MathsCompat& compat = {}) {
    Compiled c;
    Result<Ast> ast = Parser::parse(src, compat, c.diags);
    REQUIRE(ast.ok());
    c.plan = compile(*ast, scope, item, compat, c.diags);
    return c;
}

analysis::UniformSignal evaluate(Fixture& f, std::string_view src, Rate rate, std::uint64_t count,
                                 const MathItem& item = {}) {
    const SourceScope scope(f.src);
    Compiled c = compileExpr(src, scope, item);
    INFO(src);
    for (const Diagnostic& d : c.diags) {
        UNSCOPED_INFO(d.code << ": " << d.message);
    }
    REQUIRE(c.plan.ok());
    Result<analysis::UniformSignal> out = evaluateWhole(*c.plan, f.context(rate, count));
    if (!out) {
        UNSCOPED_INFO(out.error().message);
    }
    REQUIRE(out.ok());
    REQUIRE(out->values.size() == count);
    return std::move(out).value();
}

void checkValues(const analysis::UniformSignal& s, const std::vector<double>& want, double tol = 1e-6) {
    REQUIRE(s.values.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        INFO("sample " << i);
        if (std::isnan(want[i])) {
            CHECK_FALSE(s.validAt(i));
            CHECK(std::isnan(s.values[i]));
        } else {
            CHECK(s.validAt(i));
            CHECK(s.values[i] == Catch::Approx(want[i]).margin(tol));
        }
    }
}

MathItem itemWith(std::string_view resultUnit) {
    MathItem item;
    item.resultUnit = resultUnit.empty() ? units::kNoUnit : unitOf(resultUnit);
    return item;
}

} // namespace

TEST_CASE("compile+eval: elementwise arithmetic over a channel", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(10), {0, 1, 2, 3, 4, 5, 6, 7}));
    const analysis::UniformSignal s = evaluate(f, "'X' * 2 + 1", Rate::fromHz(10), 8);
    CHECK(s.rate == Rate::fromHz(10));
    CHECK(s.t0 == 0);
    CHECK(s.valid.empty());
    checkValues(s, {1, 3, 5, 7, 9, 11, 13, 15});
    checkValues(evaluate(f, "-'X'^2", Rate::fromHz(10), 4), {-0.0, -1, -4, -9});
    checkValues(evaluate(f, "'X' / 2", Rate::fromHz(10), 3), {0, 0.5, 1});
    checkValues(evaluate(f, "'X' > 3 && 'X' < 6", Rate::fromHz(10), 8), {0, 0, 0, 0, 1, 1, 0, 0});
    checkValues(evaluate(f, "remainder('X', 3) + abs(1 - 'X')", Rate::fromHz(10), 5), {1, 1, 3, 2, 4});
    checkValues(evaluate(f, "choose('X' >= 4, 'X', -'X')", Rate::fromHz(10), 6), {0, -1, -2, -3, 4, 5});
    checkValues(evaluate(f, "round('X' / 2) + int(-'X' / 2)", Rate::fromHz(10), 4), {0, 1, 0, 1});
}

TEST_CASE("compile+eval: [unit] conversions and the result unit to base conversion", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("S", "m/s", Rate::fromHz(10), {0, 10, 20}));
    f.src->layers.front().addChannel(f.f32("T", "K", Rate::fromHz(10), {273.15f, 283.15f, 373.15f}));
    // Numbers in km/h, no result unit: the values stay km/h numbers.
    checkValues(evaluate(f, "'S'[km/h]", Rate::fromHz(10), 3), {0, 36, 72}, 1e-4);
    // Declared km/h: stored in the base unit (m/s), so 'S'[km/h] comes back as S.
    checkValues(evaluate(f, "'S'[km/h] * 2", Rate::fromHz(10), 3, itemWith("km/h")), {0, 20, 40}, 1e-4);
    // Affine conversion of an absolute temperature, and back to kelvin.
    checkValues(evaluate(f, "'T'[deg C]", Rate::fromHz(10), 3), {0, 10, 100}, 1e-3);
    checkValues(evaluate(f, "'T'[deg C] + 10", Rate::fromHz(10), 3, itemWith("deg C")),
                {283.15, 293.15, 383.15}, 1e-3);
    // A unitless channel read in [ratio] (lang.unit-family): the stored numbers, no conversion.
    f.src->layers.front().addChannel(f.f32("U", "", Rate::fromHz(10), {0.25f, 2.0f, -3.0f}));
    checkValues(evaluate(f, "'U'[ratio] * 2", Rate::fromHz(10), 3), {0.5, 4, -6});
}

TEST_CASE("compile+eval: validity rules of 03 section 6.6 and the exceptions of 5.1", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(
        f.f32("X", "", Rate::fromHz(10), {1, 2, 3, 4, 5}, {true, true, false, true, true}));
    f.src->layers.front().addChannel(f.f32("Z", "", Rate::fromHz(10), {0, 1, 0, 2, 0}));
    checkValues(evaluate(f, "'X' + 1", Rate::fromHz(10), 5), {2, 3, kNaN, 5, 6});
    checkValues(evaluate(f, "'X' / 'Z'", Rate::fromHz(10), 5), {kNaN, 2, kNaN, 2, kNaN}); // / 0 is invalid
    checkValues(evaluate(f, "min('X', 'Z' * 10)", Rate::fromHz(10), 5),
                {0, 2, 0, 4, 0}); // one invalid: other
    checkValues(evaluate(f, "choose('Z' > 0, 'X', 7)", Rate::fromHz(10), 5),
                {7, 2, 7, 4, 7}); // chosen branch
    checkValues(evaluate(f, "choose('X' > 0, 1, 2)", Rate::fromHz(10), 5),
                {1, 1, kNaN, 1, 1});                                              // invalid cond
    checkValues(evaluate(f, "'X' > 2", Rate::fromHz(10), 5), {0, 0, kNaN, 1, 1}); // comparisons propagate
    checkValues(evaluate(f, "choose('Z' > 0, invalid(), 'Z')", Rate::fromHz(10), 5), {0, kNaN, 0, kNaN, 0});
    checkValues(evaluate(f, "sqrt('Z' - 1) + ln('Z')", Rate::fromHz(10), 5),
                {kNaN, 0, kNaN, 1 + std::log(2.0), kNaN});
    const analysis::UniformSignal s = evaluate(f, "'X'", Rate::fromHz(10), 5);
    REQUIRE(s.valid.size() == 5);
    CHECK(s.valid == std::vector<std::uint8_t>{1, 1, 0, 1, 1});
}

TEST_CASE("compile+eval: mixed rates resample to the highest rate; explicit and constant-only rates",
          "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("A", "", Rate::fromHz(10), {0, 1, 2, 3, 4, 5, 6, 7}));
    f.src->layers.front().addChannel(f.f32("B", "", Rate::fromHz(5), {0, 10, 20, 30}));
    const SourceScope scope(f.src);
    Compiled c = compileExpr("'A' + 'B'", scope);
    REQUIRE(c.plan.ok());
    REQUIRE(c.plan->out.rate.has_value());
    CHECK(*c.plan->out.rate == Rate::fromHz(10));
    // B at 10 Hz by linear interpolation: 0, 5, 10, ..., 30, then outside its extent (invalid).
    checkValues(evaluate(f, "'A' + 'B'", Rate::fromHz(10), 8), {0, 6, 12, 18, 24, 30, 36, kNaN});

    MathItem explicitRate;
    explicitRate.rate.automatic = false;
    explicitRate.rate.rate = Rate::fromHz(20);
    Compiled e = compileExpr("'B' * 2", scope, explicitRate);
    REQUIRE(e.plan.ok());
    CHECK(*e.plan->out.rate == Rate::fromHz(20));

    Compiled k = compileExpr("2 + 3", scope);
    REQUIRE(k.plan.ok());
    CHECK_FALSE(k.plan->out.rate.has_value()); // the host takes the layer's highest logged rate
    CHECK(k.plan->inputs.empty());
    checkValues(evaluate(f, "2 + 3", Rate::fromHz(10), 4), {5, 5, 5, 5});
}

TEST_CASE("compile+eval: text details fold into the plan; constants are immediates", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(10), {1, 2, 3}));
    model::Detail driver;
    driver.id = "Driver";
    driver.value = std::string("Joe");
    f.src->details.push_back(driver);
    model::Detail ratio;
    ratio.id = "Diff Ratio";
    ratio.value = model::Quantityd{3.5, units::kNoUnit, 2, ""};
    f.src->details.push_back(ratio);
    checkValues(evaluate(f, "choose('Driver' == \"Joe\", 'X', 0)", Rate::fromHz(10), 3), {1, 2, 3});
    checkValues(evaluate(f, "choose('Driver' > \"Kim\", 'X', 0)", Rate::fromHz(10), 3), {0, 0, 0});
    checkValues(evaluate(f, "'X' * 'Diff Ratio'", Rate::fromHz(10), 3), {3.5, 7, 10.5});

    const SourceScope scope(f.src);
    Compiled c = compileExpr("'X' * (2 + 3) * 'Diff Ratio'", scope);
    REQUIRE(c.plan.ok());
    REQUIRE(c.plan->stages.size() == 1);
    const auto* ew = dynamic_cast<const lang::EwKernel*>(c.plan->stages[0].kernel.get());
    REQUIRE(ew != nullptr);
    // One load and two multiplications by immediates; nothing for the folded constants.
    CHECK(ew->code().size() == 3);
}

TEST_CASE("compile: plan inputs, stages, dtype and the definition hash", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "m/s", Rate::fromHz(10), {1, 2, 3}));
    f.src->layers.front().addChannel(f.f32("G", "", Rate::fromHz(10), {1, 2, 3}));
    model::Channel i32 = f.f32("N", "", Rate::fromHz(10), {1, 2, 3});
    i32.desc.dtype = store::Dtype::I32;
    f.src->layers.front().addChannel(i32);
    model::Channel f64 = f.f32("Lat", "", Rate::fromHz(10), {1, 2, 3});
    f64.desc.dtype = store::Dtype::F64;
    f.src->layers.front().addChannel(f64);
    const SourceScope scope(f.src);

    Compiled same = compileExpr("'X'[m/s] + 'X'[m/s] * 2", scope);
    REQUIRE(same.plan.ok());
    CHECK(same.plan->inputs.size() == 1);
    CHECK(same.plan->stages.size() == 1);
    CHECK(same.plan->stages.back().kernel->cls() == KernelClass::Elementwise);
    CHECK(same.plan->extent == ExtentMode::Whole);
    CHECK_FALSE(same.plan->selectionDependent);
    Compiled two = compileExpr("'X'[km/h] + 'X'[m/s]", scope);
    REQUIRE(two.plan.ok());
    REQUIRE(two.plan->inputs.size() == 2);
    CHECK(two.plan->inputs[0].unit == unitOf("km/h"));
    CHECK(two.plan->inputs[0].conv.a == Catch::Approx(3.6));
    CHECK(two.plan->inputs[1].conv.identity());
    CHECK(two.plan->inputs[0].key == ChannelKey::make(f.src->id, "Normal", "X"));

    CHECK(compileExpr("'X'[m/s] * 2", scope).plan->outDtype == store::Dtype::F32);
    CHECK(compileExpr("'N' + 1", scope).plan->outDtype == store::Dtype::I32);
    CHECK(compileExpr("'G' > 1", scope).plan->outDtype == store::Dtype::I32);
    CHECK(compileExpr("'Lat' * 2", scope).plan->outDtype == store::Dtype::F64);
    MathItem dbl;
    dbl.precision = "double";
    CHECK(compileExpr("'G' * 2", scope, dbl).plan->outDtype == store::Dtype::F64);
    MathItem enumerated;
    enumerated.enumeration = "Balance";
    CHECK(compileExpr("'G' * 2", scope, enumerated).plan->outDtype == store::Dtype::I32);

    // defHash: cosmetic edits keep it; the definition, the item and the compat switches change it.
    const Hash128 h = compileExpr("'X'[m/s] * 2", scope).plan->defHash;
    CHECK(compileExpr("'x'[m/s]*2 // twice", scope).plan->defHash == h);
    CHECK(compileExpr("'X'[m/s] * 3", scope).plan->defHash != h);
    CHECK(compileExpr("'X'[m/s] * 2", scope, itemWith("m/s")).plan->defHash != h);
    MathItem rated;
    rated.rate.automatic = false;
    rated.rate.rate = Rate::fromHz(20);
    CHECK(compileExpr("'X'[m/s] * 2", scope, rated).plan->defHash != h);
    // The power switch is not hashed: it changes only the parse of a signed base under '^', whose canonical
    // text differs between the settings (03 section 2.3, D-185); a function alias is hashed.
    MathsCompat compat;
    compat.unaryMinusAbovePower = true;
    CHECK(compileExpr("'X'[m/s] * 2", scope, MathItem{}, compat).plan->defHash == h);
    const Hash128 pow = compileExpr("-'X'[m/s] ^ 2", scope).plan->defHash;
    CHECK(compileExpr("-'X'[m/s] ^ 2", scope, MathItem{}, compat).plan->defHash != pow);
    MathsCompat aliased;
    aliased.functionAliases["running_lo"] = "running_min";
    CHECK(compileExpr("'X'[m/s] * 2", scope, MathItem{}, aliased).plan->defHash != h);
}

TEST_CASE("compile: refusals and their codes", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(10), {1, 2, 3}));
    const SourceScope scope(f.src);

    MathItem zero;
    zero.rate.automatic = false;
    zero.rate.rate = Rate{0, 1};
    Compiled z = compileExpr("'X'", scope, zero);
    REQUIRE_FALSE(z.plan.ok());
    CHECK(hasCode(z.diags, codes::kRateZero));

    MathItem t3;
    t3.kind = ItemKind::Table3D;
    CHECK(hasCode(compileExpr("'X'", scope, t3).diags, codes::kKindUnsupported));
    MathItem plugin;
    plugin.kind = ItemKind::Plugin;
    CHECK(hasCode(compileExpr("'X'", scope, plugin).diags, codes::kPluginUnavailable));

    // Type errors stop compilation; warnings do not (D-021).
    Compiled typeError = compileExpr("'X' & 1.5", scope);
    REQUIRE_FALSE(typeError.plan.ok());
    CHECK(typeError.plan.error().context.detail == std::string(codes::kIntegerRequired));
    Compiled warned = compileExpr("'X' + 1", scope, itemWith("m"));
    CHECK(warned.plan.ok());
    CHECK(hasCode(warned.diags, codes::kResultDim));

    // Without data a reference cannot be evaluated.
    const Scope dataless(FunctionRegistry::builtin(), *shippedUnits());
    Compiled unresolved = compileExpr("'Nope' * 2", dataless);
    REQUIRE_FALSE(unresolved.plan.ok());
    CHECK(hasCode(unresolved.diags, codes::kUnresolved));
    CHECK(hasCode(unresolved.diags, codes::kUnknownChannel));
}

TEST_CASE("compile: a non-elementwise call is its own stage with its series inputs", "[unit]") {
    Fixture f({});
    model::Channel prev = f.f32("P", "", Rate::fromHz(10), {1, 2, 3});
    prev.desc.interp = store::Interp::Previous;
    f.src->layers.front().addChannel(prev);
    const SourceScope scope(f.src);
    Compiled c = compileExpr("smooth('P', 3) * 2 + 'P'", scope);
    if (!c.plan.ok()) {
        // The kernel of smooth is not available before src/kernels lands.
        CHECK(hasCode(c.diags, codes::kKernelUnavailable));
        return;
    }
    const Plan& p = *c.plan;
    REQUIRE(p.stages.size() == 2);
    CHECK(p.stages[0].kernel->cls() == KernelClass::Windowed);
    REQUIRE(p.stages[0].inputs.size() == 1);
    CHECK(p.stages[0].inputs[0].from == Stage::Ref::From::Input);
    // smooth's first argument is resampled Linear (03 section 6.4 rule 6); the other reference keeps
    // Previous.
    CHECK(p.inputs[p.stages[0].inputs[0].index].interp == store::Interp::Linear);
    CHECK(p.inputs.size() == 2);
    CHECK(p.stages[1].kernel->cls() == KernelClass::Elementwise);
}

TEST_CASE("eval: random functions are deterministic per definition and source (D-087)", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(10), {1, 2, 3}));
    const analysis::UniformSignal a = evaluate(f, "rand_chan()", Rate::fromHz(10), 64);
    const analysis::UniformSignal b = evaluate(f, "rand_chan()", Rate::fromHz(10), 64);
    CHECK(a.values == b.values);
    bool varies = false;
    for (const double v : a.values) {
        CHECK(v >= 0.0);
        CHECK(v < 1.0);
        varies = varies || v != a.values.front();
    }
    CHECK(varies);
    const analysis::UniformSignal r = evaluate(f, "rand_val() + 0 * 'X'", Rate::fromHz(10), 3);
    CHECK(r.values[0] == r.values[1]);
    CHECK(r.values[0] == evaluate(f, "rand_val() + 0 * 'X'", Rate::fromHz(10), 3).values[0]);
    Fixture other({}, 800'000, "another-source");
    other.src->layers.front().addChannel(other.f32("X", "", Rate::fromHz(10), {1, 2, 3}));
    CHECK(evaluate(other, "rand_chan()", Rate::fromHz(10), 64).values != a.values);
}

TEST_CASE("eval: results do not depend on the block size (03 section 6.7)", "[unit]") {
    const std::size_t n = 3 * kBlockSamples + 123;
    std::vector<float> xs(n);
    std::vector<bool> valid(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 100.0);
        valid[i] = i % 97 != 0;
    }
    Fixture f({}, static_cast<Micros>(n) * 1000);
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(1000), xs, valid));
    const std::string expr = "choose('X' > 0, sqrt('X') * 2, min('X', -1)) + rand_chan()";
    const analysis::UniformSignal whole = evaluate(f, expr, Rate::fromHz(1000), n);

    // The same kernel driven in uneven pieces gives bit-identical values.
    const SourceScope scope(f.src);
    Compiled c = compileExpr(expr, scope);
    REQUIRE(c.plan.ok());
    REQUIRE(c.plan->stages.size() == 1);
    Kernel& k = *c.plan->stages[0].kernel;
    EvalContext ctx = f.context(Rate::fromHz(1000), n);
    k.reset(ctx);
    Result<MathInputGatherer> g =
        ctx.getChannel(c.plan->inputs[0].key, c.plan->inputs[0].unit, Rate::fromHz(1000));
    REQUIRE(g.ok());
    std::vector<double> in(n);
    std::vector<std::uint8_t> inBits((n + 7) / 8);
    BlockView inView{in.data(), inBits.data(), store::Dtype::F64, static_cast<std::uint32_t>(n)};
    REQUIRE(g->fill(0, static_cast<std::uint32_t>(n), inView).ok());
    std::vector<double> out(n);
    std::vector<std::uint8_t> outBits((n + 7) / 8);
    for (const std::uint32_t piece : {1000u, 8u, 4096u * 2u}) {
        for (std::uint32_t at = 0; at < n; at += piece) {
            const std::uint32_t len = std::min<std::uint32_t>(piece, static_cast<std::uint32_t>(n) - at);
            // Pieces start on byte boundaries of the bitmaps (multiples of 8 samples).
            if (at % 8 != 0) {
                continue;
            }
            BlockCtx b;
            b.first = at;
            b.n = len;
            b.rate = Rate::fromHz(1000);
            BlockView iv{in.data() + at, inBits.data() + at / 8, store::Dtype::F64, len};
            BlockView ov{out.data() + at, outBits.data() + at / 8, store::Dtype::F64, len};
            k.run(b, std::span<const BlockView>(&iv, 1), ov);
        }
        if (piece % 8 != 0) {
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            INFO("piece " << piece << " sample " << i);
            const double want = whole.values[i];
            if (std::isnan(want)) {
                REQUIRE(std::isnan(out[i]));
            } else {
                REQUIRE(static_cast<double>(static_cast<float>(out[i])) == want);
            }
        }
    }
}

TEST_CASE("eval: a cancelled evaluation fails with Cancelled", "[unit]") {
    Fixture f({});
    f.src->layers.front().addChannel(f.f32("X", "", Rate::fromHz(10), {1, 2, 3}));
    const SourceScope scope(f.src);
    Compiled c = compileExpr("'X' * 2", scope);
    REQUIRE(c.plan.ok());
    EvalContext ctx = f.context(Rate::fromHz(10), 3);
    CancelSource cancel;
    ctx.cancel = cancel.token();
    cancel.cancel();
    Result<analysis::UniformSignal> out = evaluateWhole(*c.plan, ctx);
    REQUIRE_FALSE(out.ok());
    CHECK(out.error().code == ErrorCode::Cancelled);
}

TEST_CASE("eval: integer channels and bit operations are exact", "[unit]") {
    Fixture f({});
    model::Channel n = f.f32("Status", "", Rate::fromHz(10), {0, 5, 12, 255});
    n.desc.dtype = store::Dtype::I32;
    f.src->layers.front().addChannel(n);
    checkValues(evaluate(f, "'Status' & 4", Rate::fromHz(10), 4), {0, 4, 4, 4});
    checkValues(evaluate(f, "'Status' | 16", Rate::fromHz(10), 4), {16, 21, 28, 255});
    checkValues(evaluate(f, "bit_xor('Status', 1)", Rate::fromHz(10), 4), {1, 4, 13, 254});
    checkValues(evaluate(f, "bit_decode('Status', 2)", Rate::fromHz(10), 4), {0, 1, 1, 1});
    checkValues(evaluate(f, "bit_decode('Status', 2, 2)", Rate::fromHz(10), 4), {0, 1, 3, 3});
    checkValues(evaluate(f, "~'Status'", Rate::fromHz(10), 4), {-1, -6, -13, -256});
}
