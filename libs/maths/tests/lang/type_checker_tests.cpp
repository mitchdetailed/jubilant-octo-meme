// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TypeChecker (03 section 3.2, D-021, D-085): kinds, units and dimension vectors bottom-up, every checker
// diagnostic code, warnings never blocking (D-021), text comparisons, choose unification, LapScalar, the
// data-less scope (I-UNRESOLVED), and the agreement of the language's intrinsic signatures with the registry
// manifests.

#include "lang/codes.hpp"
#include "lang/signatures.hpp"
#include "lang_test_support.hpp"

#include <esses/maths/function_registry.hpp>
#include <esses/maths/intrinsics.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;
using units::Dimension;

namespace {

const TestScope& scope() {
    static const TestScope s = [] {
        TestScope t;
        t.channel("Speed", "m/s", Rate::fromHz(100));
        t.channel("Wheel Speed FL", "m/s", Rate::fromHz(50));
        t.channel("Dist", "m", Rate::fromHz(20));
        t.channel("Lat G", "m/s/s", Rate::fromHz(100));
        t.channel("Throttle", "ratio", Rate::fromHz(50));
        t.channel("Brake Temp", "K", Rate::fromHz(10));
        t.channel("Steer", "rad", Rate::fromHz(100));
        t.channel("Gear", "", Rate::fromHz(10), store::Dtype::I32, store::Interp::Previous);
        t.channel("Status", "", Rate::fromHz(10), store::Dtype::I32);
        t.channel("Flag", "", Rate::fromHz(10));
        t.constant("Wheelbase", 2.7, "m");
        t.constant("Gain", 2.0);
        t.constant("Window", 5.0);
        t.constant("Half", 0.5);
        t.constant("Delay", 0.25, "s");
        t.constant("Diff Ratio", 3.2, "", ScopeEntry::DetailNumber);
        t.text("Driver", "Joe");
        ScopeEntry& amb = t.constant("Shared", 1.0);
        amb.diags.push_back(Diagnostic{"W-CONST-AMBIGUOUS", Diagnostic::Warning, {}, "defined twice", {}});
        return t;
    }();
    return s;
}

Checked checkOk(std::string_view src, std::optional<std::string_view> resultUnit = {}) {
    Checked c = check(src, scope(), resultUnit);
    INFO(src);
    for (const Diagnostic& d : c.diags) {
        UNSCOPED_INFO(d.code << ": " << d.message);
    }
    REQUIRE(c.parsed);
    CHECK_FALSE(hasError(c.diags));
    CHECK(c.type.kind != Kind::Invalid);
    return c;
}

Checked checkErr(std::string_view src, std::string_view code) {
    Checked c = check(src, scope());
    INFO(src << " expects " << code);
    for (const Diagnostic& d : c.diags) {
        UNSCOPED_INFO(d.code << ": " << d.message);
    }
    REQUIRE(c.parsed);
    CHECK(hasCode(c.diags, code));
    CHECK(hasError(c.diags));
    CHECK(c.type.kind == Kind::Invalid);
    return c;
}

Dimension dimOf(std::string_view unit) {
    const units::UnitRegistry& u = *shippedUnits();
    return u.quantity(u.quantityOf(unitOf(unit))).dim;
}

} // namespace

TEST_CASE("checker: literals, references and arithmetic give kinds, units, dimensions and rates", "[unit]") {
    Checked c = checkOk("1 + 2");
    CHECK(c.type.kind == Kind::Scalar);
    CHECK(c.type.integer);
    CHECK_FALSE(checkOk("1 / 2").type.integer); // integer division is floating (03 section 2.4 rule 1)
    CHECK_FALSE(checkOk("1.0 + 2").type.integer);

    c = checkOk("'Speed'[m/s] * 2");
    CHECK(c.type.kind == Kind::Series);
    CHECK(c.type.rate == Rate::fromHz(100));
    CHECK(c.type.dim == dimOf("m/s"));
    CHECK(c.type.unit == unitOf("m/s"));

    c = checkOk("'Speed'[km/h]");
    CHECK(c.type.unit == unitOf("km/h"));
    CHECK(c.type.scaleToBase == Catch::Approx(3.6));
    CHECK(c.ast.nodes[c.ast.root].unit == unitOf("km/h"));

    // Auto rate: the highest rate among the series inputs (03 section 6.4 rule 2).
    CHECK(checkOk("'Dist'[m] + 'Speed'[m/s] * 'Delay'[s]").type.rate == Rate::fromHz(100));
    CHECK(checkOk("'Wheel Speed FL'[m/s] + 1").type.rate == Rate::fromHz(50));

    CHECK(checkOk("'Speed'[m/s] / 'Wheelbase'[m]").type.dim == Dimension::basis(Dimension::Time, -1));
    CHECK(checkOk("'Speed'[m/s] * 'Delay'[s]").type.dim == dimOf("m"));
    CHECK(checkOk("'Speed'[m/s]^2").type.dim == dimOf("m/s").pow(2));
    CHECK(checkOk("'Speed'[m/s]^-1").type.dim == dimOf("m/s").inverse());
    CHECK(checkOk("'Speed'[m/s]^0.5").type.dim.isUnknown());
    CHECK(checkOk("sqrt('Speed'[m/s]^2)").type.dim == dimOf("m/s"));
    CHECK(checkOk("sqr('Dist'[m])").type.dim == dimOf("m").pow(2));
    CHECK(checkOk("'Gear' + 1").type.integer);
    CHECK(checkOk("'Gear' * 2 - 1").type.integer);
}

