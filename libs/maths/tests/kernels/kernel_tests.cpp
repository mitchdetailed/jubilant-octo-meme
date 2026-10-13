// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The intrinsic catalogue and kernels of src/kernels (03 sections 5, 6.1, 10.1; D-022, D-024): one manifest
// per intrinsic with the language's arities, argument kinds and unit rules; makeIntrinsicKernel's kernel
// class per call and its refusals; failures known only at evaluation time (Kernel::status: Nyquist, unknown
// range path); compile()'s Corr Dist input of integrate_dist (E-NO-DISTANCE without it); the Table kernel's
// refusals. Value semantics are the case corpus (case_corpus_tests.cpp) and the property tests
// (kernel_property_tests.cpp).

#include "../lang/lang_test_support.hpp"
#include "kernel_test_support.hpp"
#include "kernels/kernels_internal.hpp"
#include "lang/signatures.hpp"

#include <esses/maths/function_registry.hpp>
#include <esses/maths/intrinsics.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

TEST_CASE("kernels: every intrinsic has a manifest that agrees with the language", "[unit]") {
    const FunctionRegistry& reg = FunctionRegistry::builtin();
    REQUIRE(reg.all().size() == static_cast<std::size_t>(IntrinsicId::Count));
    std::set<std::string> names;
    for (std::size_t i = 0; i < static_cast<std::size_t>(IntrinsicId::Count); ++i) {
        const auto id = static_cast<IntrinsicId>(i);
        INFO(intrinsicName(id));
        const FunctionManifest* m = reg.find(intrinsicName(id));
        REQUIRE(m != nullptr);
        CHECK(m->name == intrinsicName(id));
        CHECK(names.insert(m->name).second);
        CHECK(reg.intrinsicId(m->name) == id);
        CHECK(reg.external(m->name) == nullptr);
        CHECK(m->provenance == "intrinsic");
        CHECK_FALSE(m->description.empty());
        CHECK(m->cls == intrinsicClass(id));
        CHECK(m->blockCapable == (m->cls == KernelClass::Elementwise || m->cls == KernelClass::Range));
        CHECK_FALSE(m->telemetry);
        const lang::Signature& sig = lang::signatureOf(id);
        REQUIRE(m->args.size() == sig.paramCount);
        const std::uint8_t minArity = *std::min_element(m->arities.begin(), m->arities.end());
        for (std::size_t k = 0; k < m->args.size(); ++k) {
            CHECK(m->args[k].name == sig.params[k].name);
            CHECK(m->args[k].optional == (k >= minArity));
        }
        CHECK_FALSE(m->returns.unitRule.empty());
    }
    // Spot checks of the unit-rule vocabulary (03 section 3.2 rule 9) and argument kinds.
    CHECK(reg.find("derivative")->returns.unitRule == "per_time:x");
    CHECK(reg.find("integrate")->returns.unitRule == "times_time:x");
    CHECK(reg.find("integrate_dist")->returns.unitRule == "times_length:x");
    CHECK(reg.find("atan2")->returns.unitRule == "angle");
    CHECK(reg.find("hypot")->returns.unitRule == "unify:x,y");
    CHECK(reg.find("choose")->returns.unitRule == "unify:a,b");
    CHECK(reg.find("sqrt")->returns.unitRule == "sqrt:x");
    CHECK(reg.find("pulse_width")->returns.unitRule == "declared:s");
    CHECK(reg.find("rand_val")->returns.kind == ArgSpec::Kind::Scalar);
    CHECK(reg.find("range_is")->args.at(0).kind == ArgSpec::Kind::RangeRef);
    CHECK(reg.find("edge_delay")->args.at(2).kind == ArgSpec::Kind::Keyword);
    CHECK(reg.find("edge_delay")->args.at(1).dim == "time");
    CHECK(reg.find("filter_lp")->args.at(1).dim == "frequency");
    CHECK(reg.find("filter_cheby_bp")->arities == std::vector<std::uint8_t>{3, 4, 5});
    CHECK(reg.find("derivative")->arities == std::vector<std::uint8_t>{1, 2, 3});
    // The case-folded lookup the checker uses.
    CHECK(reg.find("smooth") == reg.find(caseFold("SMOOTH")));
}