TEST_CASE("checker: dimension and unit findings are warnings and never block (D-021)", "[unit]") {
    Checked c = checkOk("'Speed'[m/s] + 'Dist'[m]");
    CHECK(hasCode(c.diags, codes::kDimMismatch));
    CHECK(findCode(c.diags, codes::kDimMismatch)->level == Diagnostic::Warning);

    c = checkOk("'Speed'[km/h] + 'Wheel Speed FL'[m/s]");
    CHECK(hasCode(c.diags, codes::kUnitScale));
    CHECK(findCode(c.diags, codes::kUnitScale)->level == Diagnostic::Info);
    CHECK_FALSE(hasCode(c.diags, codes::kDimMismatch));

    // A bare literal carries no unit: no mismatch against a dimensioned operand.
    for (const char* src : {"'Speed'[km/h] > 100", "'Brake Temp'[deg C] - 5", "max('Speed'[m/s], 0)",
                            "choose('Flag' > 0, 'Speed'[m/s], 0)", "100 < 'Speed'[km/h] * 2"}) {
        INFO(src);
        CHECK_FALSE(hasCode(checkOk(src).diags, codes::kDimMismatch));
    }
    CHECK(hasCode(checkOk("'Speed'[m/s] > 'Dist'[m]").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("2^'Dist'[m]").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("exp('Dist'[m])").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("min('Speed'[m/s], 'Lat G'[m/s/s])").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("choose(1, 'Speed'[m/s], 'Dist'[m])").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("sin('Speed'[m/s])").diags, codes::kDimMismatch));
    CHECK(hasCode(checkOk("sin('Steer'[deg])").diags, codes::kUnitScale));
    CHECK_FALSE(hasCode(checkOk("sin('Steer'[rad])").diags, codes::kUnitScale));
}

TEST_CASE("checker: W-UNIT-MISSING on dimensioned references without [unit] (03 section 2.4 rule 5)",
          "[unit]") {
    struct Case {
        const char* src;
        bool warns;
    };
    for (const Case& c : std::vector<Case>{{"'Speed' * 2", true},
                                           {"'Speed'[] * 2", true},
                                           {"'Throttle' > 0.5", true},
                                           {"'Brake Temp' > 100", true},
                                           {"'Wheelbase' * 2", true},
                                           {"'Gear' + 1", false},
                                           {"'Gain' * 'Speed'[m/s]", false},
                                           {"'Speed'[m/s] * 2", false},
                                           {"'Diff Ratio' * 2", false}}) {
        INFO(c.src);
        const Checked k = checkOk(c.src);
        CHECK(hasCode(k.diags, codes::kUnitMissing) == c.warns);
        if (c.warns) {
            CHECK(findCode(k.diags, codes::kUnitMissing)->level == Diagnostic::Warning);
            CHECK(findCode(k.diags, codes::kUnitMissing)->hint.find("[") != std::string::npos);
        }
    }
}

TEST_CASE("checker: [unit] of another family passes the numbers through with W-UNIT-FAMILY", "[unit]") {
    const Checked c = checkOk("'Speed'[deg C]");
    CHECK(hasCode(c.diags, codes::kUnitFamily));
    CHECK(c.type.unit == unitOf("m/s"));
    CHECK_FALSE(c.ast.nodes[c.ast.root].unit.has_value());
    // A unit spelling that resolves nowhere is an error with a suggestion.
    const Checked e = checkErr("'Speed'[kmh/h]", codes::kUnitUnknown);
    CHECK(findCode(e.diags, codes::kUnitUnknown)->hint.find("did you mean") != std::string::npos);
    // Case and spelling variants resolve within the channel's quantity ("g" here is the acceleration unit).
    CHECK(checkOk("'Lat G'[g]").type.unit == unitOf("G"));
    CHECK(checkOk("'Brake Temp'[degC]").type.unit == unitOf("deg C"));
}

TEST_CASE("checker: a unitless channel read in [ratio] or [rad] is read as stored without W-UNIT-FAMILY",
          "[unit]") {
    // AC-M2-01: generated table maths tags unitless channels with [ratio]; the numbers are the same either
    // way.
    for (const char* src :
         {"'Flag'[ratio]", "'Flag'[fraction]", "'Flag'[rad]", "'Flag'[ratio] * 'Gear'[ratio]"}) {
        const Checked c = checkOk(src);
        CHECK_FALSE(hasCode(c.diags, codes::kUnitFamily));
    }
    const Checked ratio = checkOk("'Flag'[ratio]");
    CHECK(ratio.type.unit == unitOf("ratio"));
    CHECK_FALSE(ratio.ast.nodes[ratio.ast.root].unit.has_value()); // no conversion in the plan
    CHECK(checkOk("sin('Flag'[rad])").type.dim == Dimension::none());
    // A unit of another factor still says that the numbers are not converted.
    CHECK(hasCode(checkOk("'Flag'[%]").diags, codes::kUnitFamily));
    CHECK(hasCode(checkOk("'Flag'[deg]").diags, codes::kUnitFamily));
    CHECK(hasCode(checkOk("'Flag'[m]").diags, codes::kUnitFamily));
}

TEST_CASE("checker: unresolved names are I-UNRESOLVED without data and E-UNKNOWN-CHANNEL with data",
          "[unit]") {
    const Scope dataless(FunctionRegistry::builtin(), *shippedUnits());
    Checked c = check("'Nope'[km/h] * 2 + 'Other'", dataless);
    REQUIRE(c.parsed);
    CHECK(countCode(c.diags, codes::kUnresolved) == 2);
    CHECK_FALSE(hasError(c.diags));
    CHECK(findCode(c.diags, codes::kUnresolved)->level == Diagnostic::Info);
    CHECK(c.type.kind != Kind::Invalid);
    CHECK_FALSE(dataless.dataBacked());
    CHECK(scope().dataBacked());

    checkErr("'Nope' * 2", codes::kUnknownChannel);
    // The lookup's own diagnostics are reported at the reference.
    const Checked amb = checkOk("'Shared' + 1");
    const Diagnostic* d = findCode(amb.diags, "W-CONST-AMBIGUOUS");
    REQUIRE(d != nullptr);
    CHECK(d->span.begin == 0);
    CHECK(d->span.end == 8);
}