TEST_CASE("kernels: each non-elementwise call compiles to a stage of its kernel class", "[unit]") {
    KernelFixture f;
    f.add("X", {1, 2, 3, 4, 5, 6, 7, 8}, Rate::fromHz(10));
    f.add("Corr Dist", {0, 1, 2, 3, 4, 5, 6, 7}, Rate::fromHz(10), "m");
    f.ranges().setGroup(model::RangeGroup::make("Outings"));
    const struct {
        const char* expr;
        KernelClass cls;
    } rows[] = {
        {"smooth('X', 3)", KernelClass::Windowed},
        {"derivative('X')", KernelClass::Windowed},
        {"filter_lp('X', 1)", KernelClass::IIR},
        {"filter_cheby_bs('X', 1, 2)", KernelClass::IIR},
        {"integrate('X')", KernelClass::Stateful},
        {"integrate_dist('X')", KernelClass::Stateful},
        {"stat_mean('X')", KernelClass::Stateful},
        {"running_max('X')", KernelClass::Stateful},
        {"flip_flop('X' > 2, 'X' > 5)", KernelClass::Stateful},
        {"edge_delay('X' > 2, 0.1, fall)", KernelClass::Stateful},
        {"time_valid('X' > 2, 0.1)", KernelClass::Stateful},
        {"time_shift('X', 0.1)", KernelClass::Stateful},
        {"pulse_width('X' > 2)", KernelClass::Stateful},
        {"previous_sample('X', 2)", KernelClass::Stateful},
        {"range_is(\"Outings\")", KernelClass::Range},
        {"range_change(\"Outings\")", KernelClass::Range},
        {"variance_time('X')", KernelClass::Variance},
        {"variance_dist('X')", KernelClass::Variance},
    };
    for (const auto& row : rows) {
        INFO(row.expr);
        KernelFixture::Outcome o = f.eval(row.expr);
        REQUIRE(o.plan.has_value());
        REQUIRE_FALSE(o.plan->stages.empty());
        CHECK(o.plan->stages.back().kernel->cls() == row.cls);
        CHECK(o.signal.ok());
    }
    // integrate_dist reads Corr Dist as its last stage input (Linear, F64), so the node version follows it.
    KernelFixture::Outcome o = f.eval("integrate_dist('X')");
    REQUIRE(o.plan.has_value());
    REQUIRE(o.plan->stages.back().inputs.size() == 2);
    const Stage::Ref dist = o.plan->stages.back().inputs.back();
    REQUIRE(dist.from == Stage::Ref::From::Input);
    CHECK(o.plan->inputs.at(dist.index).key.nameCf == "corr dist");
    CHECK(o.plan->inputs.at(dist.index).interp == store::Interp::Linear);
    CHECK(o.plan->outDtype == store::Dtype::F64); // an integrator (03 section 6.5)
    // Ungated statistics and variance are SelectionDependent; a statistic with reset is not.
    CHECK(f.eval("stat_mean('X')").plan->selectionDependent);
    CHECK(f.eval("variance_time('X')").plan->selectionDependent);
    CHECK_FALSE(f.eval("stat_mean('X', 1, range_change(\"Outings\"))").plan->selectionDependent);
    CHECK(f.eval("range_is(\"Outings\")").plan->rangeGroups == std::vector<std::string>{"Outings"});
}