TEST_CASE("checker: functions, arities and argument types", "[unit]") {
    const Checked unknown = checkErr("smoth('Speed'[m/s], 3)", codes::kUnknownFunction);
    CHECK(findCode(unknown.diags, codes::kUnknownFunction)->hint == "did you mean smooth()?");
    checkErr("map('Speed'[m/s])", codes::kUnknownFunction);

    const Checked arity = checkErr("smooth('Speed'[m/s])", codes::kArity);
    CHECK(findCode(arity.diags, codes::kArity)->message.find("takes 2 arguments") != std::string::npos);
    const Checked arity3 = checkErr("derivative('Speed'[m/s], 1, 2, 3)", codes::kArity);
    CHECK(findCode(arity3.diags, codes::kArity)->message.find("1, 2 or 3") != std::string::npos);
    checkErr("abs()", codes::kArity);
    checkErr("rand_val(1)", codes::kArity);

    // 03 section 2.5: derivative(1) is a wrong parameter type.
    checkErr("derivative(1)", codes::kArgType);
    checkErr("smooth(5, 3)", codes::kArgType);
    checkErr("smooth('Speed'[m/s], 'Speed'[m/s])", codes::kArgType);
    checkErr("'Driver' + 1", codes::kArgType);
    checkErr("'Driver' == 1", codes::kArgType);
    checkErr("range_is('Speed')", codes::kArgType);
    checkErr("edge_delay('Speed'[m/s] > 1, 1.0, 2)", codes::kArgType);
    checkErr("abs(rise)", codes::kArgType);
    checkErr("'Speed'[m/s] * fall", codes::kArgType);
    checkErr("rise", codes::kArgType);
    checkErr("\"Joe\"", codes::kArgType);
    checkErr("'Driver'", codes::kArgType);
    checkErr("filter_lp('Speed'[m/s], 'Speed'[m/s])", codes::kArgType);
    checkErr("smooth('Speed'[m/s], 'Dist'[m])", codes::kArgType);
    checkErr("time_shift('Speed'[m/s], 'Dist'[m])", codes::kArgType);
    checkErr("'Driver'[m]", codes::kArgType);

    checkErr("filter_lp('Speed'[m/s], 5, 4)", codes::kArgValue);
    checkErr("filter_cheby_lp('Speed'[m/s], 5, 3)", codes::kArgValue);
    checkErr("filter_lp('Speed'[m/s], 0)", codes::kArgValue);
    checkErr("smooth('Speed'[m/s], -1)", codes::kArgValue);
    checkErr("previous_sample('Speed'[m/s], 0)", codes::kArgValue);
    checkErr("previous_sample('Speed'[m/s], 1.5)", codes::kArgValue);
    checkErr("range_is(\"\")", codes::kArgValue);
    checkErr("filter_cheby_bp('Speed'[m/s], 1, 5, 4, -1)", codes::kArgValue);
    // An integral order beyond int's range is refused, not converted (UBSan, fuzz_maths_parser).
    checkErr("filter_cheby_bp('Speed'[m/s], 2, 5, + 2^-3^2^2 -3, 0.5)", codes::kArgValue);
    checkErr("filter_lp('Speed'[m/s], 5, 1e300)", codes::kArgValue);
    checkErr("filter_lp('Speed'[m/s], 5, -1e300)", codes::kArgValue);
    checkErr("edge_delay('Flag' > 0, -1.0, rise)", codes::kArgValue);

    for (const char* src : {"filter_lp('Speed'[m/s], 5)", "filter_lp('Speed'[m/s], 5, -2)",
                            "filter_hp('Speed'[m/s], 0.5, 3)", "filter_cheby_lp('Speed'[m/s], 5, -12, 0.5)",
                            "filter_cheby_bs('Speed'[m/s], 1, 5)", "filter_lp('Speed'[m/s], 5 [Hz] )"}) {
        INFO(src);
        Checked c = check(src, scope());
        if (c.parsed) {
            CHECK_FALSE(hasCode(c.diags, codes::kArgValue));
        }
    }
    checkOk("time_shift('Speed'[m/s], -0.5)"); // a negative shift moves data earlier
    checkOk("edge_delay('Throttle'[%] > 90.0, 1.0, RISE)");
    checkOk("previous_sample('Speed'[m/s], 2)");
}

TEST_CASE("checker: window arguments read points or seconds (03 section 2.4 rule 2)", "[unit]") {
    CHECK_FALSE(hasCode(checkOk("smooth('Speed'[m/s], 'Window')").diags, codes::kWindowArg));
    CHECK_FALSE(hasCode(checkOk("smooth('Speed'[m/s], 'Delay'[s])").diags, codes::kWindowArg));
    CHECK_FALSE(hasCode(checkOk("smooth('Speed'[m/s], 'Delay'[ms])").diags, codes::kWindowArg));
    const Checked half = checkOk("smooth('Speed'[m/s], 'Half')");
    CHECK(hasCode(half.diags, codes::kWindowArg));
    CHECK(findCode(half.diags, codes::kWindowArg)->level == Diagnostic::Warning);
    checkOk("smooth('Speed'[m/s], 0)");
    checkOk("smooth('Speed'[m/s], 1)");
    checkOk("smooth('Speed'[m/s], 0.2)");
    checkOk("derivative('Speed'[m/s], 5)");
    checkOk("derivative('Speed'[m/s], 0.1)");
}

TEST_CASE("checker: E-INTEGER-REQUIRED for the bitwise operators (the one hard type error)", "[unit]") {
    checkErr("'Speed'[m/s] & 1", codes::kIntegerRequired);
    checkErr("~'Speed'[m/s]", codes::kIntegerRequired);
    checkErr("1.5 | 1", codes::kIntegerRequired);
    checkErr("bit_and('Speed'[m/s], 1)", codes::kIntegerRequired);
    checkErr("bit_decode('Speed'[m/s], 3)", codes::kIntegerRequired);
    for (const char* src : {"'Gear' & 1", "int('Speed'[m/s]) | 1", "('Speed'[m/s] > 1) & ('Gear' > 2)",
                            "bit_decode('Status', 3)", "bit_decode('Status', 4, 2)",
                            "bit_xor('Status', 'Gear')", "~'Status'", "bit_not('Status')"}) {
        INFO(src);
        const Checked c = checkOk(src);
        CHECK(c.type.integer);
    }
}

TEST_CASE("checker: declared result unit, W-RESULT-DIM and I-RESULT-SCALE (03 section 3.2 rule 10)",
          "[unit]") {
    const Checked dim = checkOk("'Speed'[m/s]", "m");
    CHECK(hasCode(dim.diags, codes::kResultDim));
    CHECK(findCode(dim.diags, codes::kResultDim)->level == Diagnostic::Warning);
    CHECK_FALSE(hasCode(checkOk("derivative('Speed'[m/s])", "m/s/s").diags, codes::kResultDim));
    CHECK_FALSE(hasCode(checkOk("5", "m").diags, codes::kResultDim)); // a literal takes any unit

    const Checked scale = checkOk("'Speed'[km/h] + 'Speed'[km/h]", "m/s");
    const Diagnostic* d = findCode(scale.diags, codes::kResultScale);
    REQUIRE(d != nullptr);
    CHECK(d->level == Diagnostic::Info);
    CHECK(d->message.find("km/h") != std::string::npos);
    CHECK(d->hint == "declare the result unit km/h");
    const Checked g = checkOk("derivative('Speed'[m/s])", "G");
    REQUIRE(findCode(g.diags, codes::kResultScale) != nullptr);
    CHECK(findCode(g.diags, codes::kResultScale)->hint.find("m/s/s") != std::string::npos);
    CHECK_FALSE(hasCode(checkOk("'Speed'[km/h] * 2", "km/h").diags, codes::kResultScale));
}

TEST_CASE("checker: a literal factor is a conversion by hand: no I-RESULT-SCALE through it (D-185)",
          "[unit]") {
    // The usual hand conversions: the numbers are in the declared unit, so no hint to declare another one.
    for (const auto& [src, unit] :
         {std::pair{"'Speed'[km/h] / 3.6", "m/s"}, std::pair{"'Speed'[m/s] * 3.6", "km/h"},
          std::pair{"derivative('Speed'[m/s]) / 9.81", "G"}, std::pair{"'Speed'[km/h] * 2", "m/s"},
          std::pair{"1000 * 'Speed'[km/h]", "m/s"}}) {
        INFO(src << " declared " << unit);
        const Checked c = checkOk(src, unit);
        CHECK_FALSE(hasCode(c.diags, codes::kResultScale));
        CHECK_FALSE(hasCode(c.diags, codes::kResultDim));
        CHECK(std::isnan(c.type.scaleToBase));
    }
    // The dimension is still checked, and a factor of 1 (or -1) keeps the scale.
    CHECK(hasCode(checkOk("'Speed'[km/h] / 3.6", "m").diags, codes::kResultDim));
    CHECK(hasCode(checkOk("'Speed'[km/h] * 1", "m/s").diags, codes::kResultScale));
    CHECK(hasCode(checkOk("'Speed'[km/h] * -1", "m/s").diags, codes::kResultScale));
    // A unitless value scaled by a literal keeps its scale (nothing to convert).
    CHECK(checkOk("'Gear' * 2").type.scaleToBase == 1.0);
}

TEST_CASE("checker: text compares with text under all six operators, case-sensitively", "[unit]") {
    for (const char* src :
         {"'Driver' == \"Joe\"", "'Driver' != \"joe\"", "'Driver' < \"Kim\"", "'Driver' <= \"Joe\"",
          "'Driver' > \"Ann\"", "'Driver' >= \"Joe\"", "\"a\" < \"b\""}) {
        INFO(src);
        const Checked c = checkOk(src);
        CHECK(c.type.kind == Kind::Bool);
        CHECK_FALSE(c.type.rate.has_value());
    }
}