TEST_CASE("kernels: makeIntrinsicKernel refuses elementwise ids and non-call nodes", "[unit]") {
    const Ast ast = parseOk("abs('X')");
    Result<std::unique_ptr<Kernel>> ew = makeIntrinsicKernel(IntrinsicId::Abs, ast, ast.root);
    REQUIRE_FALSE(ew.ok());
    CHECK(ew.error().code == ErrorCode::Unsupported);
    const Ast lit = parseOk("2");
    Result<std::unique_ptr<Kernel>> notCall = makeIntrinsicKernel(IntrinsicId::Smooth, lit, lit.root);
    REQUIRE_FALSE(notCall.ok());
    CHECK(notCall.error().code == ErrorCode::Invalid);
    // A filter order outside the catalogue (the checker refuses it first; the factory too).
    const Ast bad = parseOk("filter_lp('X', 5, 7)");
    Result<std::unique_ptr<Kernel>> order = makeIntrinsicKernel(IntrinsicId::FilterLp, bad, bad.root);
    REQUIRE_FALSE(order.ok());
    CHECK(order.error().context.detail == "E-ARG-VALUE");
}

TEST_CASE("kernels: failures known only at evaluation time come through Kernel::status", "[unit]") {
    KernelFixture f;
    f.add("X", {1, 2, 3, 4, 5, 6, 7, 8}, Rate::fromHz(10));
    f.ranges().setGroup(model::RangeGroup::make("Outings"));
    f.ranges().setGroup(model::RangeGroup::make("Outings:Laps"));
    // 5 Hz is half of 10 Hz: E-ARG-VALUE when the kernel learns the result rate.
    KernelFixture::Outcome nyquist = f.eval("filter_lp('X', 5)");
    REQUIRE(nyquist.plan.has_value());
    REQUIRE_FALSE(nyquist.signal.ok());
    CHECK(nyquist.signal.error().context.detail == "E-ARG-VALUE");
    CHECK(nyquist.signal.error().message.find("half the result rate") != std::string::npos);
    CHECK(f.eval("filter_lp('X', 4.9)").signal.ok());
    // An unknown range path lists the paths the source has.
    KernelFixture::Outcome unknown = f.eval("range_is(\"Outings:Stints\")");
    REQUIRE_FALSE(unknown.signal.ok());
    CHECK(unknown.signal.error().context.detail == "E-RANGE-UNKNOWN");
    CHECK(unknown.signal.error().message.find(R"("Outings:Laps")") != std::string::npos);
    // An empty group exists: no error, never inside a range.
    const std::vector<double> v = f.values("range_is(\"Outings:Laps\") + range_change(\"Outings:Laps\")");
    CHECK(std::all_of(v.begin(), v.end(), [](double x) { return x == 0.0; }));
}

TEST_CASE("kernels: integrate_dist without Corr Dist is E-NO-DISTANCE at compile time", "[unit]") {
    KernelFixture f;
    f.add("X", {1, 2, 3, 4}, Rate::fromHz(10));
    KernelFixture::Outcome o = f.eval("integrate_dist('X', 'X' > 1)");
    REQUIRE_FALSE(o.signal.ok());
    CHECK_FALSE(o.plan.has_value());
    CHECK(o.signal.error().context.detail == "E-NO-DISTANCE");
    CHECK(hasCode(o.diags, "E-NO-DISTANCE"));
}

TEST_CASE("kernels: the Table kernel refuses unusable tables", "[unit]") {
    const units::UnitRegistry& units = *shippedUnits();
    TableSpec t;
    t.x.breakpoints = {0, 10};
    t.values = {1, 2};
    CHECK(kern::makeTableKernel(t, units).ok());
    const auto code = [&](const TableSpec& spec) {
        Result<std::unique_ptr<Kernel>> k = kern::makeTableKernel(spec, units);
        return k.ok() ? std::string("ok") : k.error().context.detail.value_or("");
    };
    TableSpec one = t;
    one.x.breakpoints = {0};
    one.values = {1};
    CHECK(code(one) == "E-TABLE-AXIS");
    TableSpec flat = t;
    flat.x.breakpoints = {0, 0};
    CHECK(code(flat) == "E-TABLE-AXIS");
    TableSpec count = t;
    count.values = {1, 2, 3};
    CHECK(code(count) == "E-TABLE-AXIS");
    TableSpec nonFinite = t;
    nonFinite.values = {1, kNaN};
    CHECK(code(nonFinite) == "E-TABLE-AXIS");
    TableSpec threeD = t;
    threeD.y = TableSpec::Axis{};
    CHECK(code(threeD) == "E-KIND-UNSUPPORTED");
    Result<std::unique_ptr<Kernel>> k = kern::makeTableKernel(t, units);
    REQUIRE(k.ok());
    CHECK((*k)->cls() == KernelClass::Table);
}

TEST_CASE("kernels: kernel classes report their extent and halo", "[unit]") {
    KernelFixture f;
    f.add("X", {1, 2, 3, 4, 5, 6, 7, 8}, Rate::fromHz(10));
    const auto last = [&](std::string_view expr) -> const Kernel& {
        static std::vector<Plan> keep; // the kernels live in their plans
        KernelFixture::Outcome o = f.eval(expr);
        REQUIRE(o.plan.has_value());
        keep.push_back(std::move(*o.plan));
        return *keep.back().stages.back().kernel;
    };
    CHECK(last("smooth('X', 5)").halo() == 2);
    CHECK(last("smooth('X', 4)").halo() == 2);
    CHECK(last("smooth('X', 0.5)").halo() == 0); // seconds: known at the result rate
    CHECK(last("smooth('X', 5)").extent() == ExtentMode::Whole);
    CHECK(last("filter_lp('X', 1, -2)").extent() == ExtentMode::Whole);
    CHECK(last("integrate('X')").extent() == ExtentMode::Whole);
    CHECK(last("smooth('X', 7)").cls() == KernelClass::Windowed);
}

namespace {

std::string kindName(ArgSpec::Kind k) {
    switch (k) {
    case ArgSpec::Kind::Scalar:
        return "Scalar";
    case ArgSpec::Kind::Series:
        return "Series";
    case ArgSpec::Kind::Text:
        return "Text";
    case ArgSpec::Kind::RangeRef:
        return "RangeRef";
    case ArgSpec::Kind::Keyword:
        return "Keyword";
    }
    return "?";
}

std::string className(KernelClass c) {
    switch (c) {
    case KernelClass::Elementwise:
        return "Elementwise";
    case KernelClass::Windowed:
        return "Windowed";
    case KernelClass::IIR:
        return "IIR";
    case KernelClass::Stateful:
        return "Stateful";
    case KernelClass::Range:
        return "Range";
    case KernelClass::Variance:
        return "Variance";
    case KernelClass::Table:
        return "Table";
    case KernelClass::External:
        return "External";
    }
    return "?";
}

} // namespace

TEST_CASE("kernels: every intrinsic manifest is a valid mathfn-manifest-1 document", "[unit]") {
    std::ifstream in(repoRoot() / "schemas" / "mathfn-manifest-1.schema.json", std::ios::binary);
    REQUIRE(in.good());
    const nlohmann::json schema = nlohmann::json::parse(in);
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    for (const FunctionManifest* m : FunctionRegistry::builtin().all()) {
        nlohmann::json args = nlohmann::json::array();
        for (const ArgSpec& a : m->args) {
            args.push_back(
                {{"name", a.name}, {"kind", kindName(a.kind)}, {"dim", a.dim}, {"optional", a.optional}});
        }
        const nlohmann::json doc = {
            {"$schema", "urn:esses:mathfn-manifest-1"},
            {"schemaVersion", 1},
            {"name", m->name},
            {"description", m->description},
            {"args", args},
            {"arities", m->arities},
            {"returns", {{"kind", kindName(m->returns.kind)}, {"unitRule", m->returns.unitRule}}},
            {"class", className(m->cls)},
            {"blockCapable", m->blockCapable},
            {"telemetry", m->telemetry},
            {"provenance", m->provenance}};
        INFO(doc.dump());
        std::string error;
        try {
            validator.validate(doc);
        } catch (const std::exception& e) {
            error = e.what();
        }
        CHECK(error.empty());
    }
}