TEST_CASE("checker: choose unifies its branches; relational and logical results are Bool", "[unit]") {
    Checked c = checkOk("choose('Speed'[m/s] > 1, 'Speed'[m/s], invalid())");
    CHECK(c.type.kind == Kind::Series);
    CHECK(c.type.dim == dimOf("m/s"));
    c = checkOk("choose(1, 2, 3)");
    CHECK(c.type.kind == Kind::Scalar);
    CHECK(c.type.integer);
    c = checkOk("choose('Flag' > 0, 1, 0)");
    CHECK(c.type.kind == Kind::Series);
    c = checkOk("choose('Flag' > 0, 'Speed'[m/s] > 1, 'Gear' > 2)");
    CHECK(c.type.kind == Kind::Bool);
    const Checked inv = check("invalid()", scope());
    CHECK(inv.type.kind == Kind::Invalid);
    CHECK_FALSE(hasError(inv.diags));
    c = checkOk("'Speed'[m/s] > 1 && 'Gear' > 2");
    CHECK(c.type.kind == Kind::Bool);
    CHECK(c.type.rate == Rate::fromHz(100));
    CHECK(c.type.integer);
    c = checkOk("1 < 2");
    CHECK(c.type.kind == Kind::Bool);
    CHECK_FALSE(c.type.rate.has_value());
}

TEST_CASE("checker: statistics, LapScalar and the units of the calculus functions", "[unit]") {
    Checked c = checkOk("stat_mean('Speed'[m/s], 1, range_change(\"Outings:Laps\"))");
    CHECK(c.type.kind == Kind::LapScalar);
    CHECK(c.type.lapGroup == "Outings:Laps");
    CHECK(checkOk("stat_mean('Speed'[m/s])").type.kind == Kind::Scalar);
    CHECK(checkOk("stat_max('Speed'[m/s], 'Flag' > 0)").type.kind == Kind::Scalar);
    CHECK(checkOk("stat_mean('Speed'[m/s], 1, range_change(\"Outings:Laps\")) * 2").type.kind ==
          Kind::LapScalar);
    CHECK(checkOk("stat_mean('Speed'[m/s], 1, range_change(\"Outings:Laps\")) - 'Speed'[m/s]").type.kind ==
          Kind::Series);

    CHECK(checkOk("derivative('Speed'[m/s])").type.dim == dimOf("m/s/s"));
    CHECK(checkOk("derivative('Speed'[m/s], 'Dist'[m])").type.dim == Dimension::basis(Dimension::Time, -1));
    CHECK(checkOk("derivative('Speed'[km/h], 'Dist'[m], 3)").type.scaleToBase == Catch::Approx(3.6));
    CHECK(checkOk("integrate('Speed'[m/s])").type.dim == dimOf("m"));
    CHECK(checkOk("integrate_dist('Lat G'[m/s/s])").type.dim == dimOf("m/s").pow(2));
    CHECK(checkOk("pulse_width('Flag' > 0)").type.dim == dimOf("s"));
    CHECK(checkOk("atan2('Lat G'[m/s/s], 'Lat G'[m/s/s])").type.unit == unitOf("rad"));
    CHECK(checkOk("range_change(\"Outings:Laps\")").type.integer);
    CHECK(checkOk("range_is(\"Outings:Laps\")").type.kind == Kind::Bool);
    CHECK(checkOk("smooth('Speed'[km/h], 5)").type.unit == unitOf("km/h"));
    CHECK(checkOk("rand_chan()").type.kind == Kind::Series);
    CHECK(checkOk("rand_val()").type.kind == Kind::Scalar);
}

TEST_CASE("checker: the language's intrinsic signatures agree with the registry manifests", "[unit]") {
    const FunctionRegistry& reg = FunctionRegistry::builtin();
    std::size_t compared = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(IntrinsicId::Count); ++i) {
        const auto id = static_cast<IntrinsicId>(i);
        const lang::Signature& sig = lang::signatureOf(id);
        CHECK(sig.id == id);
        CHECK(sig.arityCount > 0);
        CHECK(lang::intrinsicByName(intrinsicName(id)) == id);
        const FunctionManifest* m = reg.find(intrinsicName(id));
        if (m == nullptr) {
            continue; // registerIntrinsics not complete yet (src/kernels)
        }
        INFO(intrinsicName(id));
        std::vector<std::uint8_t> arities(sig.arities.begin(), sig.arities.begin() + sig.arityCount);
        std::vector<std::uint8_t> manifest = m->arities;
        std::sort(manifest.begin(), manifest.end());
        CHECK(manifest == arities);
        ++compared;
    }
    if (compared == 0) {
        WARN("no intrinsic manifest registered yet: registerIntrinsics is still an M2 stub");
    }
}
